/* Copyright (c) 2026 Bernhard Haumacher */
// The control pages E-Home deploys with the image (spec/runtime-image.md 3.9, GET /api/pages): the
// navigation entry of each, and a page as sections of tiles in a grid: a function as its tile by
// kind (tiles.js, with its detail sheet), a datapoint as a value tile with the control its type
// chooses, a chart as a wide tile (elements.js). Without pages the generated overview stays the
// start page (overview.js).

import { api, errorText } from "/api.js";
import { h, replace } from "/dom.js";
import { t } from "/i18n.js";
import { label, loadModel, pages as fetchPages } from "/model.js";
import { functionTile } from "/tiles.js";
import { openSheet } from "/sheet.js";
import { valueTile, chartTile, CHART_REFRESH_MS } from "/elements.js";

const AGE_REFRESH_MS = 30 * 1000;
const SPARK_REFRESH_MS = 5 * 60 * 1000;

/// The pages, assembled from the five lists of GET /api/pages: [{id, label, icon, sections:
/// [{label, elements: [{function | datapoint, label, readOnly} | {chart: {..., series, references}}]}]}].
/// Empty without an image, without pages or in a build without images.
export async function loadPages() {
	for (let attempt = 0; attempt < 3; attempt++) {
		const lists = { pages: [], sections: [], elements: [], series: [], references: [] };
		let generation = null;
		let restart = false;
		try {
			await fetchPages("/api/pages", (page) => {
				if (generation !== null && page.generation !== generation) {
					restart = true;
				}
				generation = page.generation;
				for (const name of Object.keys(lists)) {
					lists[name].push(...(page[name] || []));
				}
			});
		} catch (error) {
			if (error && (error.code === "image.unavailable" || error.status === 404)) {
				return [];
			}
			throw error;
		}
		if (restart) {
			continue;
		}
		const pages = lists.pages.map((p) => ({ id: p.id, label: p.label, icon: p.icon, sections: [] }));
		const sections = lists.sections.map((s) => {
			const section = { label: s.label, elements: [] };
			if (pages[s.page]) {
				pages[s.page].sections.push(section);
			}
			return section;
		});
		const elements = lists.elements.map((e) => {
			const element = { ...e };
			if (element.chart) {
				element.chart = { ...element.chart, series: [], references: [] };
			}
			if (sections[e.section]) {
				sections[e.section].elements.push(element);
			}
			return element;
		});
		for (const s of lists.series) {
			const element = elements[s.element];
			if (element && element.chart) {
				element.chart.series.push(s);
			}
		}
		for (const r of lists.references) {
			const element = elements[r.element];
			if (element && element.chart) {
				element.chart.references.push(r);
			}
		}
		return pages;
	}
	throw { code: "model.changing" };
}

