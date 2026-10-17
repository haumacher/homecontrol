/* Copyright (c) 2026 Bernhard Haumacher */
// The Overview: for now every datapoint as a tile with its live value; a writable boolean toggles on
// a tap. The dashboard of rooms and functions follows with issue #14.

import { errorText } from "/api.js";
import { h, icon, icons, replace } from "/dom.js";
import { formatValue } from "/format.js";
import { t } from "/i18n.js";

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

export function renderOverview(content, ctx) {
	const live = ctx.live;
	const message = h("p", { class: "message error", role: "alert", hidden: true });
	const grid = h("div", { class: "tiles" });
	const tiles = new Map();
	const pending = new Set();
	let messageTimer = null;

	const show = (text) => {
		message.textContent = text;
		message.hidden = !text;
		clearTimeout(messageTimer);
		if (text) {
			messageTimer = setTimeout(() => show(""), 6000);
		}
	};

	function state(dp) {
		const text = formatValue(dp);
		if (dp.type === "color" && dp.value) {
			return [h("span", { class: "swatch", style: `background:${text}` }), text];
		}
		return dp.quality === "stale" ? `${text} · ${t("value.stale")}` : text;
	}

	function update(tile, dp) {
		const toggle = dp.type === "boolean" && dp.writable;
		tile.className = "tile";
		tile.classList.toggle("on", dp.type === "boolean" && dp.value === true && dp.quality !== "unknown");
		tile.classList.toggle("stale", dp.quality === "stale");
		tile.classList.toggle("unknown", dp.quality === "unknown");
		tile.classList.toggle("pending", pending.has(dp.id));
		replace(tile.querySelector(".tile-state"), state(dp));
		if (toggle) {
			tile.setAttribute("aria-pressed", dp.value === true ? "true" : "false");
		}
	}

	function build(dp) {
		const toggle = dp.type === "boolean" && dp.writable;
		const tile = h(
			toggle ? "button" : "div",
			{
				type: toggle ? "button" : undefined,
				title: dp.name,
				onclick: toggle
					? async () => {
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
					}
					: undefined,
			},
			h("span", { class: "tile-icon" }, icon(iconFor(dp))),
			h("span", { class: "tile-text" }, h("span", { class: "tile-name" }, dp.name), h("span", { class: "tile-state" })),
		);
		update(tile, dp);
		return tile;
	}

	function renderAll() {
		tiles.clear();
		const list = [...live.datapoints.values()].sort((a, b) => a.id - b.id);
		if (list.length === 0) {
			replace(grid, h("div", { class: "empty" }, live.snapshotDone ? t("overview.empty") : t("overview.waiting")));
			grid.classList.remove("tiles");
			return;
		}
		grid.classList.add("tiles");
		replace(grid, list.map((dp) => {
			const tile = build(dp);
			tiles.set(dp.id, tile);
			return tile;
		}));
	}

	const onDatapoints = (event) => {
		if (event.detail.all) {
			renderAll();
			return;
		}
		for (const id of event.detail.ids) {
			const tile = tiles.get(id);
			const dp = live.datapoints.get(id);
			if (tile && dp) {
				update(tile, dp);
			}
		}
	};
	const onError = (event) => show(errorText({ code: event.detail.code, reason: event.detail.error }));
	live.addEventListener("datapoints", onDatapoints);
	live.addEventListener("error", onError);

	content.appendChild(h("h2", { class: "section-title" }, t("overview.datapoints")));
	content.appendChild(message);
	content.appendChild(grid);
	content.appendChild(h("p", { class: "muted" }, t("overview.preview")));
	renderAll();

	return () => {
		live.removeEventListener("datapoints", onDatapoints);
		live.removeEventListener("error", onError);
		clearTimeout(messageTimer);
	};
}
