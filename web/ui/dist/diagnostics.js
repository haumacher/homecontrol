/* Copyright (c) 2026 Bernhard Haumacher */
// The System page (issue #42): what the console's `status` prints and what else helps with a
// complaint - memory, die temperature, chip and firmware, the event loop, the network link, storage,
// the buses and every task - from GET /api/system/diagnostics, polled every 5 s while the page is open;
// then the rules with the printer's texts of their last trigger, action and problem (GET /api/rules)
// and the blocks (GET /api/blocks).
// Abbreviations and names that read the same in every language (SoC, CPU, Heap, Flash, SSID, RSSI, MAC,
// DNS, NVS, OTA, KNX, Zigbee) and the device's own words (the reset reason, a refusal's reason, the
// power-save mode none/min/max as the setting wifi.ps names it) are shown as they are; every other word
// goes through t().

import { api, errorText } from "/api.js";
import { h, replace } from "/dom.js";
import { formatAgo, formatBytes, formatDuration, formatNumber } from "/format.js";
import { t, tv } from "/i18n.js";
import { label, pages } from "/model.js";

const kPollMs = 5000;

function rows(pairs) {
	return h("dl", { class: "rows" }, pairs.filter(Boolean).map(([label, value]) => [h("dt", null, label), h("dd", null, value)]));
}

function card(title, ...body) {
	return h("section", { class: "card" }, h("h2", null, title), ...body);
}

function badge(ok, text) {
	return h("span", { class: `badge ${ok ? "ok" : "warn"}` }, text);
}

/// A section the platform does not have: its reason.
function missing(section) {
	return h("p", { class: "muted" }, t("sys.na", { reason: section.reason || "—" }));
}

function of(used, total) {
	return t("system.storage.used.value", { used, total });
}

function memoryCard(d) {
	const m = d.memory || {};
	const temp = d.temperature || {};
	return card(t("sys.memory"), rows([
		m.available ? [t("status.heap"), t("status.heap.value", { free: formatBytes(m.free), min: formatBytes(m.min_free) })] : null,
		m.total ? ["Heap", formatBytes(m.total)] : null,
		m.largest ? [t("sys.largest"), formatBytes(m.largest)] : null,
		m.resident ? [t("sys.resident"), formatBytes(m.resident)] : null,
		[t("sys.temp"), temp.available
			? `${formatNumber(temp.celsius, "°C")}${temp.accuracy ? ` ± ${formatNumber(temp.accuracy, "°C", 0)}` : ""}`
			: t("sys.na", { reason: temp.reason || "—" })],
	]), m.available ? null : missing(m));
}

function chipCard(d) {
	const c = d.chip || {};
	if (!c.available) {
		return card(t("sys.chip"), missing(c));
	}
	return card(t("sys.chip"), rows([
		c.model ? ["SoC", [c.model, c.revision].filter(Boolean).join(" ")] : null,
		c.cores ? ["CPU", `${c.cores} × ${c.cpu_mhz ? `${c.cpu_mhz} MHz` : c.arch || ""}`] : null,
		c.flash_size ? ["Flash", [formatBytes(c.flash_size), c.flash_mode, c.flash_freq].filter(Boolean).join(", ")] : null,
		c.idf ? ["ESP-IDF", c.idf] : null,
		c.os ? ["OS", c.os] : null,
	]));
}

function firmwareCard(d) {
	const device = d.device || {};
	const boot = d.boot || {};
	const update = d.update || {};
	const chip = d.chip || {};
	return card(t("status.firmware"), rows([
		[t("status.firmware"), device.version || "—"],
		[t("status.build"), device.build || "—"],
		[t("status.ui"), device.ui || "—"],
		update.partition ? [t("status.update.partition"),
			[update.partition, chip.ota_state ? `OTA ${chip.ota_state}` : null].filter(Boolean).join(", ")] : null,
		[t("sys.reset"), boot.reason || "—"],
		boot.count !== undefined ? [t("sys.boots"), String(boot.count)] : null,
		boot.brownouts !== undefined ? [t("sys.brownouts"), String(boot.brownouts)] : null,
		boot.previous_uptime_s !== undefined ? [t("sys.prev"), formatDuration(boot.previous_uptime_s)] : null,
		[t("status.uptime"), formatDuration(d.uptime_s || 0)],
		[t("system.coredump"), boot.coredump
			? badge(false, [boot.coredump_task, boot.coredump_pc].filter(Boolean).join(" ") || t("status.yes"))
			: t("system.coredump.none")],
	]));
}

