/* Copyright (c) 2026 Bernhard Haumacher */
// The System section of the Settings page: what an installed device's owner needs of the serial
// console (issue #38, spec/api.md "System"): the update (also from a firmware file, issue #39),
// restart and factory reset, log levels, the core dump, the API tokens, storage, the connections
// (KNX, MQTT, external sources, the language pack) and the WiFi credentials. Every refusal shows the
// device's reason.

import { api, errorText } from "/api.js";
import { h, replace } from "/dom.js";
import { formatAgo, formatBytes } from "/format.js";
import { t, tv } from "/i18n.js";

function rows(pairs) {
	return h("dl", { class: "rows" }, pairs.filter(Boolean).map(([label, value]) => [h("dt", null, label), h("dd", null, value)]));
}

function card(title, ...children) {
	return h("section", { class: "card" }, h("h2", null, title), ...children);
}

/// A button that runs `action` and shows its outcome (or the refusal) in `feedback`.
function actionButton(label, feedback, action, options = {}) {
	const button = h("button", { type: "button", class: `button small ${options.danger ? "danger" : "secondary"}` }, label);
	button.addEventListener("click", async () => {
		if (options.confirm && !window.confirm(options.confirm())) {
			return;
		}
		button.disabled = true;
		feedback.className = "hint";
		feedback.textContent = "";
		try {
			const result = await action();
			feedback.textContent = options.done ? options.done(result) : t("system.done");
		} catch (error) {
			feedback.className = "error-text";
			feedback.textContent = errorText(error);
		} finally {
			button.disabled = false;
		}
	});
	return button;
}

function feedbackLine() {
	return h("p", { class: "hint", role: "status" });
}

function updateCard(status, app) {
	const update = (status && status.update) || {};
	const feedback = feedbackLine();
	if (update.available === false) {
		return card(t("system.update"), h("p", { class: "muted" }, t("status.update.unavailable")));
	}
	return card(t("system.update"),
		rows([
			[t("status.update.last"), update.checking ? t("system.update.checking") : (update.last || "—")],
			[t("status.update.source"), update.source || t("status.none")],
		]),
		h("div", { class: "actions" },
			actionButton(t("system.update.check"), feedback, () => api("POST", "/api/system/update/check"),
				{ done: () => t("system.update.started") }),
			actionButton(t("system.update.now"), feedback, () => api("POST", "/api/system/update/now"),
				{ confirm: () => t("system.update.now.confirm"), done: () => t("system.update.started") })),
		feedback,
		uploadSection(status, app));
}

// ---- Install a firmware file (doc/ota.md "Upload from the browser") -------------------------------
//
// The application cannot overwrite itself: it hands over to the updater, which serves PUT /firmware
// on the same address and port. The one-time token travels in this origin's sessionStorage, where
// the updater's own page finds it after a reload.

const TOKEN_KEY = "hc.upload.token";
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

/// The app descriptor of an ESP image (doc/ota.md): {project, version}, or null if `file` is none.
async function readImage(file) {
	const bytes = new Uint8Array(await file.slice(0, 288).arrayBuffer());
	if (bytes.length < 288 || bytes[0] !== 0xe9 || new DataView(bytes.buffer).getUint32(32, true) !== 0xabcd5432) {
		return null;
	}
	const text = (offset) => new TextDecoder().decode(bytes.subarray(offset, offset + 32)).replace(/\0[\s\S]*$/, "");
	return { version: text(48), project: text(80) };
}

/// The JSON at `path`; null for an error status or another kind of answer (the application serves its
/// page for unknown paths). Rejects when the device does not answer at all.
async function getJson(path) {
	const options = { cache: "no-store", credentials: "same-origin" };
	if (AbortSignal.timeout) {
		options.signal = AbortSignal.timeout(4000); // a restarting device may leave a request hanging
	}
	const response = await fetch(path, options);
	if (!response.ok) {
		return null;
	}
	try {
		return await response.json();
	} catch (e) {
		return null;
	}
}

/// Waits until the updater answers /status (the application's server is gone meanwhile: connection
/// errors mean "not yet"). "updater", "application" (it came back instead) or "timeout".
async function waitForUpdater() {
	let down = false;
	for (let i = 0; i < 80; i++) {
		await sleep(1500);
		try {
			const s = await getJson("/status");
			if (s && s.updater) {
				return "updater";
			}
			if (down && await getJson("/api/auth/session")) {
				return "application";
			}
		} catch (e) {
			down = true;
		}
	}
	return "timeout";
}

