/* Copyright (c) 2026 Bernhard Haumacher */
// Settings: the device status (network, time, update, versions, system) live from the WebSocket,
// the settings the UI may change (validated by the device exactly as on the console), the UI
// language (ui.lang, doc/i18n.md), and logout.

import { api, errorText } from "/api.js";
import { h, replace } from "/dom.js";
import { formatAgo, formatBytes, formatDuration } from "/format.js";
import { t, tv, has, bundled, language, languageFound } from "/i18n.js";
import { loadModel } from "/model.js";
import { currentTheme, setTheme } from "/theme.js";

function rows(pairs) {
	return h("dl", { class: "rows" }, pairs.filter(Boolean).map(([label, value]) => [h("dt", null, label), h("dd", null, value)]));
}

function badge(ok, yes, no) {
	return h("span", { class: `badge ${ok ? "ok" : "warn"}` }, ok ? yes : no);
}

function statusCards(s) {
	const n = s.network || {};
	const time = s.time || {};
	const update = s.update || {};
	const device = s.device || {};
	const memory = s.memory || {};
	const improv = s.improv || {};
	const web = s.web || {};
	return h(
		"div",
		{ class: "cards" },
		h("section", { class: "card" }, h("h2", null, t("settings.network")), rows([
			[t("status.state"), badge(n.state === "up", tv("state.link.", n.state), tv("state.link.", n.state))],
			[t("status.ip"), n.ip || "—"],
			[t("status.mdns"), n.mdns || "—"],
			n.mode && n.mode !== "normal" ? [t("status.mode"), tv("state.mode.", n.mode)] : null,
		])),
		h("section", { class: "card" }, h("h2", null, t("settings.time")), rows([
			[t("status.local"), time.local || "—"],
			[t("status.synchronized"), badge(time.synchronized, t("status.yes"), t("status.not_synchronized"))],
			[t("status.tz"), time.tz || "—"],
			[t("status.ntp"), time.ntp_server || t("status.none")],
		])),
		h("section", { class: "card" }, h("h2", null, t("settings.update")), update.available === false && !update.partition
			? h("p", { class: "muted" }, t("status.update.unavailable"))
			: rows([
				update.partition ? [t("status.update.partition"), update.partition] : null,
				[t("status.update.source"), update.source || t("status.none")],
				update.last ? [t("status.update.last"), update.last] : null,
				update.next_check_s !== undefined ? [t("status.update.next"), formatDuration(update.next_check_s)] : null,
			])),
		h("section", { class: "card" }, h("h2", null, t("settings.versions")), rows([
			[t("status.firmware"), device.version || "—"],
			[t("status.build"), device.build || "—"],
			[t("status.ui"), device.ui || "—"],
		])),
		h("section", { class: "card" }, h("h2", null, t("settings.system")), rows([
			[t("status.uptime"), formatDuration(s.uptime_s || 0)],
			[t("status.heap"), t("status.heap.value", { free: formatBytes(memory.free), min: formatBytes(memory.min_free) })],
			[t("status.improv"), improv.active ? t("status.improv.active", { seconds: improv.ends_in_s }) : t("status.improv.ended")],
			[t("status.led"), tv("state.led.", s.led)],
			[t("status.datapoints"), String(s.datapoints ?? "—")],
		])),
		h("section", { class: "card" }, h("h2", null, t("settings.web")), rows([
			[t("status.sessions"), String(web.sessions ?? "—")],
			[t("status.ws"), String(web.ws_clients ?? "—")],
			[t("status.tokens"), String(web.tokens ?? "—")],
		])),
	);
}

/// A setting with a fixed set of values (wifi.ps): a selector, applied on change, with the hint
/// text settings.<key>.hint; the choices are labelled settings.<key>.<value>.
function choiceRow(setting, choices) {
	const id = `setting-${setting.key}`;
	const value = setting.value ?? setting.default ?? choices[0];
	const select = h("select", { id, disabled: !setting.editable },
		choices.map((c) => h("option", { value: c, selected: c === value },
			has(`settings.${setting.key}.${c}`) ? t(`settings.${setting.key}.${c}`) : c)));
	const feedback = h("span", { class: "hint" }, has(`settings.${setting.key}.hint`) ? t(`settings.${setting.key}.hint`) : "");
	select.addEventListener("change", async () => {
		select.disabled = true;
		feedback.className = "hint";
		try {
			await api("PUT", `/api/settings/${encodeURIComponent(setting.key)}`, { value: select.value });
			feedback.textContent = t("settings.saved");
		} catch (error) {
			feedback.className = "error-text";
			feedback.textContent = errorText(error);
		} finally {
			select.disabled = false;
		}
	});
	return h("div", { class: "field" }, h("label", { for: id }, t(`settings.key.${setting.key}`)), select, feedback);
}

