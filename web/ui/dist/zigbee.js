/* Copyright (c) 2026 Bernhard Haumacher */
// Zigbee: the coordinator's network and devices (GET /api/zigbee, spec/api.md "Zigbee"). "Pair"
// opens the network for 254 s with a countdown; joins and interviews show as they happen (the live
// feed's zigbee messages). Per device: identify, rename, interview again, remove. Values come from
// the live datapoints, by name.

import { api, errorText } from "/api.js";
import { h, replace } from "/dom.js";
import { formatAgo, formatValue } from "/format.js";
import { t, tv } from "/i18n.js";

const kPairSeconds = 254;

function deviceTitle(d) {
	return d.label || d.model || d.ieee;
}

function stateBadge(d) {
	if (d.interviewing) {
		return h("span", { class: "badge warn" }, t("zigbee.state.interviewing"));
	}
	if (d.left) {
		return h("span", { class: "badge warn" }, t("zigbee.state.left"));
	}
	if (!d.interviewed) {
		return h("span", { class: "badge warn" }, t("zigbee.state.new"));
	}
	if (d.stale) {
		return h("span", { class: "badge warn" }, t("zigbee.state.stale"));
	}
	return h("span", { class: "badge ok" }, t("zigbee.state.ok"));
}

export function renderZigbee(content, ctx) {
	const message = h("p", { class: "message error", role: "alert", hidden: true });
	const network = h("section", { class: "card" }, h("p", { class: "muted" }, t("app.loading")));
	const activity = h("ul", { class: "event-list" });
	const devices = h("div", { class: "cards" });
	let data = null;
	let permitUntil = 0;
	let ticker = null;
	let loading = false;
	let again = false;
	let stopped = false;

	const fail = (error) => {
		message.textContent = errorText(error);
		message.hidden = false;
	};

	/// Runs a device or network operation; a refusal is shown with its reason.
	async function act(button, method, path, body) {
		button.disabled = true;
		try {
			await api(method, path, body);
			message.hidden = true;
			load();
		} catch (error) {
			fail(error);
		} finally {
			button.disabled = false;
		}
	}

	function byName() {
		const map = new Map();
		for (const dp of ctx.live.datapoints.values()) {
			map.set(dp.name, dp);
		}
		return map;
	}

	function deviceCard(d, values) {
		const path = `/api/zigbee/devices/${d.ieee}`;
		const seen = d.seen_ago_s !== undefined ? formatAgo(d.seen_ago_s * 1000)
			: d.last_seen ? formatAgo(Date.now() - d.last_seen * 1000) : t("zigbee.never");
		const identify = h("button", { type: "button", class: "button small secondary", disabled: !d.interviewed,
			onclick: () => act(identify, "POST", `${path}/identify`) }, t("zigbee.identify"));
		const rename = h("button", { type: "button", class: "button small secondary", onclick: () => {
			const label = window.prompt(t("zigbee.rename.prompt"), d.label || "");
			if (label !== null) {
				act(rename, "PUT", `${path}/label`, { label: label.trim() });
			}
		} }, t("zigbee.rename"));
		const interview = h("button", { type: "button", class: "button small secondary",
			onclick: () => act(interview, "POST", `${path}/interview`) }, t("zigbee.interview"));
		const remove = h("button", { type: "button", class: "button small danger", onclick: () => {
			if (window.confirm(t("zigbee.remove.confirm", { name: deviceTitle(d) }))) {
				act(remove, "DELETE", path);
			}
		} }, t("zigbee.remove"));
		const points = d.datapoints.map((name) => {
			const dp = values.get(name);
			return [name.replace(`zigbee.${d.ieee}.`, ""), dp ? formatValue(dp) : t("value.unknown")];
		});
		return h("section", { class: "card" },
			h("h2", null, deviceTitle(d), " ", stateBadge(d)),
			h("dl", { class: "rows" },
				[[t("zigbee.device"), `${d.manufacturer || "?"} / ${d.model || "?"}`], [t("zigbee.ieee"), d.ieee],
					[t("zigbee.seen"), seen], ...points].map(([k, v]) => [h("dt", null, k), h("dd", null, v)])),
			h("div", { class: "actions" }, identify, rename, interview, remove));
	}

	function drawDevices() {
		if (!data) {
			return;
		}
		const values = byName();
		if (data.devices.length === 0) {
			replace(devices, h("p", { class: "empty" }, t("zigbee.empty")));
			return;
		}
		replace(devices, data.devices.map((d) => deviceCard(d, values)));
	}

	function drawNetwork() {
		if (!data) {
			return;
		}
		const n = data.network;
		const left = Math.max(0, Math.ceil((permitUntil - Date.now()) / 1000));
		const pair = h("button", { type: "button", class: "button", disabled: !n.up,
			onclick: () => act(pair, "POST", "/api/zigbee/permit", { seconds: kPairSeconds }) },
		left > 0 ? t("zigbee.pair.again") : t("zigbee.pair"));
		const close = h("button", { type: "button", class: "button secondary",
			onclick: () => act(close, "POST", "/api/zigbee/permit", { seconds: 0 }) }, t("zigbee.pair.close"));
		replace(network,
			h("h2", null, t("zigbee.network")),
			h("dl", { class: "rows" }, [
				[t("status.state"), n.up ? t("zigbee.network.up", { channel: n.channel, pan: (n.pan_id || 0).toString(16) })
					: t("zigbee.network.down")],
				[t("zigbee.devices"), t("zigbee.devices.value", { count: data.count, max: data.max_devices })],
				[t("zigbee.joining"), left > 0 ? t("zigbee.joining.open", { seconds: left }) : t("zigbee.joining.closed")],
			].map(([k, v]) => [h("dt", null, k), h("dd", null, v)])),
			h("p", { class: "hint" }, left > 0 ? t("zigbee.pair.hint") : t("zigbee.pair.intro")),
			h("div", { class: "actions" }, pair, left > 0 ? close : null),
			activity.childElementCount ? activity : null);
		clearTimeout(ticker);
		if (left > 0 && !stopped) {
			ticker = setTimeout(drawNetwork, 1000);
		}
	}

	async function load() {
		if (loading) {
			again = true;
			return;
		}
		loading = true;
		try {
			let first = null;
			const all = [];
			let from = 0;
			do {
				const page = await api("GET", `/api/zigbee?from=${from}`);
				first = first || page;
				all.push(...page.devices);
				from = page.next;
			} while (from !== null && from !== undefined && !stopped);
			data = { ...first, devices: all };
			permitUntil = Date.now() + first.network.permit_join_s * 1000;
			message.hidden = true;
			drawNetwork();
			drawDevices();
		} catch (error) {
			if (error.code === "system.unavailable") {
				replace(network, h("h2", null, t("zigbee.network")), h("p", { class: "muted" }, t("zigbee.unavailable", { reason: error.reason || "" })));
			} else {
				fail(error);
			}
		} finally {
			loading = false;
			if (again && !stopped) {
				again = false;
				load();
			}
		}
	}

	// The live feed: an event names what happened; the list is read again for the details.
	const onZigbee = (event) => {
		const m = event.detail;
		if (m.event === "permit") {
			permitUntil = Date.now() + (m.permit_join_s || 0) * 1000;
		}
		if (m.ieee) {
			const known = data && data.devices.find((d) => d.ieee === m.ieee);
			activity.insertBefore(h("li", null, `${new Date().toLocaleTimeString()} `,
				tv("zigbee.event.", m.event), " ", known ? deviceTitle(known) : m.ieee), activity.firstChild);
			while (activity.childElementCount > 8) {
				activity.lastChild.remove();
			}
		}
		load();
	};
	// A snapshot (after a reconnect or a missed message) comes in chunks: read the list once after it.
	let snapshotTimer = null;
	const onValues = (event) => {
		if (event.detail && event.detail.all) {
			clearTimeout(snapshotTimer);
			snapshotTimer = setTimeout(load, 300);
		} else {
			drawDevices();
		}
	};
	const onState = (event) => event.detail.state === "connected" && load();
	ctx.live.addEventListener("zigbee", onZigbee);
	ctx.live.addEventListener("datapoints", onValues);
	ctx.live.addEventListener("state", onState);
	load();

	content.appendChild(message);
	content.appendChild(h("p", { class: "intro muted" }, t("zigbee.intro")));
	content.appendChild(h("div", { class: "cards" }, network));
	content.appendChild(h("h2", { class: "section-title" }, t("zigbee.devices")));
	content.appendChild(devices);
	return () => {
		stopped = true;
		clearTimeout(ticker);
		clearTimeout(snapshotTimer);
		ctx.live.removeEventListener("zigbee", onZigbee);
		ctx.live.removeEventListener("datapoints", onValues);
		ctx.live.removeEventListener("state", onState);
	};
}
