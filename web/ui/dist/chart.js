/* Copyright (c) 2026 Bernhard Haumacher */
// History drawn as inline SVG (GET /api/history: points [t, last] or [t, last, min, max, count]):
// a sparkline for the tiles and a chart with a min/max band, axes and a hover read-out for the
// detail sheet. One series each, in the accent colour; the text stays in the text colours.

import { h } from "/dom.js";
import { formatNumber, formatTick, formatWhen } from "/format.js";
import { t } from "/i18n.js";

const SVG = "http://www.w3.org/2000/svg";

function svg(tag, attributes) {
	const element = document.createElementNS(SVG, tag);
	for (const [name, value] of Object.entries(attributes || {})) {
		element.setAttribute(name, String(value));
	}
	return element;
}

function bounds(points) {
	let lo = Infinity;
	let hi = -Infinity;
	for (const p of points) {
		lo = Math.min(lo, p.length > 2 ? p[2] : p[1]);
		hi = Math.max(hi, p.length > 2 ? p[3] : p[1]);
	}
	if (lo === hi) {
		lo -= 1;
		hi += 1;
	}
	return [lo, hi];
}

/// The path through the points' last values; `step` holds each value until the next point.
function linePath(points, x, y, step) {
	let d = "";
	points.forEach((p, i) => {
		const px = x(p[0]).toFixed(1);
		const py = y(p[1]).toFixed(1);
		if (i === 0) {
			d += `M${px} ${py}`;
		} else if (step) {
			d += `H${px}V${py}`;
		} else {
			d += `L${px} ${py}`;
		}
	});
	return d;
}

/// A small line without axes (aria-hidden: the tile shows the value as text).
export function sparkline(points, from, to, step) {
	const width = 120;
	const height = 32;
	if (!points || points.length === 0) {
		return h("span", { class: "spark-empty" }, t("history.none_yet"));
	}
	if (points.length === 1) {
		points = [points[0], [to, points[0][1]]]; // one bucket so far: its value up to now
	}
	const root = svg("svg", { class: "sparkline", viewBox: `0 0 ${width} ${height}`, preserveAspectRatio: "none", "aria-hidden": "true" });
	const [lo, hi] = bounds(points);
	const x = (time) => ((time - from) / (to - from)) * width;
	const y = (v) => height - 3 - ((v - lo) / (hi - lo)) * (height - 6);
	root.appendChild(svg("path", { d: linePath(points, x, y, step), class: "spark-line", "vector-effect": "non-scaling-stroke" }));
	return root;
}