const kChoices = { "wifi.ps": ["none", "min", "max"] };

function settingRow(setting, onSaved) {
	const id = `setting-${setting.key}`;
	const label = has(`settings.key.${setting.key}`) ? t(`settings.key.${setting.key}`) : setting.key;
	// A secret (mqtt.password) is write-only: the device says whether one is stored, never its value;
	// an empty save removes it.
	const input = h("input", { type: setting.secret ? "password" : "text", id, value: setting.value ?? "",
		disabled: !setting.editable, spellcheck: "false", autocomplete: setting.secret ? "new-password" : "off",
		placeholder: setting.secret ? t(setting.stored ? "settings.secret.stored" : "settings.secret.empty") : "" });
	const feedback = h("span", { class: "hint" }, !setting.editable ? t("settings.readonly")
		: setting.secret ? t("settings.secret.hint")
		: (setting.default ? t("settings.default", { value: setting.default }) : ""));
	const save = h("button", { class: "button", type: "submit", disabled: !setting.editable }, t("settings.save"));
	return h(
		"form",
		{
			class: "field",
			onsubmit: async (event) => {
				event.preventDefault();
				save.disabled = true;
				feedback.className = "hint";
				try {
					await api("PUT", `/api/settings/${encodeURIComponent(setting.key)}`, { value: input.value.trim() });
					feedback.textContent = t("settings.saved");
					onSaved();
				} catch (error) {
					feedback.className = "error-text";
					feedback.textContent = errorText(error);
				} finally {
					save.disabled = false;
				}
			},
		},
		h("label", { for: id }, label),
		h("div", { class: "input-row" }, input, setting.editable ? save : null),
		feedback,
	);
}

/// The UI language: the bundled packs and the ones the release's manifest offers, by their own
/// names; a change is stored as ui.lang and applied at once. The device's pack state below it.
function languageRow(setting, i18n, ctx, reload) {
	const id = "setting-ui.lang";
	const names = { ...bundled };
	for (const offer of (i18n && i18n.offered) || []) {
		if (typeof offer.code === "string" && typeof offer.name === "string" && !(offer.code in names)) {
			names[offer.code] = offer.name;
		}
	}
	const value = setting.value || setting.default || "en";
	if (!(value in names)) {
		names[value] = value;
	}
	const select = h("select", { id, disabled: !setting.editable },
		Object.entries(names).map(([code, name]) => h("option", { value: code, selected: code === value }, name)));
	const feedback = h("span", { class: "hint", role: "status" });
	const state = i18n && i18n.lang === value ? i18n : null;
	const describe = () => {
		if (!setting.editable) {
			return t("settings.readonly");
		}
		const parts = [];
		if (value !== language() || !languageFound()) {
			parts.push(t("lang.missing", { lang: names[value] }));
		}
		if (state) {
			parts.push(has(`lang.state.${state.state}`) ? t(`lang.state.${state.state}`, { detail: state.detail || "" }) : state.detail);
		}
		return parts.join(" ");
	};
	feedback.textContent = describe();
	select.addEventListener("change", async () => {
		select.disabled = true;
		feedback.className = "hint";
		try {
			await api("PUT", "/api/settings/ui.lang", { value: select.value });
			// Redraws the page in the new language (and comes back here).
			await ctx.setLanguage(select.value);
		} catch (error) {
			feedback.className = "error-text";
			feedback.textContent = errorText(error);
			select.disabled = false;
		}
	});
	// While the device downloads the pack, look again until it is there.
	if (state && (state.state === "waiting" || state.state === "fetching") && reload) {
		reload.schedule();
	}
	return h("div", { class: "field" }, h("label", { for: id }, t("settings.key.ui.lang")), select, feedback);
}

