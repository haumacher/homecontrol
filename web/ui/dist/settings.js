/* Copyright (c) 2026 Bernhard Haumacher */
// Settings: the device status (network, time, update, versions, system) live from the WebSocket,
// the settings the UI may change (validated by the device exactly as on the console), and logout.

import { api, errorText } from "/api.js";
import { h, replace } from "/dom.js";
import { formatBytes, formatDuration } from "/format.js";
import { t, tv, has } from "/i18n.js";

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

function settingRow(setting, onSaved) {
	const id = `setting-${setting.key}`;
	const label = has(`settings.key.${setting.key}`) ? t(`settings.key.${setting.key}`) : setting.key;
	const input = h("input", { type: "text", id, value: setting.value ?? "", disabled: !setting.editable, spellcheck: "false",
		autocomplete: "off" });
	const feedback = h("span", { class: "hint" }, setting.editable
		? (setting.default ? t("settings.default", { value: setting.default }) : "")
		: t("settings.readonly"));
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

	const loadSettings = () => api("GET", "/api/settings").then((data) => {
		replace(editable, data.settings.map((s) => settingRow(s, () => {})));
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

	content.appendChild(message);
	content.appendChild(h("h2", { class: "section-title" }, t("settings.status")));
	content.appendChild(status);
	content.appendChild(h("h2", { class: "section-title" }, t("settings.editable")));
	content.appendChild(editable);
	content.appendChild(logout);
	return () => ctx.live.removeEventListener("status", onStatus);
}
