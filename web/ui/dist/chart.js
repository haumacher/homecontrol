/* Copyright (c) 2026 Bernhard Haumacher */
// History drawn as inline SVG (GET /api/history: points [t, last] or [t, last, min, max, count]):
// a sparkline for the tiles and a chart with a min/max band, axes and a hover read-out for the
// detail sheet. One series each, in the accent colour; the text stays in the text colours.

import { h } from "/dom.js";
import { formatNumber, formatWhen } from "/format.js";
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
