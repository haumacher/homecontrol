/* Copyright (c) 2026 Bernhard Haumacher */
// The detail sheet of a function (a modal <dialog>: Escape closes it, focus stays inside): the
// values of its datapoints with quality and age, the history over 24 h or 7 days for the
// datapoints that keep one, and how each datapoint is bound to its technology.

import { api, errorText } from "/api.js";
import { h, icon, icons, replace } from "/dom.js";
import { chart } from "/chart.js";
import { formatAgo, formatNumber, formatValue } from "/format.js";
import { t, has } from "/i18n.js";
import { label, dpLabel, dpUnit } from "/model.js";

const RANGES = [
	{ key: "history.day", seconds: 86400, points: 96 },
	{ key: "history.week", seconds: 7 * 86400, points: 140 },
];

/// "KNX · DPT 9.001 · sends 1/3/2 · listens 1/3/1", or the binding's JSON for other technologies.
function bindingText(info) {
	if (!info) {
		return "";
	}
	const parts = [];
	const binding = info.binding || {};
	if (binding.knx) {
		const knx = binding.knx;
		parts.push(t("binding.knx"));
		if (knx.dpt) {
			parts.push(t("binding.dpt", { dpt: knx.dpt }));
		}
		if (knx.send) {
			parts.push(t("binding.send", { address: knx.send }));
		}
		if (knx.listen && knx.listen.length) {
			parts.push(t("binding.listen", { addresses: knx.listen.join(", ") }));
		}
	}
	for (const [name, value] of Object.entries(binding)) {
		if (name === "knx") {
			continue;
		}
		parts.push(name === "virtual" ? t("binding.virtual") : `${name} ${JSON.stringify(value)}`);
	}
	if (!parts.length && info.technology) {
		parts.push(has(`binding.${info.technology}`) ? t(`binding.${info.technology}`) : info.technology);
	}
	return parts.join(" · ") || t("binding.none");
}

function roleName(role) {
	return has(`role.${role}`) ? t(`role.${role}`) : role;
}

export function openSheet(fn, env) {
	const { live, model } = env;
	const roles = Object.entries(fn.datapoints || {}).filter(([, id]) => model.datapoints.has(id));
	const room = fn.room && model.locationsById.get(fn.room);
	const dialog = h("dialog", { class: "sheet", "aria-labelledby": "sheet-title" });
	const close = () => dialog.close();
	const rows = new Map();

	const valueText = (id) => {
		const dp = live.datapoints.get(id);
		if (!dp) {
			return t("value.unknown");
		}
		let text = dp.quality === "unknown" || dp.value === null ? t("value.unknown")
			: typeof dp.value === "number" ? formatNumber(dp.value, dpUnit(model, id) || ((fn.unit && roles.length === 1) ? fn.unit : ""), 2)
			: formatValue(dp);
		const ago = live.ageMs(dp);
		if (dp.quality === "stale") {
			text += ` · ${t("tile.last_known", { ago: formatAgo(ago) })}`;
		} else if (ago !== null && dp.quality === "good") {
			text += ` · ${formatAgo(ago)}`;
		}
		return text;
	};

	const values = h("dl", { class: "rows sheet-values" }, roles.map(([role, id]) => {
		const value = h("dd", null, valueText(id));
		rows.set(id, value);
		return [h("dt", null, roleName(role), h("small", { class: "muted block" }, dpLabel(model, id, live))), value];
	}));

	const bindings = h("dl", { class: "rows" }, roles.map(([role, id]) => {
		const info = model.datapoints.get(id);
		return [h("dt", null, roleName(role)), h("dd", null, h("code", null, info ? info.name : String(id)), " ", bindingText(info))];
	}));

	// History: every datapoint of the function that keeps one (each id once).
	const historyIds = [...new Set(roles.map(([, id]) => id))].filter((id) => (model.datapoints.get(id) || {}).history);
	let range = RANGES[0];
	const historyBody = h("div", null);
	const tabs = h("div", { class: "segmented", role: "group", "aria-label": t("history.range") }, RANGES.map((r) =>
		h("button", { type: "button", "aria-pressed": r === range ? "true" : "false", onclick: (event) => {
			range = r;
			for (const b of tabs.children) {
				b.setAttribute("aria-pressed", b === event.currentTarget ? "true" : "false");
			}
			loadHistory();
		} }, t(r.key))));

	async function loadHistory() {
		replace(historyBody, h("p", { class: "muted" }, t("app.loading")));
		const to = Math.floor(Date.now() / 1000) + 1;
		const from = to - range.seconds;
		try {
			const charts = [];
			for (const id of historyIds) {
				const data = await api("GET", `/api/history?ids=${id}&from=${from}&to=${to}&points=${range.points}`);
				const series = data.series[0];
				const dp = live.datapoints.get(id);
				charts.push(h("h3", { class: "sheet-subtitle" }, dpLabel(model, id, live)));
				charts.push(series.points
					? chart(series.points, { from, to, unit: dpUnit(model, id), step: dp && dp.type === "boolean", digits: dpUnit(model, id) === "%" ? 0 : 1, width: historyBody.clientWidth })
					: h("p", { class: "muted" }, errorText({ code: series.code, reason: series.error })));
			}
			replace(historyBody, charts);
		} catch (error) {
			replace(historyBody, h("p", { class: "message error" }, errorText(error)));
		}
	}

	dialog.append(h("div", { class: "sheet-body" },
		h("header", { class: "sheet-head" },
			h("div", null,
				h("h2", { id: "sheet-title" }, label(fn.label, fn.id)),
				h("p", { class: "muted" }, [has(`kind.${fn.kind}`) ? t(`kind.${fn.kind}`) : fn.kind, room ? label(room.label, room.id) : null].filter(Boolean).join(" · "))),
			h("button", { type: "button", class: "icon-button", "aria-label": t("sheet.close"), onclick: close }, icon(icons.close))),
		h("section", null, h("h3", { class: "sheet-subtitle" }, t("sheet.values")), values),
		historyIds.length ? h("section", null, h("div", { class: "sheet-row" }, h("h3", { class: "sheet-subtitle" }, t("sheet.history")), tabs), historyBody) : null,
		h("section", null, h("h3", { class: "sheet-subtitle" }, t("sheet.binding")), bindings),
	));

	const onDatapoints = (event) => {
		for (const [id, cell] of rows) {
			if (event.detail.all || event.detail.ids.includes(id)) {
				cell.textContent = valueText(id);
			}
		}
	};
	live.addEventListener("datapoints", onDatapoints);
	dialog.addEventListener("close", () => {
		live.removeEventListener("datapoints", onDatapoints);
		dialog.remove();
	});
	dialog.addEventListener("click", (event) => {
		if (event.target === dialog) {
			close(); // a tap on the backdrop
		}
	});
	document.body.appendChild(dialog);
	dialog.showModal();
	if (historyIds.length) {
		loadHistory();
	}
	return dialog;
}
