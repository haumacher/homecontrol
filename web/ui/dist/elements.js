/* Copyright (c) 2026 Bernhard Haumacher */
// The datapoint and chart elements of a control page (spec/runtime-image.md 3.9, GET /api/pages).
// A datapoint tile shows the value; its control is chosen by the value type, and only when the
// datapoint is writable and the element is not readOnly. A refused write shows its reason on the
// tile. A chart tile draws its series from the history (GET /api/history) over a range the viewer
// switches between the image's presets, stepping back and forth in time.

import { api, errorText } from "/api.js";
import { h, icon, icons, replace } from "/dom.js";
import { formatAgo, formatDurationShort, formatNumber, formatValue } from "/format.js";
import { t, has as hasText } from "/i18n.js";
import { label, dpLabel, dpUnit } from "/model.js";
import { seriesChart, PALETTE_SIZE } from "/chart.js";

const RANGE_SECONDS = { "1h": 3600, "24h": 86400, "7d": 7 * 86400, "30d": 30 * 86400, "1y": 365 * 86400 };
/// Consumption bars per range: their count and width (s), aligned to the local hour or day.
const BARS = { "1h": [12, 300], "24h": [24, 3600], "7d": [7, 86400], "30d": [30, 86400], "1y": [12, 365 * 86400 / 12] };
const LINE_POINTS = 48;
const MAX_POINTS = 160; // GET /api/history: points times ids
const TILE_ROW = 64;
const GAP = 12;
const REFRESH_MS = 5 * 60 * 1000;

function refusalText(error, model, id) {
	const technology = (model.datapoints.get(id) || {}).technology;
	return error && error.code === "dp.refused" && technology
		? t("dp.refused.by", { technology: hasText(`technology.${technology}`) ? t(`technology.${technology}`) : technology })
		: errorText(error);
}

function two(n) {
	return String(n).padStart(2, "0");
}

/// The value of an input in the datapoint's JSON form (spec/api.md "Values"); undefined if empty.
function inputValue(type, text) {
	if (text === "") {
		return undefined;
	}
	switch (type) {
		case "time of day": {
			const [hour, minute, second = 0] = text.split(":").map(Number);
			return { hour, minute, second };
		}
		case "date": {
			const [year, month, day] = text.split("-").map(Number);
			return { year, month, day };
		}
		case "date-time": {
			const [date, time] = text.split("T");
			const [year, month, day] = date.split("-").map(Number);
			const [hour, minute, second = 0] = time.split(":").map(Number);
			return { year, month, day, hour, minute, second };
		}
		case "color": {
			const n = parseInt(text.slice(1), 16);
			return { red: (n >> 16) & 255, green: (n >> 8) & 255, blue: n & 255 };
		}
		case "string":
			return text;
		default: {
			const n = Number(text);
			return Number.isFinite(n) ? n : undefined;
		}
	}
}

/// The datapoint's value as the input's text.
function inputText(type, v) {
	if (v === null || v === undefined) {
		return "";
	}
	switch (type) {
		case "time of day":
			return `${two(v.hour)}:${two(v.minute)}:${two(v.second)}`;
		case "date":
			return `${v.year}-${two(v.month)}-${two(v.day)}`;
		case "date-time":
			return `${v.year}-${two(v.month)}-${two(v.day)}T${two(v.hour)}:${two(v.minute)}:${two(v.second)}`;
		case "color":
			return `#${[v.red, v.green, v.blue].map((c) => c.toString(16).padStart(2, "0")).join("")}`;
		default:
			return String(v);
	}
}

