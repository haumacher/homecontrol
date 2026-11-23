/* Copyright (c) 2026 Bernhard Haumacher */
// The detail sheet of a function (a modal <dialog>: Escape closes it, focus stays inside): the
// values of its datapoints with quality and age, and a chart of each datapoint that keeps a history,
// over exactly the ranges its coverage reaches, drawn by its kind (homecontrol #115: a number as its
// line with the min/max band, a boolean as the on/off strip, a meter as consumption bars). Each
// datapoint is named by its role and its label (its id only without a label); the technical name and
// the binding are the technical pages' business (homecontrol #82).

import { h, icon, icons } from "/dom.js";
import { chartTile } from "/elements.js";
import { formatAgo, formatNumber, formatValue } from "/format.js";
import { t, has } from "/i18n.js";
import { label, dpLabel, dpUnit } from "/model.js";
import { consolidationOf, coveredRanges, drawKind, firstRange } from "/history_view.js";

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
		if (dp.pending) {
			text += ` · ${t("value.waiting")}`;
		}
		return text;
	};

	const values = h("dl", { class: "rows sheet-values" }, roles.map(([role, id]) => {
		const value = h("dd", null, valueText(id));
		rows.set(id, value);
		return [h("dt", null, roleName(role), h("small", { class: "muted block" }, dpLabel(model, id, live))), value];
	}));

	// History: every datapoint of the function that keeps one (each id once), its ranges by its coverage.
	const charts = [...new Set(roles.map(([, id]) => id))]
		.map((id) => [id, model.datapoints.get(id) || {}])
		.filter(([, info]) => info.history)
		.map(([id, info]) => {
			const ranges = coveredRanges(info.coverage);
			return chartTile({
				label: dpLabel(model, id, live),
				chart: { range: firstRange(ranges), presets: ranges, height: drawKind(info) === "strip" ? 2 : 4,
					series: [{ datapoint: id, consolidation: consolidationOf(info) }] },
			}, env);
		});

	dialog.append(h("div", { class: "sheet-body" },
		h("header", { class: "sheet-head" },
			h("div", null,
				h("h2", { id: "sheet-title" }, label(fn.label, fn.id)),
				h("p", { class: "muted" }, [has(`kind.${fn.kind}`) ? t(`kind.${fn.kind}`) : fn.kind, room ? label(room.label, room.id) : null].filter(Boolean).join(" · "))),
			h("button", { type: "button", class: "icon-button", "aria-label": t("sheet.close"), onclick: close }, icon(icons.close))),
		h("section", null, h("h3", { class: "sheet-subtitle" }, t("sheet.values")), values),
		charts.length ? h("section", { class: "sheet-history" }, h("h3", { class: "sheet-subtitle" }, t("sheet.history")),
			charts.map((chart) => chart.element)) : null,
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
		charts.forEach((chart) => chart.dispose());
		dialog.remove();
	});
	dialog.addEventListener("click", (event) => {
		if (event.target === dialog) {
			close(); // a tap on the backdrop
		}
	});
	document.body.appendChild(dialog);
	dialog.showModal();
	return dialog;
}