/// A chart of one series over [from, to) with a read-out on hover and touch.
export function chart(points, { from, to, unit, step, digits = 1, width: available }) {
	const wrap = h("div", { class: "chart" });
	if (!points || points.length === 0) {
		wrap.appendChild(h("p", { class: "muted" }, t("history.empty")));
		return wrap;
	}
	// Drawn at the width it is shown at, so the labels keep their size on a phone.
	const width = Math.max(280, Math.round(available || 640));
	const height = 200;
	const left = 52;
	const right = 8;
	const top = 10;
	const bottom = 24;
	const [lo0, hi0] = bounds(points);
	const pad = (hi0 - lo0) * 0.08;
	const lo = lo0 - pad;
	const hi = hi0 + pad;
	const x = (time) => left + ((time - from) / (to - from)) * (width - left - right);
	const y = (v) => top + (1 - (v - lo) / (hi - lo)) * (height - top - bottom);
	const last = points[points.length - 1][1];
	const root = svg("svg", { viewBox: `0 0 ${width} ${height}`, role: "img",
		"aria-label": t("history.summary", { min: formatNumber(lo0, unit, digits), max: formatNumber(hi0, unit, digits), last: formatNumber(last, unit, digits) }) });

	// Grid and axis labels: three value lines, four time marks.
	for (let i = 0; i <= 2; i++) {
		const v = lo0 + ((hi0 - lo0) * i) / 2;
		root.appendChild(svg("line", { x1: left, x2: width - right, y1: y(v), y2: y(v), class: "chart-grid" }));
		const label = svg("text", { x: left - 6, y: y(v) + 4, class: "chart-label", "text-anchor": "end" });
		label.textContent = formatNumber(v, "", digits);
		root.appendChild(label);
	}
	for (let i = 0; i <= 3; i++) {
		const time = from + ((to - from) * i) / 3;
		const label = svg("text", { x: x(time), y: height - 6, class: "chart-label", "text-anchor": i === 0 ? "start" : i === 3 ? "end" : "middle" });
		label.textContent = formatWhen(time, to);
		root.appendChild(label);
	}
	// The min/max band of decimated buckets, then the line.
	const banded = points.filter((p) => p.length > 2);
	if (banded.length > 1) {
		const upper = banded.map((p) => `${x(p[0]).toFixed(1)} ${y(p[3]).toFixed(1)}`);
		const lower = banded.slice().reverse().map((p) => `${x(p[0]).toFixed(1)} ${y(p[2]).toFixed(1)}`);
		root.appendChild(svg("path", { d: `M${upper.join("L")}L${lower.join("L")}Z`, class: "chart-band" }));
	}
	root.appendChild(svg("path", { d: linePath(points, x, y, step), class: "chart-line" }));
	const cross = svg("line", { y1: top, y2: height - bottom, class: "chart-cross", visibility: "hidden" });
	const dot = svg("circle", { r: 4, class: "chart-dot", visibility: "hidden" });
	root.appendChild(cross);
	root.appendChild(dot);
	const tip = h("div", { class: "chart-tip", hidden: true });

	const show = (event) => {
		const box = root.getBoundingClientRect();
		const sx = ((event.clientX - box.left) / box.width) * width;
		let best = points[0];
		for (const p of points) {
			if (Math.abs(x(p[0]) - sx) < Math.abs(x(best[0]) - sx)) {
				best = p;
			}
		}
		const px = x(best[0]);
		cross.setAttribute("x1", px);
		cross.setAttribute("x2", px);
		dot.setAttribute("cx", px);
		dot.setAttribute("cy", y(best[1]));
		cross.setAttribute("visibility", "visible");
		dot.setAttribute("visibility", "visible");
		tip.hidden = false;
		tip.textContent = best.length > 2
			? t("history.tip.range", { when: formatWhen(best[0], to), value: formatNumber(best[1], unit, digits), min: formatNumber(best[2], "", digits), max: formatNumber(best[3], "", digits) })
			: t("history.tip", { when: formatWhen(best[0], to), value: formatNumber(best[1], unit, digits) });
		const fraction = px / width;
		if (fraction > 0.5) {
			tip.style.left = "auto";
			tip.style.right = `${(1 - fraction) * 100}%`;
		} else {
			tip.style.right = "auto";
			tip.style.left = `${fraction * 100}%`;
		}
	};
	const hide = () => {
		cross.setAttribute("visibility", "hidden");
		dot.setAttribute("visibility", "hidden");
		tip.hidden = true;
	};
	root.addEventListener("pointermove", show);
	root.addEventListener("pointerdown", show);
	root.addEventListener("pointerleave", hide);
	wrap.appendChild(root);
	wrap.appendChild(tip);
	return wrap;
}

// ---- The chart element of a control page (spec/runtime-image.md 3.9) ----------------------------
// Several series on a left and a right axis: lines (the bucket's mean or last value), a min/max band
// for a single banded series, consumption bars, boolean series as on/off strips beneath the plot
// (a bucket's on-share as the strip's opacity), and horizontal reference lines. Colours come from
// the fixed palette --s1..--s12 in series order (style.css); text stays in the text colours.

export const PALETTE_SIZE = 12;

/// The axis bounds of the values on one axis: fixed ends where the image fixes them, else the
/// data's range padded by 8 % (a bar axis includes 0).
function axisRange(values, fixed, zero) {
	let lo = Infinity;
	let hi = -Infinity;
	for (const v of values) {
		if (Number.isFinite(v)) {
			lo = Math.min(lo, v);
			hi = Math.max(hi, v);
		}
	}
	if (zero) {
		lo = Math.min(lo, 0);
		hi = Math.max(hi, 0);
	}
	if (lo === Infinity) {
		lo = 0;
		hi = 1;
	}
	if (lo === hi) {
		lo -= 1;
		hi += 1;
	}
	const pad = (hi - lo) * 0.08;
	const min = fixed && fixed.min !== null && fixed.min !== undefined ? fixed.min : lo - (zero && lo === 0 ? 0 : pad);
	const max = fixed && fixed.max !== null && fixed.max !== undefined ? fixed.max : hi + pad;
	return min < max ? [min, max] : [min, min + 1];
}