function loopCard(d) {
	const l = d.loop || {};
	const longest = l.longest_turn_ms !== undefined && (l.longest_turn_ms > 0 || l.longest_turn_ago_s !== undefined)
		? `${l.longest_turn_ms} ms, ${formatAgo((l.longest_turn_ago_s || 0) * 1000)}`
		: "—";
	return card(t("sys.loop"), rows([
		[t("sys.missed"), badge(!l.missed, String(l.missed ?? 0))],
		[t("sys.longest"), longest],
		l.stack_free !== undefined ? [t("sys.stack"), `${l.stack_free} B`] : null,
		[t("sys.timers"), `${l.timers ?? "—"} / ${l.timer_capacity ?? "—"}`],
		...consoleRows(d.console || {}),
	]));
}

/// The console output queue (issue #46): lines dropped although a host was reading are a warning;
/// those dropped while nobody read the port are only counted.
function consoleRows(c) {
	if (!c.available) {
		return [[t("sys.console.queue"), t("sys.na", { reason: c.reason || "—" })]];
	}
	const dropped = c.dropped ?? 0;
	const unread = c.dropped_unread ?? 0;
	return [
		[t("sys.console.dropped"), badge(dropped === unread,
			unread > 0 ? t("sys.console.dropped.value", { dropped, unread }) : String(dropped))],
		[t("sys.console.queue"), `${c.queued ?? 0} / ${c.capacity ?? 0} B`],
		[t("sys.console.host"), c.host ? t("status.yes") : t("status.no")],
	];
}

function networkCard(d) {
	const n = d.network || {};
	const w = d.wifi || {};
	return card(t("settings.network"), rows([
		[t("status.state"), [badge(n.state === "up", tv("state.link.", n.state)), n.state !== "up" && n.reason ? ` ${n.reason}` : null]],
		n.ip ? [t("status.ip"), n.ip] : null,
		n.mode && n.mode !== "normal" ? [t("status.mode"), tv("state.mode.", n.mode)] : null,
		n.pairing ? ["WPS", t("status.yes")] : null,
		n.up_for_s !== undefined ? [t("sys.up"), formatDuration(n.up_for_s)] : null,
		[t("sys.reconnects"), String(n.reconnects ?? 0)],
		w.ssid ? ["SSID", w.ssid] : null,
		w.channel ? [t("sys.channel"), String(w.channel)] : null,
		w.rssi !== undefined ? ["RSSI", `${w.rssi} dBm`] : null,
		w.tx_power_dbm !== undefined ? [t("sys.txpower"), formatNumber(w.tx_power_dbm, "dBm")] : null,
		w.power_save ? [t("settings.key.wifi.ps"), w.power_save] : null,
		w.mac ? ["MAC", w.mac] : null,
		w.gateway ? [t("sys.gateway"), w.gateway] : null,
		w.dns ? ["DNS", w.dns] : null,
	]), w.available ? null : missing(w));
}

function storageCard(d) {
	const s = d.storage || {};
	const nvs = s.nvs || {};
	return card(t("system.storage"), rows([
		s.total ? [t("system.storage.used"), of(formatBytes(s.used), formatBytes(s.total))] : null,
		s.files !== undefined ? [t("system.storage.files"), t("system.storage.files.value", { files: s.files, bytes: formatBytes(s.bytes) })] : null,
		["NVS", nvs.available ? t("sys.nvs.value", { used: nvs.used, total: nvs.total, ns: nvs.namespaces }) : t("sys.na", { reason: nvs.reason || "—" })],
	]), s.available ? null : missing(s));
}

function busCard(d) {
	const knx = d.knx || {};
	const zb = d.zigbee || {};
	return card(t("system.connections"), rows([
		["KNX", knx.available ? tv("system.knx.", knx.state) : t("sys.na", { reason: knx.reason })],
		knx.channel !== undefined ? [t("sys.channel"), String(knx.channel)] : null,
		knx.telegrams !== undefined ? [t("sys.telegrams"), String(knx.telegrams)] : null,
		knx.available ? [t("sys.reconnects"), String(knx.reconnects)] : null,
		["Zigbee", !zb.available ? t("sys.na", { reason: zb.reason })
			: zb.up ? t("zigbee.network.up", { channel: zb.channel, pan: (zb.pan_id || 0).toString(16).padStart(4, "0") })
			: t("zigbee.network.down")],
		zb.available ? [t("zigbee.devices"), String(zb.devices)] : null,
	]));
}

