/* Copyright (c) 2026 Bernhard Haumacher */
// Every user-visible text of the UI goes through t(key) (doc/i18n.md). The base language, English,
// is /lang/en.json in the UI bundle, loaded before anything renders; it is the key set every pack
// follows and the fallback for a key a pack lacks. The device's ui.lang selects a language pack:
// the one bundled with the UI (/lang/<code>.json) and the one the device downloaded for its release
// (/i18n/<code>.json, preferred key by key). Pack strings are untrusted input: only strings are
// taken, and they are only ever set as textContent or attribute values, never parsed as HTML. The
// API's error codes are keys too ("auth.wrong_password"), so a refusal shows in the UI's language;
// the API's English reason is the fallback for codes the table does not know.

/// The packs bundled with this UI, by their own name (not translated). A language pack the device
/// downloads later is offered by the device's manifest instead (GET /api/settings, "i18n").
export const bundled = {
	"en": "English",
	"de": "Deutsch",
};

const kTag = /^[a-z]{2}(-[A-Z]{2})?$/;

/// Whether `lang` is a language tag the device accepts for ui.lang.
export function validLanguage(lang) {
	return typeof lang === "string" && kTag.test(lang);
}

/// The strings of a parsed pack: own members whose values are strings, nothing else.
function strings(pack) {
	const result = Object.create(null);
	if (pack && typeof pack === "object" && !Array.isArray(pack)) {
		for (const [key, value] of Object.entries(pack)) {
			if (typeof value === "string") {
				result[key] = value;
			}
		}
	}
	return result;
}

async function loadPack(url) {
	try {
		const response = await fetch(url, { credentials: "same-origin" });
		// 204: the device has no downloaded pack for this language (not an error).
		return response.ok && response.status !== 204 ? strings(await response.json()) : null;
	} catch (e) {
		return null;
	}
}

const base = (await loadPack("/lang/en.json")) || Object.create(null);
let table = Object.create(null);
let current = "en";
let found = true;

/// Switches to `lang` (the device's ui.lang): loads its bundled and its downloaded pack, fills the
/// static texts of the page. Resolves with whether a translation was found (English always is);
/// without one the texts stay English.
export async function setLanguage(lang) {
	current = validLanguage(lang) ? lang : "en";
	found = true;
	const next = Object.create(null);
	const base = current.split("-")[0];
	if (base !== "en") {
		// A regional tag ("de-AT") layers its own packs over the base language's bundled one ("de");
		// the device downloads the base language's pack when the release has none for the region.
		const [shippedBase, shipped, downloaded] = await Promise.all([
			base !== current ? loadPack(`/lang/${base}.json`) : null,
			loadPack(`/lang/${current}.json`),
			loadPack(`/i18n/${current}.json`),
		]);
		Object.assign(next, shippedBase || {}, shipped || {}, downloaded || {});
		found = !!(shippedBase || shipped || downloaded);
	}
	table = next;
	document.documentElement.lang = found ? current : "en";
	applyI18n();
	return found;
}

/// The UI language (a tag), also the locale of number and date formats.
export function language() {
	return current;
}

/// Whether a pack for the UI language was found (false: the texts are English).
export function languageFound() {
	return found;
}

/// The text of `key` with `{name}` placeholders filled from `params` (values as given, never HTML).
export function t(key, params) {
	let text = table[key] ?? base[key];
	if (text === undefined) {
		return key;
	}
	if (params) {
		text = text.replace(/\{(\w+)\}/g, (all, name) => (name in params ? String(params[name]) : all));
	}
	return text;
}

/// The translation of a value the device names (a state), or the name itself if there is none.
export function tv(prefix, value) {
	return value !== undefined && has(prefix + value) ? t(prefix + value) : (value ?? "—");
}

/// Whether the table knows `key`.
export function has(key) {
	return key in table || key in base;
}

/// Fills the static texts under `root`: data-i18n sets textContent, data-i18n-label the aria-label.
export function applyI18n(root = document) {
	for (const element of root.querySelectorAll("[data-i18n]")) {
		element.textContent = t(element.dataset.i18n);
	}
	for (const element of root.querySelectorAll("[data-i18n-label]")) {
		element.setAttribute("aria-label", t(element.dataset.i18nLabel));
	}
}