/// A datapoint element: {datapoint, label, readOnly}. Returns {element, ids, update()}.
export function valueTile(el, env) {
	const { live, model } = env;
	const id = el.datapoint;
	const info = model.datapoints.get(id) || { name: String(id), type: "raw", writable: false };
	const name = el.label !== undefined ? label(el.label, info.name) : dpLabel(model, id, live);
	const unit = dpUnit(model, id);
	const type = info.type;
	const editable = !!info.writable && !el.readOnly;
	const numeric = type === "unsigned" || type === "signed" || type === "real" || type === "enumeration";
	const element = h("div", { class: "tile kind-value", "data-datapoint": info.name });
	const stateText = h("span", { class: "tile-state" });
	const error = h("p", { class: "tile-error", role: "status", hidden: true });
	let errorTimer = null;
	let pending = false;
	const current = () => live.datapoints.get(id);

	function showError(text) {
		error.textContent = text;
		error.hidden = !text;
		clearTimeout(errorTimer);
		if (text) {
			errorTimer = setTimeout(() => showError(""), 8000);
		}
	}

	async function send(value) {
		if (value === undefined || pending) {
			return;
		}
		pending = true;
		element.classList.add("pending");
		showError("");
		try {
			await live.command(id, value);
		} catch (e) {
			showError(`${t("element.refused")}: ${refusalText(e, model, id)}`);
		} finally {
			pending = false;
			element.classList.remove("pending");
			update();
		}
	}

	// ---- The control by the value type ----
	let toggle = null;
	let input = null;
	let slider = null;
	const controls = [];
	if (editable && type === "boolean") {
		toggle = h("button", { type: "button", class: "tile-icon", "aria-label": t("tile.toggle", { name }),
			onclick: () => {
				const dp = current();
				send(!(dp && dp.value === true));
			} }, icon(icons.toggle));
	} else if (editable && type === "enumeration" && Array.isArray(info.choices) && info.choices.length) {
		input = h("select", { class: "element-input", "aria-label": name, onchange: () => send(Number(input.value)) },
			info.choices.map((c) => h("option", { value: String(c.value) }, label(c.label, String(c.value)))));
		controls.push(input);
	} else if (editable && (numeric || type === "string" || type === "time of day" || type === "date" || type === "date-time" || type === "color")) {
		const kind = numeric ? "number" : type === "string" ? "text" : type === "time of day" ? "time" : type === "date" ? "date"
			: type === "date-time" ? "datetime-local" : "color";
		input = h("input", { type: kind, class: "element-input", "aria-label": name,
			step: numeric ? (info.step || (type === "real" ? "any" : 1)) : kind === "time" || kind === "datetime-local" ? 1 : undefined,
			min: numeric && info.min !== undefined ? info.min : undefined,
			max: numeric && info.max !== undefined ? info.max : undefined,
			onchange: () => send(inputValue(type, input.value)) });
		const row = [input];
		if (unit && numeric) {
			row.push(h("span", { class: "element-unit" }, unit));
		}
		controls.push(h("div", { class: "element-row" }, row));
		if (numeric && typeof info.min === "number" && typeof info.max === "number") {
			slider = h("input", { type: "range", class: "slider", min: info.min, max: info.max, step: info.step || (type === "real" ? "any" : 1),
				"aria-label": t("element.slider", { name }),
				oninput: () => {
					stateText.textContent = formatNumber(Number(slider.value), unit, 2);
				},
				onchange: () => send(Number(slider.value)) });
			controls.push(slider);
		}
	}
	const iconElement = toggle || h("span", { class: "tile-icon", "aria-hidden": "true" },
		icon(type === "boolean" ? icons.toggle : numeric ? icons.gauge : type === "string" ? icons.text
			: type === "color" ? icons.palette : type.includes("date") || type.includes("time") ? icons.clock : icons.data));
	element.append(h("div", { class: "tile-head" }, iconElement,
		h("div", { class: "tile-main static" }, h("span", { class: "tile-name", title: info.name }, name), stateText)));
	if (controls.length) {
		element.appendChild(h("div", { class: "tile-controls" }, controls));
	}
	element.appendChild(error);

	function update() {
		const dp = current();
		const quality = dp ? dp.quality : "unknown";
		let text;
		if (!dp || quality === "unknown" || dp.value === null) {
			text = t("value.unknown");
		} else if (typeof dp.value === "number" && type !== "enumeration") {
			text = formatNumber(dp.value, unit, unit === "%" ? 0 : 2);
		} else if (type === "enumeration" && Array.isArray(info.choices)) {
			const choice = info.choices.find((c) => c.value === dp.value);
			text = choice ? label(choice.label, String(dp.value)) : formatValue(dp);
		} else {
			text = formatValue(dp);
		}
		if (quality === "stale" && dp) {
			text += ` · ${t("tile.last_known", { ago: formatAgo(live.ageMs(dp)) })}`;
		}
		if (!editable && info.writable) {
			text += ` · ${t("element.read_only")}`;
		}
		stateText.textContent = text;
		element.classList.toggle("stale", quality === "stale");
		element.classList.toggle("unknown", quality === "unknown");
		const on = type === "boolean" && dp && quality !== "unknown" && dp.value === true;
		element.classList.toggle("on", !!on);
		if (toggle) {
			toggle.setAttribute("aria-pressed", on ? "true" : "false");
		}
		const editing = (x) => x && document.activeElement === x;
		if (input && !editing(input) && dp && quality !== "unknown") {
			input.value = inputText(type, dp.value);
		}
		if (slider && !editing(slider) && dp && typeof dp.value === "number") {
			slider.value = dp.value;
		}
	}

	update();
	return { element, ids: new Set([id]), update };
}