function statusCard(d) {
	const time = d.time || {};
	const improv = d.improv || {};
	return card(t("settings.status"), rows([
		[t("status.led"), tv("state.led.", d.led)],
		[t("status.local"), time.local || "—"],
		[t("status.synchronized"), badge(time.synchronized, time.synchronized ? t("status.yes") : t("status.not_synchronized"))],
		[t("status.improv"), improv.active ? t("status.improv.active", { seconds: improv.ends_in_s }) : t("status.improv.ended")],
	]));
}

function taskCard(d) {
	const tasks = d.tasks || {};
	if (!tasks.available) {
		return card(t("sys.tasks"), missing(tasks));
	}
	const list = [...(tasks.list || [])].sort((a, b) => (b.priority ?? -1) - (a.priority ?? -1) || a.name.localeCompare(b.name));
	const head = h("tr", null, h("th", null, t("system.tokens.name")), h("th", null, t("status.state")),
		h("th", { class: "num" }, t("sys.prio")), h("th", { class: "num" }, t("sys.stack")),
		tasks.cpu ? h("th", { class: "num" }, "CPU") : null);
	const body = list.map((task) => h("tr", { class: task.current ? "current" : null },
		h("td", null, task.name),
		h("td", null, tv("sys.ts.", task.state)),
		h("td", { class: "num" }, task.priority ?? "—"),
		h("td", { class: "num" }, task.stack_free !== undefined ? `${task.stack_free} B` : "—"),
		tasks.cpu ? h("td", { class: "num" }, task.cpu_permille !== undefined ? `${formatNumber(task.cpu_permille / 10, "%")}` : "—") : null));
	return h("section", { class: "card wide" }, h("h2", null, `${t("sys.tasks")} (${tasks.count})`),
		h("div", { class: "table-scroll" }, h("table", { class: "table" }, h("thead", null, head), h("tbody", null, body))),
		tasks.omitted ? h("p", { class: "muted" }, t("sys.omitted", { n: tasks.omitted })) : null,
		tasks.cpu ? null : h("p", { class: "muted" }, t("sys.cpu.na")));
}

/// A value of a block's port or state, with its unit.
function blockValue(value, unit) {
	if (value === null || value === undefined) {
		return t("value.unknown");
	}
	if (typeof value === "boolean") {
		return value ? t("value.on") : t("value.off");
	}
	return formatNumber(value, unit, Number.isInteger(value) ? 0 : 2);
}

/// One block (spec/blocks.md 9): status, fault and last problem, its calls, its inputs, outputs and
/// state; the enable switch and the restart of a faulted block (PUT /api/blocks/<id>).
function blockCard(block, nowMs, change) {
	const ok = block.status === "running";
	const ago = (ms) => formatAgo(nowMs - ms);
	const feedback = h("p", { class: "error-text", role: "status" });
	const send = (body) => change(block.id, body).catch((error) => {
		feedback.textContent = errorText(error);
	});
	const actions = [];
	if (block.status === "fault") {
		actions.push(h("button", { type: "button", class: "button small", onclick: () => send({ restart: true }) }, t("blocks.restart")));
	}
	if (block.enabledAdjustable) {
		actions.push(h("button", { type: "button", class: "button secondary small", onclick: () => send({ enabled: !block.enabled }) },
			block.enabled ? t("blocks.disable") : t("blocks.enable")));
	}
	const io = (list, inputs) => h("table", { class: "table" },
		h("tbody", null, list.map((port) => h("tr", null,
			h("td", null, port.id),
			h("td", { class: "num" }, blockValue(port.value, port.unit)),
			h("td", { class: "muted" }, inputs
				? ((port.datapoints || []).length
					? [port.datapoints.join(", "), port.quality && port.quality !== "good" ? ` (${tv("blocks.quality.", port.quality)})` : ""].join("")
					: t("blocks.unbound"))
				: (port.datapoint || (port.diagnostic ? t("blocks.diagnostic") : t("blocks.unbound"))))))));
	const state = Object.entries(block.state || {}).map(([k, v]) => `${k} ${formatNumber(v, "", Number.isInteger(v) ? 0 : 3)}`).join(" · ");
	return h("section", { class: "card wide block", "data-block": block.id },
		h("div", { class: "card-head" }, h("h2", null, label(block.label, block.id)), badge(ok, tv("blocks.status.", block.status))),
		rows([
			[t("blocks.type"), `${block.type} ${block.version}`],
			block.fault ? [t("blocks.fault"), h("span", { class: "error-text" }, `${block.fault.reason} · ${ago(block.fault.ms)}`)] : null,
			[t("blocks.calls"), block.lastCall
				? t("blocks.calls.value", { count: block.calls, last: [block.lastCall.kind, block.lastCall.arg].filter(Boolean).join(" "), ago: ago(block.lastCall.ms) })
				: String(block.calls)],
			[t("blocks.time"), t("blocks.time.value", { total: block.timeMs, longest: block.longestMs })],
			block.errors > 0 ? [t("rules.errors"), h("span", { class: "error-text" }, t("rules.errors_value", { count: block.errors }),
				block.lastError ? ` · ${block.lastError.code}: ${block.lastError.text} (${ago(block.lastError.ms)})` : "")] : null,
			state ? [t("blocks.state"), state] : null,
		]),
		h("h3", null, t("blocks.inputs")), io(block.inputs || [], true),
		h("h3", null, t("blocks.outputs")), io(block.outputs || [], false),
		actions.length ? h("div", { class: "actions" }, actions) : null,
		feedback);
}

