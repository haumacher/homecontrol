/* Copyright (c) 2026 Bernhard Haumacher */
// Formats datapoint values (spec/api.md "Values") and durations for display.

import { t, language } from "/i18n.js";

// Number and date formats follow the UI language (created on first use per language).
const formats = new Map();

function format(kind, options) {
	const key = `${language()}|${kind}`;
	let formatter = formats.get(key);
	if (!formatter) {
		let locale = language();
		try {
			formatter = kind.startsWith("date") ? new Intl.DateTimeFormat(locale, options) : new Intl.NumberFormat(locale, options);
		} catch (e) {
			locale = undefined;
			formatter = kind.startsWith("date") ? new Intl.DateTimeFormat(locale, options) : new Intl.NumberFormat(locale, options);
		}
		formats.set(key, formatter);
	}
	return formatter;
}

const number = { format: (value) => format("number2", { maximumFractionDigits: 2 }).format(value) };

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

/// "3 min ago" for an age in ms.
export function formatAgo(ms) {
	if (ms === null || ms === undefined) {
		return "";
	}
	if (ms < 45000) {
		return t("ago.now");
	}
	return t("ago.past", { duration: formatDurationShort(ms / 1000) });
}

/// "in 3 h" / "3 h ago" relative to now, both in seconds.
export function formatRelative(seconds, now) {
	const delta = seconds - now;
	if (Math.abs(delta) < 45) {
		return t("ago.now");
	}
	return delta > 0 ? t("ago.future", { duration: formatDurationShort(delta) }) : t("ago.past", { duration: formatDurationShort(-delta) });
}

/// The largest unit only: "40 s", "3 min", "5 h", "2 d".
export function formatDurationShort(seconds) {
	const s = Math.round(seconds);
	if (s < 60) {
		return t("duration.s", { s });
	}
	if (s < 3600) {
		return t("duration.m", { m: Math.round(s / 60) });
	}
	if (s < 86400 * 2) {
		return t("duration.hh", { h: Math.round(s / 3600) });
	}
	return t("duration.dd", { d: Math.round(s / 86400) });
}

const dateTime = { format: (date) => format("date-short", { weekday: "short", hour: "2-digit", minute: "2-digit" }).format(date) };
const fullDate = { format: (date) => format("date-full", { dateStyle: "medium", timeStyle: "short" }).format(date) };

/// A UTC time (seconds) as local weekday and time, or with the date when it is more than 6 days away.
export function formatWhen(seconds, now) {
	const date = new Date(seconds * 1000);
	return Math.abs(seconds - (now || Date.now() / 1000)) > 6 * 86400 ? fullDate.format(date) : dateTime.format(date);
}

/// A time axis tick, as short as the span of the axis allows: the time up to a day, weekday and
/// time up to six days, the day and month up to 60 days, month and year beyond.
export function formatTick(seconds, span) {
	const date = new Date(seconds * 1000);
	if (span <= 86400) {
		return format("date-tick-time", { hour: "2-digit", minute: "2-digit" }).format(date);
	}
	if (span <= 6 * 86400) {
		return dateTime.format(date);
	}
	if (span <= 60 * 86400) {
		return format("date-tick-day", { day: "numeric", month: "short" }).format(date);
	}
	return format("date-tick-month", { month: "short", year: "numeric" }).format(date);
}

/// A number with a unit, at most `digits` fraction digits.
export function formatNumber(value, unit, digits = 1) {
	if (value === null || value === undefined || Number.isNaN(value)) {
		return t("value.unknown");
	}
	const text = format(`number${digits}`, { maximumFractionDigits: digits }).format(value);
	return unit ? `${text} ${unit}` : text;
}