/// The deployed image: project, revision, when and how it came; "forget image" removes it.
function imageCard(ctx, rerender) {
	const card = h("section", { class: "card" }, h("h2", null, t("image.title")), h("p", { class: "muted" }, t("app.loading")));
	const feedback = h("p", { class: "error-text", role: "status" });
	loadModel().then((model) => {
		if (!model.loaded) {
			replace(card, h("h2", null, t("image.title")), h("p", { class: "muted" }, t("image.none")));
			return;
		}
		const deployedAgo = model.nowMs - model.loadedMs + (Date.now() - model.fetchedAt);
		const forget = h("button", { type: "button", class: "button danger", onclick: async () => {
			if (!window.confirm(t("image.forget.confirm", { name: model.name || model.project }))) {
				return;
			}
			forget.disabled = true;
			try {
				await api("DELETE", "/api/image");
				rerender();
			} catch (error) {
				feedback.textContent = errorText(error);
				forget.disabled = false;
			}
		} }, t("image.forget"));
		replace(card, h("h2", null, t("image.title")), rows([
			[t("image.project"), model.name ? `${model.name} (${model.project})` : model.project],
			[t("image.revision"), String(model.revision)],
			model.generatedAt ? [t("image.generated"), model.generatedAt] : null,
			model.generator ? [t("image.generator"), model.generator] : null,
			[t("image.deployed"), t("image.deployed.value", { ago: formatAgo(deployedAgo), source: tv("image.source.", model.source) })],
			[t("image.stored"), model.stored ? t("status.yes") : t("image.not_stored")],
		]), h("p", { class: "hint" }, t("image.forget.hint")), forget, feedback);
	}).catch((error) => {
		replace(card, h("h2", null, t("image.title")), h("p", { class: "muted" },
			error && error.code === "image.unavailable" ? t("image.unavailable") : errorText(error)));
	});
	return card;
}

function themeCard() {
	const select = h("select", { id: "theme", onchange: (e) => setTheme(e.target.value) },
		["auto", "dark", "light"].map((v) => h("option", { value: v, selected: currentTheme() === v }, t(`theme.${v}`))));
	return h("section", { class: "card" }, h("h2", null, t("theme.title")),
		h("div", { class: "field" }, h("label", { for: "theme" }, t("theme.label")), select));
}

export function renderSettings(content, ctx) {
	const status = h("div", null, h("p", { class: "muted" }, t("app.loading")));
	const editable = h("div", { class: "card" }, h("p", { class: "muted" }, t("app.loading")));
	const message = h("p", { class: "message error", role: "alert", hidden: true });

	const showStatus = (s) => replace(status, statusCards(s));
	const onStatus = (event) => showStatus(event.detail);
	ctx.live.addEventListener("status", onStatus);
	if (ctx.live.status) {
		showStatus(ctx.live.status);
	} else {
		api("GET", "/api/status").then(showStatus).catch((error) => {
			message.textContent = errorText(error);
			message.hidden = false;
		});
	}

	// A pack the device is downloading: the settings are read again every 3 s (at most 20 times),
	// and the page switches to the pack once it is installed.
	let polls = 0;
	let pollTimer = null;
	const reload = {
		schedule() {
			if (polls++ < 20) {
				clearTimeout(pollTimer);
				pollTimer = setTimeout(() => loadSettings(), 3000);
			}
		},
	};
	let lastState = null;
	const loadSettings = () => api("GET", "/api/settings").then((data) => {
		const i18n = data.i18n || null;
		if (lastState && i18n && lastState !== "installed" && i18n.state === "installed" && i18n.lang === language()) {
			ctx.setLanguage(i18n.lang); // the downloaded pack replaces the bundled texts
			return;
		}
		lastState = i18n ? i18n.state : null;
		replace(editable, data.settings.map((s) => (s.key === "ui.lang" ? languageRow(s, i18n, ctx, reload)
			: kChoices[s.key] ? choiceRow(s, kChoices[s.key]) : settingRow(s, () => {}))));
	}).catch((error) => {
		replace(editable, h("p", { class: "message error" }, errorText(error)));
	});
	loadSettings();

	const logout = h("button", {
		class: "button secondary",
		type: "button",
		onclick: async () => {
			try {
				await api("POST", "/api/auth/logout", {});
			} catch (error) {
				// the cookie is gone either way
			}
			ctx.live.stop();
			await ctx.refreshSession().catch(() => {});
			ctx.navigate("/login", true);
		},
	}, t("auth.logout"));

	const project = h("div", null);
	const showImage = () => replace(project, imageCard(ctx, showImage));
	showImage();
	content.appendChild(message);
	content.appendChild(h("h2", { class: "section-title" }, t("image.section")));
	content.appendChild(h("div", { class: "cards" }, project, themeCard()));
	content.appendChild(h("h2", { class: "section-title" }, t("settings.status")));
	content.appendChild(status);
	content.appendChild(h("h2", { class: "section-title" }, t("settings.editable")));
	content.appendChild(editable);
	content.appendChild(logout);
	return () => {
		ctx.live.removeEventListener("status", onStatus);
		clearTimeout(pollTimer);
		polls = 20;
	};
}