/// Renders `page` into `content`; returns the cleanup.
export function renderPage(content, ctx, page) {
	const live = ctx.live;
	const message = h("p", { class: "message error", role: "alert", hidden: true });
	const body = h("div", null, h("p", { class: "muted" }, t("app.loading")));
	let tiles = [];
	let byId = new Map();
	let messageTimer = null;
	let resize = null;
	let disposed = false;

	const show = (text) => {
		message.textContent = text;
		message.hidden = !text;
		clearTimeout(messageTimer);
		if (text) {
			messageTimer = setTimeout(() => show(""), 8000);
		}
	};

	function build(model) {
		const histories = new Map();
		const env = {
			model,
			live,
			open: (fn) => openSheet(fn, env),
			history: (id) => {
				if (!histories.has(id)) {
					const to = Math.floor(Date.now() / 1000) + 1;
					const from = to - 86400;
					histories.set(id, api("GET", `/api/history?ids=${id}&from=${from}&to=${to}&points=48`).then((data) => {
						const series = data.series[0];
						return series && series.points ? { points: series.points, from, to } : null;
					}));
				}
				return histories.get(id);
			},
			clearHistories: () => histories.clear(),
		};
		const functions = new Map(model.functions.map((fn) => [fn.id, fn]));
		tiles = [];
		byId = new Map();
		const grids = [];
		const element = h("div", { class: "rooms page" }, page.sections.map((section, s) => {
			const title = section.label !== undefined ? label(section.label, "") : "";
			const grid = h("div", { class: "tiles" }, section.elements.map((el) => {
				let tile = null;
				if (el.function !== undefined) {
					const fn = functions.get(el.function);
					if (!fn) {
						return h("div", { class: "tile unknown" }, t("page.unknown_function", { id: el.function }));
					}
					tile = functionTile(el.label !== undefined ? { ...fn, label: el.label } : fn, env);
				} else if (el.datapoint !== undefined) {
					tile = valueTile(el, env);
				} else if (el.chart) {
					tile = chartTile(el, env);
				}
				if (!tile) {
					return null;
				}
				tiles.push(tile);
				for (const id of tile.ids) {
					if (!byId.has(id)) {
						byId.set(id, []);
					}
					byId.get(id).push(tile);
				}
				return tile.element;
			}));
			grids.push(grid);
			const headingId = `section-${s}`;
			return h("section", { class: "room", "aria-labelledby": title ? headingId : undefined },
				title ? h("h2", { class: "section-title", id: headingId }, title) : null, grid);
		}));
		// A chart's width in columns is capped at the columns the grid has on this screen.
		const layout = () => {
			grids.forEach((grid) => {
				const columns = getComputedStyle(grid).gridTemplateColumns.split(" ").filter(Boolean).length || 1;
				for (const tile of tiles) {
					if (tile.layout && grid.contains(tile.element)) {
						tile.layout(columns);
					}
				}
			});
		};
		if (typeof ResizeObserver === "function") {
			resize = new ResizeObserver(layout);
			grids.forEach((grid) => resize.observe(grid));
		}
		replace(body, page.sections.length ? element : h("div", { class: "empty" }, t("page.empty")));
		layout();
	}

	async function load() {
		try {
			const model = await loadModel();
			if (disposed) {
				return;
			}
			if (!model.loaded) {
				replace(body, h("div", { class: "empty" }, t("overview.no_image")));
				return;
			}
			build(model);
		} catch (error) {
			show(errorText(error));
			replace(body, h("div", { class: "empty" }, t("overview.no_model")));
		}
	}

	const onDatapoints = (event) => {
		const detail = event.detail;
		if (detail.all) {
			tiles.forEach((tile) => tile.update());
			return;
		}
		const touched = new Set();
		for (const id of detail.ids) {
			for (const tile of byId.get(id) || []) {
				touched.add(tile);
			}
		}
		touched.forEach((tile) => tile.update());
	};
	const onError = (event) => show(errorText({ code: event.detail.code, reason: event.detail.error }));
	live.addEventListener("datapoints", onDatapoints);
	live.addEventListener("error", onError);
	const ageTimer = setInterval(() => tiles.forEach((tile) => tile.update()), AGE_REFRESH_MS);
	const sparkTimer = setInterval(() => {
		for (const tile of tiles) {
			if (tile.sparkId !== null && tile.sparkId !== undefined) {
				api("GET", `/api/history?ids=${tile.sparkId}&points=48`).then((data) => {
					const series = data.series[0];
					if (series && series.points) {
						tile.refreshSpark({ points: series.points, from: data.from, to: data.to });
					}
				}).catch(() => {});
			}
		}
	}, SPARK_REFRESH_MS);
	const chartTimer = setInterval(() => tiles.forEach((tile) => tile.tick && tile.tick(true)), CHART_REFRESH_MS);

	content.append(message, body);
	load();

	return () => {
		disposed = true;
		live.removeEventListener("datapoints", onDatapoints);
		live.removeEventListener("error", onError);
		clearTimeout(messageTimer);
		clearInterval(ageTimer);
		clearInterval(sparkTimer);
		clearInterval(chartTimer);
		if (resize) {
			resize.disconnect();
		}
		tiles.forEach((tile) => tile.dispose && tile.dispose());
	};
}