/// The section "Blocks" (spec/blocks.md 12): nothing without blocks.
async function blockCards() {
	const all = [];
	let nowMs = 0;
	try {
		await pages("/api/blocks", (page) => {
			all.push(...page.blocks);
			nowMs = page.now_ms;
		});
	} catch (error) {
		return error && error.code === "image.unavailable" ? [] : [card(t("blocks.title"), h("p", { class: "error-text" }, errorText(error)))];
	}
	if (!all.length) {
		return [];
	}
	const change = async (id, body) => {
		await api("PUT", `/api/blocks/${encodeURIComponent(id)}`, body);
	};
	return [h("h2", { class: "section-title" }, t("blocks.title")), ...all.map((b) => blockCard(b, nowMs, change))];
}

/// One rule as the engine reports it (spec/rules.md 9.1, GET /api/rules): the printer's texts of
/// the last trigger, the last action and the last problem with their age; the Rules page shows
/// the inhabitant only when it was last triggered and the problem count (homecontrol #82).
function ruleCard(rule, nowMs) {
	const ago = (ms) => formatAgo(nowMs - ms);
	const event = (e) => (e ? `${e.text} · ${ago(e.ms)}` : t("rules.never"));
	const name = label(rule.name, "");
	return h("section", { class: "card rule-diag", "data-rule": rule.id },
		h("div", { class: "card-head" },
			h("h2", null, name || rule.id, name && name !== rule.id ? h("span", { class: "section-sub" }, rule.id) : null),
			badge(rule.enabled, rule.enabled ? t("rules.enabled") : t("rules.disabled"))),
		rows([
			rule.state ? [t("rules.state"), rule.state] : null,
			[t("rules.last_trigger"), event(rule.lastTrigger)],
			[t("rules.last_action"), event(rule.lastAction)],
			[t("rules.errors"), rule.errors > 0
				? h("span", { class: "error-text" }, t("rules.errors_value", { count: rule.errors }),
					rule.lastError ? ` · ${rule.lastError.code}: ${rule.lastError.text} (${ago(rule.lastError.ms)})` : "")
				: String(rule.errors ?? 0)],
		]));
}

/// The section "Rules": nothing without rules.
async function ruleCards() {
	const all = [];
	let nowMs = 0;
	try {
		await pages("/api/rules", (page) => {
			all.push(...page.rules);
			nowMs = page.now_ms;
		});
	} catch (error) {
		return error && error.code === "image.unavailable" ? [] : [card(t("rules.title"), h("p", { class: "error-text" }, errorText(error)))];
	}
	if (!all.length) {
		return [];
	}
	return [h("h2", { class: "section-title" }, t("rules.title")), ...all.map((r) => ruleCard(r, nowMs))];
}

export function renderDiagnostics(content) {
	const cards = h("div", { class: "cards" }, h("p", { class: "muted" }, t("app.loading")));
	const message = h("p", { class: "message error", role: "alert", hidden: true });
	let timer = null;
	let stopped = false;

	async function poll() {
		try {
			const d = await api("GET", "/api/system/diagnostics");
			const rules = await ruleCards();
			const blocks = await blockCards();
			if (stopped) {
				return;
			}
			message.hidden = true;
			replace(cards, memoryCard(d), loopCard(d), chipCard(d), firmwareCard(d), networkCard(d), storageCard(d),
				busCard(d), statusCard(d), taskCard(d), ...rules, ...blocks);
		} catch (error) {
			message.textContent = errorText(error);
			message.hidden = false;
		}
		if (!stopped) {
			timer = setTimeout(poll, kPollMs);
		}
	}

	content.appendChild(message);
	content.appendChild(cards);
	poll();
	return () => {
		stopped = true;
		clearTimeout(timer);
	};
}