/// Draws the chart into `wrap` (emptied first) at `width` x `height` px. `series`: [{label, color
/// (1..12), axis ("left"|"right"), kind ("line"|"band"|"bars"|"strip"), unit, digits, hidden,
/// points: line [t, v], band [t, v, min, max], strip [t, share, t_end], bars [t, t_end, v]}];
/// `references`: [{label, axis, value, unit}]; `axes`: the image's {left: {min, max}, right}.
export function seriesChart(wrap, { width, height, from, to, series, references = [], axes = {} }) {
	wrap.textContent = "";
	const visible = series.filter((s) => !s.hidden);
	const strips = visible.filter((s) => s.kind === "strip");
	const plotted = visible.filter((s) => s.kind !== "strip");
	const used = (axis) => plotted.some((s) => s.axis === axis) || references.some((r) => r.axis === axis && Number.isFinite(r.value));
	const leftUsed = used("left") || !used("right");
	const rightUsed = used("right");
	const left = leftUsed ? 44 : 8;
	const right = rightUsed ? 44 : 8;
	// The units stand above the topmost value labels.
	const top = plotted.some((s) => s.unit) ? 18 : 6;
	const stripHeight = 10;
	const timeHeight = 16;
	const bottom = timeHeight + strips.length * (stripHeight + 2);
	const plotBottom = height - bottom;
	const x = (time) => left + ((time - from) / (to - from)) * (width - left - right);
	const range = {};
	for (const axis of ["left", "right"]) {
		const values = [];
		let bars = false;
		for (const s of plotted.filter((s) => s.axis === axis)) {
			bars = bars || s.kind === "bars";
			for (const p of s.points) {
				if (s.kind === "bars") {
					values.push(p[2]);
				} else {
					values.push(p[1]);
					if (s.kind === "band") {
						values.push(p[2], p[3]);
					}
				}
			}
		}
		for (const r of references) {
			if (r.axis === axis) {
				values.push(r.value);
			}
		}
		range[axis] = axisRange(values, axes[axis], bars);
	}
	const y = (axis, v) => {
		const [lo, hi] = range[axis];
		return top + (1 - (v - lo) / (hi - lo)) * (plotBottom - top);
	};
	const unitOf = (axis) => (plotted.find((s) => s.axis === axis) || {}).unit || "";
	const digitsOf = (axis) => (plotted.find((s) => s.axis === axis) || { digits: 1 }).digits;
	const root = svg("svg", { viewBox: `0 0 ${width} ${height}`, width, height, role: "img", class: "series-chart",
		"aria-label": visible.map((s) => s.label).join(", ") });

	// Grid and the axes' labels: three value lines per used axis, four time marks.
	for (let i = 0; i <= 2; i++) {
		const gy = top + ((plotBottom - top) * i) / 2;
		root.appendChild(svg("line", { x1: left, x2: width - right, y1: gy, y2: gy, class: "chart-grid" }));
		for (const side of ["left", "right"]) {
			if ((side === "left" && !leftUsed) || (side === "right" && !rightUsed)) {
				continue;
			}
			const [a, b] = range[side];
			const v = b - ((b - a) * i) / 2;
			const text = svg("text", { x: side === "left" ? left - 4 : width - right + 4, y: gy + 4, class: "chart-label",
				"text-anchor": side === "left" ? "end" : "start" });
			text.textContent = formatNumber(v, "", digitsOf(side));
			root.appendChild(text);
		}
	}
	for (const side of ["left", "right"]) {
		const unit = unitOf(side);
		if (unit && ((side === "left" && leftUsed) || (side === "right" && rightUsed))) {
			const text = svg("text", { x: side === "left" ? 2 : width - 2, y: 10, class: "chart-label chart-unit",
				"text-anchor": side === "left" ? "start" : "end" });
			text.textContent = unit;
			root.appendChild(text);
		}
	}
	for (let i = 0; i <= 3; i++) {
		const time = from + ((to - from) * i) / 3;
		const text = svg("text", { x: x(time), y: height - 4, class: "chart-label", "text-anchor": i === 0 ? "start" : i === 3 ? "end" : "middle" });
		text.textContent = formatTick(time, to - from);
		root.appendChild(text);
	}

	// Bars first, then bands, lines, reference lines; the strips beneath.
	const single = plotted.length === 1;
	for (const s of plotted) {
		const cls = `series-${s.color}`;
		if (s.kind === "bars") {
			const base = y(s.axis, Math.max(range[s.axis][0], Math.min(range[s.axis][1], 0)));
			for (const [t0, t1, v] of s.points) {
				const x0 = x(t0);
				const w = Math.max(1, x(t1) - x0 - 2); // a 2 px gap between bars
				const top0 = y(s.axis, v);
				root.appendChild(svg("rect", { x: (x0 + 1).toFixed(1), y: Math.min(top0, base).toFixed(1), width: w.toFixed(1),
					height: Math.max(1, Math.abs(base - top0)).toFixed(1), class: `chart-bar ${cls}`, rx: 2 }));
			}
			continue;
		}
		if (s.kind === "band" && single && s.points.length > 1) {
			const upper = s.points.map((p) => `${x(p[0]).toFixed(1)} ${y(s.axis, p[3]).toFixed(1)}`);
			const lower = s.points.slice().reverse().map((p) => `${x(p[0]).toFixed(1)} ${y(s.axis, p[2]).toFixed(1)}`);
			root.appendChild(svg("path", { d: `M${upper.join("L")}L${lower.join("L")}Z`, class: `chart-band ${cls}` }));
		}
		// A gap of more than 2.5 buckets is no data: the line breaks there.
		let d = "";
		let previous = null;
		const gap = s.gap || Infinity;
		for (const p of s.points) {
			const px = x(p[0]).toFixed(1);
			const py = y(s.axis, p[1]).toFixed(1);
			d += previous === null || p[0] - previous > gap ? `M${px} ${py}` : `L${px} ${py}`;
			previous = p[0];
		}
		if (s.points.length === 1) {
			const p = s.points[0];
			root.appendChild(svg("circle", { cx: x(p[0]), cy: y(s.axis, p[1]), r: 3, class: `chart-point ${cls}` }));
		}
		root.appendChild(svg("path", { d, class: `chart-line ${cls}` }));
	}
	references.forEach((r, i) => {
		if (!Number.isFinite(r.value)) {
			return;
		}
		const ry = y(r.axis, r.value);
		if (ry < top || ry > plotBottom) {
			return;
		}
		root.appendChild(svg("line", { x1: left, x2: width - right, y1: ry, y2: ry, class: "chart-reference" }));
		// Labels alternate between the right and the left end, so lines at one value stay readable.
		const end = i % 2 === 0;
		const text = svg("text", { x: end ? width - right - 4 : left + 4, y: ry - 3, class: "chart-label", "text-anchor": end ? "end" : "start" });
		text.textContent = `${r.label ? `${r.label} ` : ""}${formatNumber(r.value, r.unit || "", 1)}`;
		root.appendChild(text);
	});
	strips.forEach((s, i) => {
		const sy = plotBottom + 2 + i * (stripHeight + 2);
		root.appendChild(svg("rect", { x: left, y: sy, width: width - left - right, height: stripHeight, class: "chart-strip-bg", rx: 2 }));
		for (const [t0, share, t1] of s.points) {
			if (share <= 0) {
				continue;
			}
			const x0 = Math.max(left, x(t0));
			const x1 = Math.min(width - right, x(t1));
			if (x1 > x0) {
				root.appendChild(svg("rect", { x: x0.toFixed(1), y: sy, width: (x1 - x0).toFixed(1), height: stripHeight,
					class: `chart-strip ${`series-${s.color}`}`, "fill-opacity": Math.max(0.15, Math.min(1, share)).toFixed(2) }));
			}
		}
	});

	// Read-out on hover and touch: the time and every visible series' value there.
	const cross = svg("line", { y1: top, y2: plotBottom, class: "chart-cross", visibility: "hidden" });
	root.appendChild(cross);
	const tip = h("div", { class: "chart-tip", hidden: true });
	const valueAt = (s, time) => {
		let best = null;
		for (const p of s.points) {
			if (p[0] <= time && (best === null || p[0] >= best[0])) {
				best = p;
			}
		}
		if (!best) {
			return null;
		}
		if (s.kind === "strip") {
			return best[2] >= time ? best[1] : null;
		}
		if (s.kind === "bars") {
			return best[1] >= time ? best[2] : null;
		}
		return best[1];
	};
	const show = (event) => {
		const box = root.getBoundingClientRect();
		const sx = ((event.clientX - box.left) / box.width) * width;
		if (sx < left || sx > width - right) {
			return;
		}
		const time = from + ((sx - left) / (width - left - right)) * (to - from);
		cross.setAttribute("x1", sx);
		cross.setAttribute("x2", sx);
		cross.setAttribute("visibility", "visible");
		const lines = visible.map((s) => {
			const v = valueAt(s, time);
			const text = v === null ? "–" : s.kind === "strip"
				? (s.share ? `${Math.round(v * 100)} %` : v >= 0.5 ? t("value.on") : t("value.off"))
				: formatNumber(v, s.unit, s.digits);
			return h("div", null, h("span", { class: `legend-swatch series-${s.color}` }), ` ${s.label}: ${text}`);
		});
		replaceChildren(tip, h("div", { class: "muted" }, formatWhen(time, to)), lines);
		tip.hidden = false;
		const fraction = sx / width;
		tip.style.left = fraction > 0.5 ? "auto" : `${fraction * 100}%`;
		tip.style.right = fraction > 0.5 ? `${(1 - fraction) * 100}%` : "auto";
	};
	const hide = () => {
		cross.setAttribute("visibility", "hidden");
		tip.hidden = true;
	};
	root.addEventListener("pointermove", show);
	root.addEventListener("pointerdown", show);
	root.addEventListener("pointerleave", hide);
	wrap.appendChild(root);
	wrap.appendChild(tip);
}

function replaceChildren(element, ...children) {
	element.textContent = "";
	for (const child of children.flat()) {
		element.appendChild(child instanceof Node ? child : document.createTextNode(String(child)));
	}
}
