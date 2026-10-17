/* Copyright (c) 2026 Bernhard Haumacher */
// The Overview: the dashboard of the deployed image, its rooms as sections (in the image's location
// order, under their floor's name) and each function a tile by kind (tiles.js); functions without a
// room under "Elsewhere". Without an image, or in a build without images, every datapoint is a
// plain tile with its live value (a writable boolean toggles on a tap) and a hint says why.

import { api, errorText } from "/api.js";
import { h, icon, icons, replace } from "/dom.js";
import { formatValue } from "/format.js";
import { t } from "/i18n.js";
import { label, loadModel } from "/model.js";
import { functionTile } from "/tiles.js";
import { openSheet } from "/sheet.js";

const SPARK_REFRESH_MS = 5 * 60 * 1000;
const AGE_REFRESH_MS = 30 * 1000;

function iconFor(dp) {
	switch (dp.type) {
		case "boolean":
			return dp.writable ? icons.bulb : icons.toggle;
		case "string":
			return icons.text;
		case "time of day":
		case "date":
		case "date-time":
			return icons.clock;
		case "color":
			return icons.palette;
		case "unsigned":
		case "signed":
		case "real":
			return icons.gauge;
		default:
			return icons.data;
	}
}

/// Every datapoint as a tile (no image, or an image without functions).
function flatTiles(live, show) {
	const grid = h("div", { class: "tiles" });
	const tiles = new Map();
	const pending = new Set();

	function state(dp) {
		const text = formatValue(dp);
		if (dp.type === "color" && dp.value) {
			return [h("span", { class: "swatch", style: `background:${text}` }), text];
		}
		return dp.quality === "stale" ? `${text} · ${t("value.stale")}` : text;
	}

	function update(tile, dp) {
		tile.classList.toggle("on", dp.type === "boolean" && dp.value === true && dp.quality !== "unknown");
		tile.classList.toggle("stale", dp.quality === "stale");
		tile.classList.toggle("unknown", dp.quality === "unknown");
		tile.classList.toggle("pending", pending.has(dp.id));
		replace(tile.querySelector(".tile-state"), state(dp));
		if (dp.type === "boolean" && dp.writable) {
			tile.setAttribute("aria-pressed", dp.value === true ? "true" : "false");
		}
	}

	function build(dp) {
		const toggle = dp.type === "boolean" && dp.writable;
		const tile = h(toggle ? "button" : "div", {
			class: "tile flat",
			type: toggle ? "button" : undefined,
			title: dp.name,
			onclick: toggle ? async () => {
				const current = live.datapoints.get(dp.id);
				if (!current || pending.has(dp.id)) {
					return;
				}
				pending.add(dp.id);
				update(tile, current);
				try {
					await live.command(dp.id, !(current.value === true));
				} catch (error) {
					show(`${current.name}: ${errorText(error)}`);
				} finally {
					pending.delete(dp.id);
					update(tile, live.datapoints.get(dp.id) || current);
				}
			} : undefined,
		},
		h("span", { class: "tile-icon" }, icon(iconFor(dp))),
		h("span", { class: "tile-text" }, h("span", { class: "tile-name" }, dp.name), h("span", { class: "tile-state" })));
		update(tile, dp);
		return tile;
	}

	function render() {
		tiles.clear();
		const list = [...live.datapoints.values()].sort((a, b) => a.id - b.id);
		if (list.length === 0) {
			grid.classList.remove("tiles");
			replace(grid, h("div", { class: "empty" }, live.snapshotDone ? t("overview.empty") : t("overview.waiting")));
			return;
		}
		grid.classList.add("tiles");
		replace(grid, list.map((dp) => {
			const tile = build(dp);
			tiles.set(dp.id, tile);
			return tile;
		}));
	}

	render();
	return {
		element: grid,
		datapoints(detail) {
			if (detail.all) {
				render();
				return;
			}
			for (const id of detail.ids) {
				const tile = tiles.get(id);
				const dp = live.datapoints.get(id);
				if (tile && dp) {
					update(tile, dp);
				}
			}
		},
		tick() {},
	};
}