/// Waits until the application answers again; its open session answer names version and build.
async function waitForApplication() {
	for (let i = 0; i < 90; i++) {
		await sleep(2000);
		try {
			const s = await getJson("/api/auth/session");
			if (s && s.version) {
				return s;
			}
		} catch (e) {
			// still restarting
		}
	}
	return null;
}

function putFirmware(file, token, onProgress) {
	return new Promise((resolve) => {
		const xhr = new XMLHttpRequest(); // fetch() reports no upload progress
		xhr.open("PUT", "/firmware");
		xhr.setRequestHeader("Authorization", `Bearer ${token}`);
		xhr.upload.onprogress = (e) => e.lengthComputable && onProgress(Math.round((100 * e.loaded) / e.total));
		xhr.onload = () => {
			let body = {};
			try {
				body = JSON.parse(xhr.responseText);
			} catch (e) {
				// not JSON: the status says enough
			}
			resolve({ status: xhr.status, body });
		};
		xhr.onerror = () => resolve({ status: 0, body: {} });
		xhr.send(file);
	});
}

function uploadSection(status, app) {
	const ip = (status && status.network && status.network.ip) || "";
	const url = `http://${ip}/`;
	const feedback = feedbackLine();
	const progress = h("progress", { max: "100", value: "0", hidden: true });
	const input = h("input", { type: "file", accept: ".bin", hidden: true });
	const button = h("button", { type: "button", class: "button small secondary" }, t("system.upload"));
	const say = (text, error) => {
		feedback.className = error ? "error-text" : "hint";
		replace(feedback, text);
	};
	const leaveGuard = (event) => event.preventDefault();
	if (ip && window.location.hostname !== ip) {
		// The updater answers by address only (no mDNS) and sessionStorage is per origin.
		return h("div", null, h("p", { class: "hint" }, t("system.upload.by_address"), " ", h("a", { href: url }, url)));
	}
	button.addEventListener("click", () => input.click());
	input.addEventListener("change", async () => {
		const file = input.files[0];
		input.value = "";
		if (!file) {
			return;
		}
		const image = await readImage(file);
		if (!image || image.project !== "homecontrol") {
			say(t("system.upload.not_firmware"), true);
			return;
		}
		if (!window.confirm(t("system.upload.confirm", { version: image.version, file: file.name }))) {
			return;
		}
		button.disabled = true;
		window.addEventListener("beforeunload", leaveGuard);
		try {
			await install(file);
		} finally {
			window.removeEventListener("beforeunload", leaveGuard);
			button.disabled = false;
		}
	});
	async function install(file) {
		let started;
		try {
			started = await api("POST", "/api/system/update/upload");
		} catch (error) {
			say(errorText(error), true);
			return;
		}
		sessionStorage.setItem(TOKEN_KEY, started.token);
		app.live.stop(); // the application goes away; its WebSocket would only report that
		say(t("system.upload.restarting"));
		const reached = await waitForUpdater();
		if (reached !== "updater") {
			sessionStorage.removeItem(TOKEN_KEY);
			say(t(reached === "application" ? "system.upload.no_updater" : "system.upload.lost", { url: started.url }), true);
			return;
		}
		progress.hidden = false;
		const result = await putFirmware(file, started.token, (percent) => {
			progress.value = percent;
			say(percent < 100 ? t("system.upload.progress", { percent }) : t("system.upload.verifying"));
		});
		progress.hidden = true;
		if (result.status === 401) {
			say(t("system.upload.unauthorized", { url: started.url }), true);
			return;
		}
		if (result.status === 0) {
			say(t("system.upload.network", { url: started.url }), true);
			return;
		}
		if (result.status !== 200) {
			say(t("system.upload.refused", { reason: result.body.error || `HTTP ${result.status}`, url: started.url }), true);
			return;
		}
		sessionStorage.removeItem(TOKEN_KEY);
		say(t("system.upload.installed", { version: result.body.version }));
		const running = await waitForApplication();
		if (!running) {
			say(t("system.upload.lost", { url: started.url }), true);
			return;
		}
		say([t("system.upload.done", { version: running.version, build: running.build || "?" }), " ",
			h("button", { type: "button", class: "button small", onclick: () => window.location.reload() }, t("system.upload.reload"))]);
	}
	return h("div", null,
		h("p", { class: "hint" }, t("system.upload.hint")),
		h("div", { class: "actions" }, button, input), progress, feedback);
}

