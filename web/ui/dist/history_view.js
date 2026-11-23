/* Copyright (c) 2026 Bernhard Haumacher */
// What the UI draws of a datapoint's history (homecontrol #115): E-Home sets at the datapoint how far
// back it is recorded and whether it is a meter reading; GET /api/model gives both (`coverage` in
// seconds, `meter`). The generic view offers exactly the ranges the coverage reaches and draws by the
// kind of the value: a number as its line with the min/max band, a boolean as the on/off strip, a
// meter as consumption bars. Pure functions, tested with node (web/ui/test/history_view.test.mjs).

/// The chart ranges of the image (spec/runtime-image.md 3.9) in seconds; a year is 365 days.
export const RANGE_SECONDS = {
	"1h": 3600,
	"6h": 6 * 3600,
	"24h": 86400,
	"7d": 7 * 86400,
	"30d": 30 * 86400,
	"1y": 365 * 86400,
	"10y": 3650 * 86400,
};

/// Consumption bars per range: their count and width (s), aligned to the local hour or day.
export const BARS = {
	"1h": [12, 300],
	"6h": [24, 900],
	"24h": [24, 3600],
	"7d": [7, 86400],
	"30d": [30, 86400],
	"1y": [12, 365 * 86400 / 12],
	"10y": [10, 365 * 86400],
};

/// A line, band or strip is drawn at this many points per range.
export const LINE_POINTS = 48;

/// The ranges the generic view offers, shortest first ("1h" stays valid on the pages of older images).
export const GENERIC_RANGES = ["6h", "24h", "7d", "30d", "1y", "10y"];

/// The generic ranges a history of `coverage` seconds reaches. A history shorter than the shortest
/// range (an image of an older E-Home) gets the shortest, which it fills as far as it reaches.
export function coveredRanges(coverage) {
	if (!(coverage > 0)) {
		return [];
	}
	const covered = GENERIC_RANGES.filter((range) => RANGE_SECONDS[range] <= coverage);
	return covered.length ? covered : [GENERIC_RANGES[0]];
}

/// The range shown first: 24 h where it is offered, else the shortest offered.
export function firstRange(ranges) {
	return ranges.includes("24h") ? "24h" : ranges[0];
}

/// How the history of a datapoint (its /api/model entry) is drawn: "strip", "bars" or "band".
export function drawKind(info) {
	if (!info) {
		return "band";
	}
	if (info.type === "boolean") {
		return "strip";
	}
	return info.meter ? "bars" : "band";
}

/// The series' consolidation of a chart (spec/runtime-image.md 3.9) for the kind drawn.
export function consolidationOf(info) {
	switch (drawKind(info)) {
		case "strip": return "mean";
		case "bars": return "consumption";
		default: return "band";
	}
}

/// The seconds a tile's sparkline spans: 24 h, or the coverage where it is shorter.
export function sparkSeconds(info) {
	const coverage = info && info.coverage;
	return coverage > 0 ? Math.min(86400, coverage) : 86400;
}

/// Whether a tile shows a sparkline of the datapoint: it keeps a history.
export function hasSpark(info) {
	return !!(info && info.history);
}