/// The rooms and their function tiles.
function dashboard(model, live, show) {
	const histories = new Map(); // id -> Promise of {points, from, to}
	const fetchHistory = (id) => {
		const to = Math.floor(Date.now() / 1000) + 1;
		const from = to - 86400;
		return api("GET", `/api/history?ids=${id}&from=${from}&to=${to}&points=48`).then((data) => {
			const series = data.series[0];
			return series && series.points ? { points: series.points, from, to } : null;
		});
	};
	const env = {
		model,
		live,
		open: (fn) => openSheet(fn, env),
		history: (id) => {
			if (!histories.has(id)) {
				histories.set(id, fetchHistory(id));
			}
			return histories.get(id);
		},
	};
	const tiles = [];
	const byId = new Map();
	const sections = new Map(); // room id ("" for none) -> functions
	for (const fn of model.functions) {
		const room = fn.room && model.locationsById.has(fn.room) ? fn.room : "";
		if (!sections.has(room)) {
			sections.set(room, []);
		}
		sections.get(room).push(fn);
	}
	const order = model.locations.map((l) => l.id).filter((id) => sections.has(id));
	if (sections.has("")) {
		order.push("");
	}
	const element = h("div", { class: "rooms" }, order.map((room) => {
		const location = model.locationsById.get(room);
		const parent = location && location.parent ? model.locationsById.get(location.parent) : null;
		const headingId = `room-${room || "elsewhere"}`;
		return h("section", { class: "room", "aria-labelledby": headingId },
			h("h2", { class: "section-title", id: headingId },
				location ? label(location.label, room) : t("overview.elsewhere"),
				parent ? h("span", { class: "section-sub" }, label(parent.label, parent.id)) : null),
			h("div", { class: "tiles" }, sections.get(room).map((fn) => {
				const tile = functionTile(fn, env);
				tiles.push(tile);
				for (const id of tile.ids) {
					if (!byId.has(id)) {
						byId.set(id, []);
					}
					byId.get(id).push(tile);
				}
				return tile.element;
			})));
	}));
	return {
		element,
		datapoints(detail) {
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
		},
		tick(sparks) {
			tiles.forEach((tile) => tile.update());
			if (sparks) {
				histories.clear();
				for (const tile of tiles) {
					if (tile.sparkId !== null) {
						env.history(tile.sparkId).then((data) => tile.refreshSpark(data)).catch(() => {});
					}
				}
			}
		},
	};
}

export function renderOverview(content, ctx) {
	const live = ctx.live;
	const message = h("p", { class: "message error", role: "alert", hidden: true });
	const hint = h("p", { class: "muted hint-line", hidden: true });
	const body = h("div", null, h("p", { class: "muted" }, t("app.loading")));
	let view = null;
	let model = null;
	let messageTimer = null;
	let loading = false;
	let ageTimer = null;
	let sparkTimer = null;

	const show = (text) => {
		message.textContent = text;
		message.hidden = !text;
		clearTimeout(messageTimer);
		if (text) {
			messageTimer = setTimeout(() => show(""), 8000);
		}
	};

	function flat(hintKey, params) {
		hint.textContent = t(hintKey, params);
		hint.hidden = false;
		view = flatTiles(live, show);
		replace(body, h("h2", { class: "section-title" }, t("overview.datapoints")), view.element);
	}

	async function load() {
		if (loading) {
			return;
		}
		loading = true;
		try {
			model = await loadModel();
			if (!model.loaded) {
				flat("overview.no_image");
			} else if (model.functions.length === 0) {
				flat("overview.no_functions", { name: model.name || model.project, revision: model.revision });
			} else {
				hint.hidden = true;
				view = dashboard(model, live, show);
				replace(body, view.element);
			}
		} catch (error) {
			model = null;
			if (error && error.code === "image.unavailable") {
				flat("overview.no_image_service");
			} else {
				show(errorText(error));
				flat("overview.no_model");
			}
		} finally {
			loading = false;
		}
	}

	const onDatapoints = (event) => view && view.datapoints(event.detail);
	const onStatus = (event) => {
		const image = event.detail.image;
		if (!image || loading) {
			return;
		}
		const shown = model && model.loaded ? `${model.project}/${model.revision}` : "";
		const now = image.loaded ? `${image.project}/${image.revision}` : "";
		if (model !== null && shown !== now) {
			load(); // another image was deployed or it was removed
		}
	};
	const onError = (event) => show(errorText({ code: event.detail.code, reason: event.detail.error }));
	live.addEventListener("datapoints", onDatapoints);
	live.addEventListener("status", onStatus);
	live.addEventListener("error", onError);
	ageTimer = setInterval(() => view && view.tick(false), AGE_REFRESH_MS);
	sparkTimer = setInterval(() => view && view.tick(true), SPARK_REFRESH_MS);

	content.append(message, hint, body);
	load();

	return () => {
		live.removeEventListener("datapoints", onDatapoints);
		live.removeEventListener("status", onStatus);
		live.removeEventListener("error", onError);
		clearTimeout(messageTimer);
		clearInterval(ageTimer);
		clearInterval(sparkTimer);
	};
}