function restartCard(ctx) {
	const feedback = feedbackLine();
	const name = h("input", { type: "text", id: "reset-name", autocomplete: "off", spellcheck: "false", placeholder: ctx.deviceName });
	const reset = actionButton(t("system.reset"), feedback,
		() => api("POST", "/api/system/factory-reset", { confirm: name.value.trim() }),
		{ danger: true, confirm: () => t("system.reset.confirm"), done: () => t("system.restarting") });
	reset.disabled = true;
	name.addEventListener("input", () => {
		reset.disabled = name.value.trim() !== ctx.deviceName;
	});
	return card(t("system.restart.title"),
		h("div", { class: "actions" }, actionButton(t("system.restart"), feedback, () => api("POST", "/api/system/restart"),
			{ confirm: () => t("system.restart.confirm"), done: () => t("system.restarting") })),
		h("div", { class: "field" }, h("label", { for: "reset-name" }, t("system.reset.label", { name: ctx.deviceName })),
			h("div", { class: "input-row" }, name, reset), h("span", { class: "hint" }, t("system.reset.hint"))),
		feedback);
}

function logCard() {
	const list = h("div", null, h("p", { class: "muted" }, t("app.loading")));
	const feedback = feedbackLine();
	const tag = h("input", { type: "text", id: "log-tag", value: "*", spellcheck: "false", autocomplete: "off" });
	const level = h("select", { id: "log-level" }, ["error", "warn", "info", "debug"].map((l) => h("option", { value: l, selected: l === "info" }, l)));
	const show = (data) => replace(list, rows([[t("system.log.default"), data.default],
		...data.tags.map((x) => [x.tag, x.level])]),
	h("p", { class: "hint" }, data.platform_tags ? t("system.log.hint.platform") : t("system.log.hint")));
	api("GET", "/api/system/log-level").then(show).catch((error) => replace(list, h("p", { class: "error-text" }, errorText(error))));
	const set = actionButton(t("system.log.set"), feedback, async () => {
		const data = await api("PUT", "/api/system/log-level", { tag: tag.value.trim(), level: level.value });
		show(data);
	});
	return card(t("system.log"), list,
		h("div", { class: "field" }, h("label", { for: "log-tag" }, t("system.log.tag")), h("div", { class: "input-row" }, tag, level, set)),
		feedback);
}

function coredumpCard(status, rerender) {
	const boot = (status && status.boot) || {};
	const feedback = feedbackLine();
	if (!boot.coredump) {
		return card(t("system.coredump"), h("p", { class: "muted" }, t("system.coredump.none")));
	}
	return card(t("system.coredump"),
		rows([boot.coredump_task ? [t("system.coredump.task"), `${boot.coredump_task} ${boot.coredump_pc || ""}`] : null]),
		h("div", { class: "actions" },
			h("a", { class: "button small secondary", href: "/api/coredump", download: "core.bin" }, t("system.coredump.download")),
			actionButton(t("system.coredump.clear"), feedback, async () => {
				await api("DELETE", "/api/coredump");
				rerender();
			}, { confirm: () => t("system.coredump.clear.confirm") })),
		feedback);
}

function tokenCard() {
	const list = h("div", null);
	const feedback = feedbackLine();
	const shown = h("div", null);
	const name = h("input", { type: "text", id: "token-name", spellcheck: "false", autocomplete: "off", placeholder: "ehome" });
	const load = () => api("GET", "/api/tokens").then((data) => {
		replace(list, data.tokens.length === 0 ? h("p", { class: "muted" }, t("system.tokens.none"))
			: rows(data.tokens.map((token) => [token.name, actionButton(t("system.tokens.revoke"), feedback, async () => {
				await api("DELETE", `/api/tokens/${encodeURIComponent(token.name)}`);
				load();
			}, { danger: true, confirm: () => t("system.tokens.revoke.confirm", { name: token.name }) })])));
	}).catch((error) => replace(list, h("p", { class: "error-text" }, errorText(error))));
	load();
	const create = actionButton(t("system.tokens.create"), feedback, async () => {
		const data = await api("POST", "/api/tokens", { name: name.value.trim() });
		replace(shown, h("p", { class: "message ok" }, t("system.tokens.shown", { name: data.name })),
			h("p", { class: "token" }, data.token));
		name.value = "";
		load();
	});
	return card(t("system.tokens"), h("p", { class: "hint" }, t("system.tokens.hint")), list,
		h("div", { class: "field" }, h("label", { for: "token-name" }, t("system.tokens.name")), h("div", { class: "input-row" }, name, create)),
		shown, feedback);
}

