/* Copyright (c) 2026 Bernhard Haumacher */
// The colour theme: "auto" follows the browser (prefers-color-scheme), "dark" and "light" force
// one. Remembered in this browser only (a convenience; storage may be unavailable).

const KEY = "hc.theme";

export function currentTheme() {
	try {
		return localStorage.getItem(KEY) || "auto";
	} catch (e) {
		return "auto";
	}
}

export function applyTheme(theme = currentTheme()) {
	if (theme === "dark" || theme === "light") {
		document.documentElement.dataset.theme = theme;
	} else {
		delete document.documentElement.dataset.theme;
	}
}

export function setTheme(theme) {
	try {
		localStorage.setItem(KEY, theme);
	} catch (e) {
		// not remembered, applied for this page
	}
	applyTheme(theme);
}
