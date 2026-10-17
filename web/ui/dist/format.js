/* Copyright (c) 2026 Bernhard Haumacher */
// Formats datapoint values (spec/api.md "Values") and durations for display.

import { t } from "/i18n.js";

const number = new Intl.NumberFormat(undefined, { maximumFractionDigits: 2 });

function two(n) {
	return String(n).padStart(2, "0");
}

export function formatValue(dp) {
	const v = dp.value;
	if (v === null || v === undefined || dp.quality === "unknown") {
		return t("value.unknown");
	}
	switch (dp.type) {
		case "boolean":
			return v ? t("value.on") : t("value.off");
		case "unsigned":
		case "signed":
		case "real":
		case "enumeration":
			return number.format(v);
		case "string":
		case "raw":
			return String(v);
		case "time of day":
			return `${two(v.hour)}:${two(v.minute)}:${two(v.second)}`;
		case "date":
			return `${v.year}-${two(v.month)}-${two(v.day)}`;
		case "date-time":
			return `${v.year}-${two(v.month)}-${two(v.day)} ${two(v.hour)}:${two(v.minute)}:${two(v.second)}`;
		case "controlled":
			return `${v.control ? t("value.control") + " " : ""}${v.value}`;
		case "color":
			return `#${[v.red, v.green, v.blue].map((c) => c.toString(16).padStart(2, "0")).join("")}`;
		default:
			return JSON.stringify(v);
	}
}

export function formatDuration(seconds) {
	const s = Math.floor(seconds);
	if (s < 60) {
		return t("duration.s", { s });
	}
	const m = Math.floor(s / 60);
	if (m < 60) {
		return t("duration.m", { m });
	}
	const h = Math.floor(m / 60);
	if (h < 24) {
		return t("duration.h", { h, m: m % 60 });
	}
	return t("duration.d", { d: Math.floor(h / 24), h: h % 24 });
}

export function formatBytes(bytes) {
	if (!bytes) {
		return "—";
	}
	return `${number.format(bytes / 1024)} KB`;
}
