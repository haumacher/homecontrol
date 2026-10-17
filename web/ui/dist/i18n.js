/* Copyright (c) 2026 Bernhard Haumacher */
// Every user-visible text of the UI goes through t(key). The base language (English) is compiled in
// below; language packs (issue #24) will add tables per locale, loaded lazily, and their strings are
// untrusted: they are only ever set as textContent, never as HTML. The API's error codes are keys
// too ("auth.wrong_password"), so a refusal shows in the UI's language; the API's English reason is
// the fallback for codes this table does not know.

const base = {
	"app.title": "HomeControl",
	"app.loading": "Loading…",
	"app.noscript": "HomeControl needs JavaScript.",
	"app.offline": "The device does not answer. Retrying…",

	"nav.overview": "Overview",
	"nav.settings": "Settings",
	"nav.log": "Log",
	"nav.menu": "Menu",

	"live.connected": "Live",
	"live.connecting": "Connecting…",
	"live.refused": "Live updates refused: {reason}",

	"auth.onboarding.title": "Welcome",
	"auth.onboarding.text": "Set the device password. It protects this controller on your network; you need it to log in from every browser.",
	"auth.password": "Password",
	"auth.password.repeat": "Repeat the password",
	"auth.password.hint": "At least {min} characters.",
	"auth.password.too_short": "The password needs at least {min} characters.",
	"auth.password.too_long": "The password may have at most 64 characters.",
	"auth.password.mismatch": "The two passwords differ.",
	"auth.onboarding.submit": "Set password",
	"auth.login.title": "Log in",
	"auth.login.text": "Enter the device password of {device}.",
	"auth.login.submit": "Log in",
	"auth.login.working": "Checking…",
	"auth.login.attempts": "{left} more attempts before a pause.",
	"auth.login.wait": "Try again in {seconds} s.",
	"auth.logout": "Log out",

	"overview.title": "Overview",
	"overview.datapoints": "Datapoints",
	"overview.empty": "No datapoints yet. They come with the project E-Home deploys to this controller.",
	"overview.waiting": "Waiting for live values…",
	"overview.preview": "A first view: the dashboard of rooms and functions follows.",

	"value.on": "On",
	"value.off": "Off",
	"value.unknown": "—",
	"value.stale": "stale",
	"value.control": "control",

	"settings.title": "Settings",
	"settings.status": "Status",
	"settings.network": "Network",
	"settings.time": "Time",
	"settings.update": "Firmware update",
	"settings.versions": "Versions",
	"settings.system": "System",
	"settings.web": "Web access",
	"settings.editable": "Settings",
	"settings.save": "Save",
	"settings.saved": "Saved.",
	"settings.default": "Default: {value}",
	"settings.readonly": "Change it on the device's console.",
	"settings.key.tz": "Time zone (POSIX TZ)",
	"settings.key.ntp.server": "Time server (NTP)",
	"settings.key.update.url": "Update source",
	"settings.key.wifi.ssid": "WiFi network",

	"status.state": "State",
	"status.ip": "Address",
	"status.mdns": "Name",
	"status.mode": "Mode",
	"status.local": "Local time",
	"status.synchronized": "Synchronized",
	"status.not_synchronized": "Not synchronized",
	"status.tz": "Time zone",
	"status.ntp": "Time server",
	"status.firmware": "Firmware",
	"status.build": "Build",
	"status.ui": "Web UI",
	"status.uptime": "Uptime",
	"status.heap": "Free memory",
	"status.heap.value": "{free} (lowest {min})",
	"status.improv": "Improv Wi-Fi",
	"status.improv.active": "offered, ends in {seconds} s",
	"status.improv.ended": "ended",
	"status.led": "Status LED",
	"status.sessions": "Sessions",
	"status.tokens": "API tokens",
	"status.ws": "Live connections",
	"status.datapoints": "Datapoints",
	"status.update.source": "Source",
	"status.update.partition": "Partition",
	"status.update.last": "Last result",
	"status.update.next": "Next check",
	"status.update.unavailable": "This build does not update itself.",
	"status.none": "none",
	"state.link.down": "down",
	"state.link.connecting": "connecting",
	"state.link.up": "connected",
	"state.mode.unprovisioned": "no WiFi credentials (WPS)",
	"state.mode.re-pairing": "re-pairing (WPS)",
	"state.mode.failsafe (pairing)": "failsafe: listening for WPS",
	"state.mode.failsafe (stored credentials)": "failsafe: retrying the stored network",
	"state.led.pairing": "pairing (fast blink)",
	"state.led.connecting": "connecting (slow blink)",
	"state.led.setup": "online, no project yet (short pulse)",
	"state.led.degraded": "degraded",
	"state.led.ready": "ready (solid)",
	"status.yes": "yes",
	"status.no": "no",

	"log.title": "Log",
	"log.follow": "Follow",
	"log.empty": "No log lines yet.",
	"log.dropped": "{count} lines were dropped while the log was busy.",

	"duration.s": "{s} s",
	"duration.m": "{m} min",
	"duration.h": "{h} h {m} min",
	"duration.d": "{d} d {h} h",

	"error.network": "The device cannot be reached.",
	"error.unknown": "Something went wrong ({status}).",

	"auth.onboarding_required": "No device password is set yet: set one first.",
	"auth.password_set": "The device password is already set.",
	"auth.wrong_password": "Wrong password.",
	"auth.rate_limited": "Too many failed logins from this device: please wait.",
	"auth.password_invalid": "The password must have 8 to 64 characters and no control characters.",
	"auth.password_changed": "The password changed meanwhile: try again.",
	"auth.required": "Please log in.",
	"auth.csrf": "The request was refused (missing X-Requested-With header).",
	"auth.session_ended": "Your session ended. Please log in again.",
	"auth.storage_error": "The device could not store the password.",
	"api.bad_json": "The request was malformed.",
	"api.not_found": "Unknown API endpoint.",
	"dp.not_writable": "This datapoint cannot be switched.",
	"dp.wrong_type": "The value does not fit this datapoint.",
	"dp.refused": "The device refused the command.",
	"dp.unknown_id": "This datapoint no longer exists.",
	"dp.busy": "Busy: try again.",
	"settings.invalid": "Invalid value: {reason}",
	"settings.not_editable": "This setting cannot be changed here.",
	"ws.too_many_clients": "Too many live connections: close another tab or app.",
	"ws.foreign_origin": "Live updates refused for this page.",
};

let table = base;

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
