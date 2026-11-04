/*
 * Copyright (c) 2026 Bernhard Haumacher
 *
 * The web flasher's pages: shows the hint for browsers without Web Serial, the version the install
 * button offers, and (on the published site, which has releases.json) a choice of the releases on
 * the site. The page's <main> carries data-base, the site root relative to the page.
 */

const main = document.querySelector("main");
const base = main.dataset.base || "";
const button = document.querySelector("esp-web-install-button");
const versionLine = document.getElementById("install-version");
const picker = document.getElementById("install-picker");
const select = picker ? picker.querySelector("select") : null;

if (!("serial" in navigator)) {
	const box = document.getElementById("install-unsupported");
	if (box) {
		box.hidden = false;
	}
} else {
	// The browser's port dialog lists every serial device (a laptop's mobile modem, USB adapters)
	// under names a user cannot judge. ESP Web Tools asks without filters, so the page adds one: only
	// the chip's own USB port (Espressif's vendor id), which the installation and the WiFi setup over
	// Improv need (step 1: the socket labelled USB, not UART).
	const ESPRESSIF = 0x303a;
	const requestPort = navigator.serial.requestPort.bind(navigator.serial);
	navigator.serial.requestPort = (options = {}) =>
		requestPort({ ...options, filters: options.filters && options.filters.length ? options.filters : [{ usbVendorId: ESPRESSIF }] });
}

function showVersion(manifestUrl) {
	fetch(manifestUrl, { cache: "no-cache" })
		.then((response) => (response.ok ? response.json() : Promise.reject(new Error(response.status))))
		.then((manifest) => {
			versionLine.textContent = versionLine.dataset.label.replace("{version}", manifest.version);
		})
		.catch(() => {
			versionLine.textContent = versionLine.dataset.missing;
		});
}

function setManifest(manifestUrl) {
	button.setAttribute("manifest", manifestUrl);
	showVersion(manifestUrl);
}

showVersion(button.getAttribute("manifest"));

fetch(base + "releases.json", { cache: "no-cache" })
	.then((response) => (response.ok ? response.json() : null))
	.then((site) => {
		if (!site || !select || site.releases.length < 2) {
			return;
		}
		for (const release of site.releases) {
			const option = document.createElement("option");
			option.value = release.version;
			option.textContent = release.version
				+ (release.version === site.stable ? " (" + select.dataset.stable + ")" : "")
				+ (release.prerelease ? " (" + select.dataset.prerelease + ")" : "");
			option.selected = release.version === site.stable;
			select.append(option);
		}
		select.addEventListener("change", () => {
			setManifest(select.value === site.stable
				? base + "manifest.json"
				: base + "firmware/" + select.value + "/install.json");
		});
		picker.hidden = false;
	})
	.catch(() => {});