/// Local midnight (or the hour) after `seconds`, for bars that start at a natural boundary.
function boundaryAfter(seconds, width) {
	const date = new Date(seconds * 1000);
	if (width < 86400) {
		const step = width * 1000;
		return Math.ceil(date.getTime() / step) * step / 1000;
	}
	date.setHours(24, 0, 0, 0);
	return Math.floor(date.getTime() / 1000);
}

/// A chart element: {chart: {label, width, height, range, presets, axes, series, references}}.
/// Returns {element, ids, update(), tick(refresh), layout(columns)}.
export function chartTile(el, env) {
	const { live, model } = env;
	const spec = el.chart;
	const presets = spec.presets && spec.presets.length ? spec.presets : [spec.range];
	let range = spec.range;
	let offset = 0; // ranges back from now
	const rows = spec.height === 4 ? 4 : 2;
	const element = h("div", { class: "tile chart-tile", style: `height:${rows * TILE_ROW + (rows - 1) * GAP}px` });
	// The element's label replaces the chart's own, as on every element.
	const title = el.label !== undefined ? label(el.label, "") : spec.label !== undefined ? label(spec.label, "") : "";
	const plot = h("div", { class: "chart chart-plot" });
	const hint = h("p", { class: "chart-hint", hidden: true });
	const error = h("p", { class: "tile-error", role: "status", hidden: true });
	const legend = h("div", { class: "chart-legend", role: "group", "aria-label": t("chart.legend") });
	const series = spec.series.map((s, i) => {
		const info = model.datapoints.get(s.datapoint) || { name: String(s.datapoint), type: "real" };
		const boolean = info.type === "boolean";
		return {
			datapoint: s.datapoint,
			label: s.label !== undefined ? label(s.label, info.name) : dpLabel(model, s.datapoint, live),
			color: (i % PALETTE_SIZE) + 1,
			axis: s.axis || "left",
			consolidation: s.consolidation,
			boolean,
			kind: boolean ? "strip" : s.consolidation === "consumption" ? "bars" : s.consolidation === "band" ? "band" : "line",
			share: boolean && s.consolidation !== "last",
			unit: dpUnit(model, s.datapoint),
			digits: dpUnit(model, s.datapoint) === "%" ? 0 : 1,
			hidden: false,
			points: [],
		};
	});
	const references = (spec.references || []).map((r) => ({
		label: r.label !== undefined ? label(r.label, "") : r.datapoint !== undefined ? dpLabel(model, r.datapoint, live) : "",
		axis: r.axis || "left",
		constant: r.value,
		datapoint: r.datapoint,
		unit: r.datapoint !== undefined ? dpUnit(model, r.datapoint) : (series.find((s) => s.axis === (r.axis || "left")) || {}).unit,
	}));
	let span = { from: 0, to: 1 };
	let loaded = false;
	let generation = 0;

	const rangeButtons = presets.map((p) => h("button", { type: "button", "aria-pressed": p === range ? "true" : "false",
		onclick: () => {
			range = p;
			offset = 0;
			rangeButtons.forEach((b, i) => b.setAttribute("aria-pressed", presets[i] === range ? "true" : "false"));
			load();
		} }, t(`chart.range.${p}`)));
	const back = h("button", { type: "button", class: "icon-button small", "aria-label": t("chart.back"), onclick: () => {
		offset += 1;
		load();
	} }, icon(icons.left));
	const forward = h("button", { type: "button", class: "icon-button small", "aria-label": t("chart.forward"), onclick: () => {
		offset = Math.max(0, offset - 1);
		load();
	} }, icon(icons.right));
	element.append(
		h("div", { class: "chart-head" },
			title ? h("span", { class: "tile-name" }, title) : h("span"),
			h("div", { class: "chart-tools" }, back,
				presets.length > 1 ? h("div", { class: "segmented small", role: "group", "aria-label": t("history.range") }, rangeButtons) : null,
				forward)),
		plot, ...(series.length > 1 ? [legend] : []), hint, error);

	if (series.length > 1) {
		for (const s of series) {
			const button = h("button", { type: "button", class: "legend-item", "aria-pressed": "true", onclick: () => {
				s.hidden = !s.hidden;
				button.setAttribute("aria-pressed", s.hidden ? "false" : "true");
				draw();
			} }, h("span", { class: `legend-swatch series-${s.color}${s.kind === "strip" ? " strip" : ""}` }), s.label);
			legend.appendChild(button);
		}
	}

	function draw() {
		const width = Math.max(200, Math.round(plot.clientWidth || element.clientWidth || 320));
		const height = Math.max(60, Math.round(plot.clientHeight || 100));
		const refs = references.map((r) => {
			let value = r.constant;
			if (r.datapoint !== undefined) {
				const dp = live.datapoints.get(r.datapoint);
				value = dp && dp.quality !== "unknown" && typeof dp.value === "number" ? dp.value : NaN;
			}
			return { label: r.label, axis: r.axis, value, unit: r.unit };
		});
		if (!loaded) {
			replace(plot, h("p", { class: "muted" }, t("app.loading")));
			return;
		}
		if (series.every((s) => s.points.length === 0)) {
			replace(plot, h("p", { class: "muted chart-empty" }, t("history.empty")));
			return;
		}
		seriesChart(plot, { width, height, from: span.from, to: span.to, series, references: refs, axes: spec.axes || {} });
	}

	/// Converts a bucket list of GET /api/history into the series' points.
	function convert(s, points, width, to) {
		if (s.kind === "bars") {
			// The delta between the last values of adjacent buckets (the first is the bucket before
			// the span); a negative delta is a counter reset: a gap (spec/api.md "history").
			const out = [];
			for (let i = 1; i < points.length; i++) {
				const [t0, last0] = points[i - 1];
				const [t1, last1] = points[i];
				const b0 = Math.floor((t0 - span.queryFrom) / width);
				const b1 = Math.floor((t1 - span.queryFrom) / width);
				if (b1 !== b0 + 1) {
					continue;
				}
				const delta = last1 - last0;
				if (delta >= 0) {
					const start = span.queryFrom + b1 * width;
					out.push([start, start + width, delta]);
				}
			}
			return out;
		}
		const value = (p) => (p.length > 5 && s.consolidation !== "last" ? p[5] : p[1]);
		if (s.kind === "strip") {
			return points.map((p, i) => {
				const end = i + 1 < points.length ? points[i + 1][0] : Math.min(to, p[0] + width);
				return [p[0], s.share ? value(p) : p[1], p.length > 2 ? Math.min(end, p[0] + width) : end];
			});
		}
		if (s.kind === "band") {
			return points.map((p) => (p.length > 2 ? [p[0], value(p), p[2], p[3]] : [p[0], p[1], p[1], p[1]]));
		}
		return points.map((p) => [p[0], value(p)]);
	}

	async function query(ids, from, to, points) {
		const data = await api("GET", `/api/history?ids=${ids.join(",")}&from=${Math.floor(from)}&to=${Math.ceil(to)}&points=${points}`);
		return data;
	}

	async function load() {
		const mine = ++generation;
		const seconds = RANGE_SECONDS[range];
		const now = Math.floor(Date.now() / 1000) + 1;
		const to = now - offset * seconds;
		span = { from: to - seconds, to };
		forward.disabled = offset === 0;
		error.hidden = true;
		try {
			const lines = series.filter((s) => s.kind !== "bars");
			const bars = series.filter((s) => s.kind === "bars");
			const reach = [];
			if (bars.length) {
				// Bars start at the local hour or midnight; the whole chart takes their window, and
				// the query one bucket before it for the first delta.
				const [count, barWidth] = BARS[range];
				const end = boundaryAfter(to, barWidth);
				span = { from: end - count * barWidth, to: end, queryFrom: end - (count + 1) * barWidth };
				const perBars = Math.max(1, Math.min(8, Math.floor(MAX_POINTS / (count + 1))));
				for (let i = 0; i < bars.length; i += perBars) {
					const group = bars.slice(i, i + perBars);
					const data = await query(group.map((s) => s.datapoint), span.queryFrom, end, count + 1);
					group.forEach((s, k) => {
						const answer = data.series[k] || {};
						s.points = answer.points ? convert(s, answer.points, barWidth, end) : [];
						if (answer.retention_s !== undefined) {
							reach.push(answer.retention_s);
						}
						if (answer.code) {
							throw { code: answer.code, reason: answer.error };
						}
					});
				}
			}
			const per = Math.max(1, Math.min(8, Math.floor(MAX_POINTS / LINE_POINTS)));
			for (let i = 0; i < lines.length; i += per) {
				const group = lines.slice(i, i + per);
				const data = await query(group.map((s) => s.datapoint), span.from, span.to, LINE_POINTS);
				const width = data.width_s || Math.ceil((span.to - span.from) / LINE_POINTS);
				group.forEach((s, k) => {
					const answer = data.series[k] || {};
					s.points = answer.points ? convert(s, answer.points, width, span.to) : [];
					s.gap = width * 2.5;
					if (answer.retention_s !== undefined) {
						reach.push(answer.retention_s);
					}
					if (answer.code) {
						throw { code: answer.code, reason: answer.error };
					}
				});
			}
			if (mine !== generation) {
				return;
			}
			loaded = true;
			// The answering store (the raw ring or an archive, issue #56) reaches back only so far: say so.
			const shortest = reach.length ? Math.min(...reach) : Infinity;
			const covered = now - span.from;
			hint.hidden = !(shortest < covered);
			if (!hint.hidden) {
				hint.textContent = t("chart.only_stored", { duration: formatDurationShort(shortest) });
			}
			draw();
		} catch (e) {
			if (mine !== generation) {
				return;
			}
			loaded = true;
			error.textContent = errorText(e);
			error.hidden = false;
			draw();
		}
	}

	const resize = typeof ResizeObserver === "function" ? new ResizeObserver(() => loaded && draw()) : null;
	if (resize) {
		resize.observe(plot);
	}
	const referenceIds = new Set(references.filter((r) => r.datapoint !== undefined).map((r) => r.datapoint));
	setTimeout(load, 0);
	return {
		element,
		ids: referenceIds,
		update: () => loaded && draw(),
		tick(refresh) {
			if (refresh && offset === 0) {
				load();
			}
		},
		/// Caps the width at the columns the grid has (a narrow screen has fewer).
		layout(columns) {
			const wanted = spec.width === "full" || spec.width === undefined ? columns : Math.min(spec.width, columns);
			element.style.gridColumn = `span ${Math.max(1, wanted)}`;
		},
		dispose() {
			if (resize) {
				resize.disconnect();
			}
		},
	};
}

export const CHART_REFRESH_MS = REFRESH_MS;