function storageCard(details) {
	const s = details && details.storage;
	if (!s) {
		return card(t("system.storage"), h("p", { class: "muted" }, t("system.storage.unknown")));
	}
	return card(t("system.storage"), rows([
		s.total !== undefined ? [t("system.storage.used"), t("system.storage.used.value", { used: formatBytes(s.used), total: formatBytes(s.total) })] : null,
		s.files !== undefined ? [t("system.storage.files"), t("system.storage.files.value", { files: s.files, bytes: formatBytes(s.bytes) })] : null,
	]));
}

function connectionsCard(details) {
	const feedback = feedbackLine();
	const d = details || {};
	const parts = [];
	if (d.knx) {
		parts.push([t("system.knx"), d.knx.gateway ? `${tv("system.knx.", d.knx.state)} – ${d.knx.gateway}${d.knx.nat ? " (NAT)" : ""}`
			: t("system.knx.none")]);
	}
	if (d.mqtt) {
		parts.push([t("system.mqtt"), d.mqtt.configured ? h("span", null, `${tv("system.mqtt.", d.mqtt.state)} – ${d.mqtt.broker} `,
			actionButton(t("system.mqtt.discovery"), feedback, () => api("POST", "/api/mqtt/discovery"))) : t("system.mqtt.none")]);
	}
	for (const source of (d.external && d.external.sources) || []) {
		parts.push([source.id, h("span", null,
			source.last_ok_ago_s !== undefined ? t("system.external.ok", { ago: formatAgo(source.last_ok_ago_s * 1000) })
				: (source.error || t("system.external.never")), " ",
			actionButton(t("system.external.fetch"), feedback, () => api("POST", `/api/external/${encodeURIComponent(source.id)}/fetch`),
				{ done: () => t("system.external.fetching") }))]);
	}
	if (d.lang) {
		parts.push([t("system.lang"), h("span", null, `${d.lang.lang}: ${t(`lang.state.${d.lang.state}`, { detail: d.lang.detail })} `,
			actionButton(t("system.lang.sync"), feedback, () => api("POST", "/api/lang/sync"), { done: () => t("system.lang.syncing") }))]);
	}
	return card(t("system.connections"), parts.length ? rows(parts) : h("p", { class: "muted" }, t("status.none")), feedback);
}

function wifiCard(ctx) {
	const feedback = feedbackLine();
	const ssid = h("input", { type: "text", id: "wifi-ssid", spellcheck: "false", autocomplete: "off" });
	const psk = h("input", { type: "password", id: "wifi-psk", autocomplete: "new-password" });
	const save = actionButton(t("system.wifi.save"), feedback, () => api("PUT", "/api/system/wifi", { ssid: ssid.value, psk: psk.value }),
		{ danger: true, confirm: () => t("system.wifi.confirm", { ssid: ssid.value, name: `${ctx.deviceName}.local` }),
			done: (r) => t("system.wifi.done", { name: r.mdns }) });
	return card(t("system.wifi"), h("p", { class: "message error" }, t("system.wifi.warning")),
		h("div", { class: "field" }, h("label", { for: "wifi-ssid" }, t("system.wifi.ssid")), ssid),
		h("div", { class: "field" }, h("label", { for: "wifi-psk" }, t("system.wifi.psk")), psk),
		h("div", { class: "actions" }, save), feedback);
}

/// The System section into `container`, from GET /api/status (the details the live status lacks:
/// storage, connections). `app` is the application's context: its live connection stops while a
/// firmware file is installed. Drawn once: a token shown after its creation stays until the page is
/// left.
export function renderSystem(container, app) {
	api("GET", "/api/status").then((status) => {
		const ctx = { deviceName: (status.device && status.device.name) || status.name || "" };
		const dump = h("div", null);
		const drawDump = (s) => replace(dump, coredumpCard(s, () => api("GET", "/api/status").then(drawDump)));
		drawDump(status);
		replace(container, h("div", { class: "cards" },
			updateCard(status, app), restartCard(ctx), dump, storageCard(status),
			connectionsCard(status), logCard(), tokenCard(), wifiCard(ctx)));
	}).catch((error) => replace(container, h("p", { class: "message error" }, errorText(error))));
}
