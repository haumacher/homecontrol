/* Copyright (c) 2026 Bernhard Haumacher */
// The image's model as the dashboard needs it (GET /api/model, spec/api.md): locations, functions
// with their datapoint roles, the datapoints' labels, units and bindings. Fetched in pages; a page
// of another generation (the image changed meanwhile) restarts the fetch.

import { api } from "/api.js";
import { language } from "/i18n.js";

const browser = (navigator.language || "en").toLowerCase();

/// The text of an image label: a string, or an object of language tags to strings (the UI
/// language, its base language, the browser's language and its base, then English, then the first
/// one). The labels come from the image (E-Home) and are not translated here.
export function label(value, fallback = "") {
	if (typeof value === "string") {
		return value;
	}
	if (value && typeof value === "object") {
		const ui = language();
		for (const tag of [ui, ui.toLowerCase(), ui.split("-")[0], browser, browser.split("-")[0], "en"]) {
			if (typeof value[tag] === "string") {
				return value[tag];
			}
		}
		const first = Object.values(value).find((v) => typeof v === "string");
		return first ?? fallback;
	}
	return fallback;
}

/// Fetches every page of a paged list endpoint (`from`/`next`); `merge(page)` receives each page.
export async function pages(path, merge) {
	let from = 0;
	for (let guard = 0; guard < 200; guard++) {
		const page = await api("GET", `${path}${path.includes("?") ? "&" : "?"}from=${from}`);
		merge(page);
		if (page.next === null || page.next === undefined) {
			return;
		}
		from = page.next;
	}
}

/// The model, or {loaded: false} without an image; rejects with the ApiError (404
/// image.unavailable in a build without images).
export async function loadModel() {
	for (let attempt = 0; attempt < 3; attempt++) {
		const model = { loaded: false, locations: [], functions: [], datapoints: new Map() };
		let generation = null;
		let restart = false;
		await pages("/api/model", (page) => {
			if (!page.loaded) {
				return;
			}
			if (generation !== null && page.generation !== generation) {
				restart = true;
			}
			generation = page.generation;
			Object.assign(model, {
				loaded: true, project: page.project, name: page.name, revision: page.revision, generatedAt: page.generatedAt,
				generator: page.generator, source: page.source, stored: page.stored, loadedMs: page.loaded_ms, nowMs: page.now_ms,
				fetchedAt: Date.now(),
			});
			model.locations.push(...page.locations);
			model.functions.push(...page.functions);
			for (const dp of page.datapoints) {
				model.datapoints.set(dp.id, dp);
			}
		});
		if (!restart) {
			model.locationsById = new Map(model.locations.map((l) => [l.id, l]));
			return model;
		}
	}
	throw { code: "model.changing" };
}

/// How the inhabitant's pages name a datapoint: its label from the model, else (no label, or an
/// empty one) its id in the image (`name`, e.g. a group address). Only the technical pages
/// (System, Log, Zigbee) show the id beside the label.
export function dpLabel(model, id, live) {
	const info = model && model.datapoints.get(id);
	const text = info ? label(info.label, "") : "";
	if (text) {
		return text;
	}
	const dp = live && live.datapoints.get(id);
	return info ? info.name : dp ? dp.name : String(id);
}

/// The datapoint's unit from the model ("" if none).
export function dpUnit(model, id) {
	const info = model && model.datapoints.get(id);
	return (info && info.unit) || "";
}
