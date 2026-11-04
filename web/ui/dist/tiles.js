/* Copyright (c) 2026 Bernhard Haumacher */
// One tile per function of the image, by its kind (spec/runtime-image.md 3.5): the icon is the
// quick action (toggle, activate, press), the name opens the detail sheet, the controls below
// operate the rest (level, blind, setpoint). A command shows a pending state until the datapoint
// that reports the result has reported (or 5 s passed); a refusal shows its reason on the tile.

import { errorText } from "/api.js";
import { h, icon, icons, replace } from "/dom.js";
import { formatAgo, formatNumber, formatValue } from "/format.js";
import { t, has as hasText } from "/i18n.js";
import { label, dpUnit } from "/model.js";
import { sparkline } from "/chart.js";

const REPORT_WAIT_MS = 5000;

/// The role whose datapoint carries the state shown (and its quality) per kind; the first present.
const PRIMARY = {
	light: ["state", "command"],
	switch: ["state", "command"],
	dimmer: ["level", "levelCommand", "state"],
	blind: ["position", "move"],
	sensor: ["value"],
	climate: ["temperature"],
	scene: ["command"],
	button: ["state", "press"],
};

const ICON = {
	light: icons.bulb,
	switch: icons.power,
	dimmer: icons.bulb,
	blind: icons.blind,
	sensor: icons.gauge,
	climate: icons.thermometer,
	scene: icons.play,
	button: icons.touch,
};

/// Builds the tile of `fn`; `env` = {model, live, open(fn), history(id)}. Returns {element, ids,
/// update()}: `ids` are the datapoints whose updates call update().
export function functionTile(fn, env) {
	const { live, model } = env;
	const roles = fn.datapoints || {};
	const name = label(fn.label, fn.id);
	const dp = (role) => (roles[role] !== undefined ? live.datapoints.get(roles[role]) : undefined);
	const has = (role) => roles[role] !== undefined;
	const primaryRole = (PRIMARY[fn.kind] || Object.keys(roles)).find(has);
	const unitOf = (role) => (role === "value" || role === "temperature") && fn.unit ? fn.unit : dpUnit(model, roles[role]);
	const digitsOf = (role) => (unitOf(role) === "%" ? 0 : 1);
	const number = (role) => {
		const d = dp(role);
		return d && d.quality !== "unknown" && typeof d.value === "number" ? d.value : null;
	};
	const bool = (role) => {
		const d = dp(role);
		return d && d.quality !== "unknown" ? d.value === true : null;
	};
	// From the model: the tiles are built before the live snapshot may have arrived.
	const info = (role) => (roles[role] !== undefined ? model.datapoints.get(roles[role]) : undefined);
	const writable = (role) => {
		const d = info(role) || dp(role);
		return !!(d && d.writable);
	};
	const typeOf = (role) => {
		const d = info(role) || dp(role);
		return d ? d.type : undefined;
	};

	const element = h("div", { class: `tile kind-${fn.kind}`, "data-function": fn.id });
	const error = h("p", { class: "tile-error", role: "status", hidden: true });
	const stateText = h("span", { class: "tile-state" });
	const main = h("button", { type: "button", class: "tile-main", "aria-haspopup": "dialog", onclick: () => env.open(fn) },
		h("span", { class: "tile-name" }, name), stateText);
	let errorTimer = null;
	let pending = 0;

	function showError(text) {
		error.textContent = text;
		error.hidden = !text;
		clearTimeout(errorTimer);
		if (text) {
			errorTimer = setTimeout(() => showError(""), 8000);
		}
	}

	function setPending(on) {
		pending += on ? 1 : -1;
		element.classList.toggle("pending", pending > 0);
		element.setAttribute("aria-busy", pending > 0 ? "true" : "false");
	}

	/// Commands the datapoint of `role` and waits for `reportRole`'s datapoint to report.
	async function send(role, value, reportRole) {
		const id = roles[role];
		const watch = reportRole && has(reportRole) ? roles[reportRole] : null;
		setPending(true);
		showError("");
		let listener = null;
		let timer = null;
		const reported = watch === null || watch === id ? null : new Promise((resolve) => {
			listener = (event) => {
				if (event.detail.all || (event.detail.ids && event.detail.ids.includes(watch))) {
					resolve();
				}
			};
			live.addEventListener("datapoints", listener);
			timer = setTimeout(resolve, REPORT_WAIT_MS);
		});
		try {
			await live.command(id, value);
			if (reported) {
				await reported;
			}
		} catch (e) {
			const technology = (model.datapoints.get(id) || {}).technology;
			showError(e && e.code === "dp.refused" && technology
				? t("dp.refused.by", { technology: hasText(`technology.${technology}`) ? t(`technology.${technology}`) : technology })
				: errorText(e));
		} finally {
			if (listener) {
				live.removeEventListener("datapoints", listener);
			}
			clearTimeout(timer);
			setPending(false);
			update();
		}
	}

	// ---- The quick action on the icon ----
	let action = null; // () => void
	let pressedState = null; // () => bool|null for aria-pressed
	let actionLabel = "";
	switch (fn.kind) {
		case "light":
		case "switch":
			if (writable("command")) {
				pressedState = () => bool(has("state") ? "state" : "command");
				action = () => send("command", !(pressedState() === true), "state");
				actionLabel = t("tile.toggle", { name });
			}
			break;
		case "dimmer":
			if (writable("command")) {
				pressedState = () => bool(has("state") ? "state" : "command") ?? ((number("level") ?? number("levelCommand") ?? 0) > 0);
				action = () => send("command", !(pressedState() === true), "state");
				actionLabel = t("tile.toggle", { name });
			} else if (writable("levelCommand")) {
				pressedState = () => (number(has("level") ? "level" : "levelCommand") ?? 0) > 0;
				action = () => send("levelCommand", pressedState() ? 0 : 100, "level");
				actionLabel = t("tile.toggle", { name });
			}
			break;
		case "scene":
			if (writable("command")) {
				action = () => {
					// The image counts scenes from 1 (spec/runtime-image.md 3.5); the datapoint carries the
					// KNX scene code, which counts from 0 (DPT 17.001, 18.001).
					const code = (fn.scene || 1) - 1;
					send("command", typeOf("command") === "controlled" ? { control: false, value: code } : code);
				};
				actionLabel = t("tile.activate", { name });
			}
			break;
		case "button":
			if (writable("press")) {
				action = () => {
					send("press", typeOf("press") === "controlled" ? { control: true, value: 1 } : true, "state");
				};
				actionLabel = t("tile.press", { name });
			}
			break;
	}
	const iconElement = action
		? h("button", { type: "button", class: "tile-icon", "aria-label": actionLabel, onclick: () => action() }, icon(ICON[fn.kind] || icons.data))
		: h("span", { class: "tile-icon", "aria-hidden": "true" }, icon(ICON[fn.kind] || icons.data));
	element.appendChild(h("div", { class: "tile-head" }, iconElement, main));

	// ---- Controls below the head ----
	const controls = [];
	let slider = null;
	let sliderRole = null;
	let sliderEditing = false;
	let setpointText = null;
	let spark = null;

	function rangeControl(role, reportRole, labelKey) {
		const input = h("input", { type: "range", min: 0, max: 100, step: 1, class: "slider", "aria-label": t(labelKey, { name }) });
		input.addEventListener("input", () => {
			sliderEditing = true;
			stateText.textContent = formatNumber(Number(input.value), unitOf(role) || "%", 0);
		});
		input.addEventListener("change", () => {
			sliderEditing = false;
			send(role, Number(input.value), reportRole);
		});
		input.addEventListener("blur", () => {
			sliderEditing = false;
		});
		slider = input;
		sliderRole = reportRole && has(reportRole) ? reportRole : role;
		return input;
	}

	if (fn.kind === "dimmer" && writable("levelCommand")) {
		controls.push(rangeControl("levelCommand", "level", "tile.level"));
	}
	if (fn.kind === "dimmer" && writable("step")) {
		// Relative dimming (KNX DPT 3.007): the control bit is the direction, the value the step code;
		// code 3 is a 25 % step, so four taps run through the range.
		const dim = (brighter) => send("step", { control: brighter, value: 3 }, "level");
		controls.push(h("div", { class: "button-row" },
			h("button", { type: "button", class: "round", "aria-label": t("tile.darker", { name }), onclick: () => dim(false) }, icon(icons.minus)),
			h("button", { type: "button", class: "round", "aria-label": t("tile.brighter", { name }), onclick: () => dim(true) }, icon(icons.plus))));
	}
	if (fn.kind === "blind") {
		const buttons = [];
		if (writable("move")) {
			buttons.push(h("button", { type: "button", class: "round", "aria-label": t("tile.up", { name }), onclick: () => send("move", false, "position") }, icon(icons.up)));
		}
		if (writable("stop")) {
			buttons.push(h("button", { type: "button", class: "round", "aria-label": t("tile.stop", { name }), onclick: () => send("stop", true) }, icon(icons.stop)));
		}
		if (writable("move")) {
			buttons.push(h("button", { type: "button", class: "round", "aria-label": t("tile.down", { name }), onclick: () => send("move", true, "position") }, icon(icons.down)));
		}
		controls.push(h("div", { class: "button-row" }, buttons));
		if (writable("positionCommand")) {
			controls.push(rangeControl("positionCommand", "position", "tile.position"));
		}
	}
	if (fn.kind === "climate") {
		const target = has("setpoint") ? "setpoint" : "setpointCommand";
		setpointText = h("span", { class: "setpoint" });
		const step = (delta) => {
			const current = number(target) ?? number("setpointCommand") ?? number("temperature");
			if (current === null) {
				showError(t("tile.no_setpoint"));
				return;
			}
			send("setpointCommand", Math.round((current + delta) * 2) / 2, "setpoint");
		};
		const adjustable = writable("setpointCommand");
		controls.push(h("div", { class: "button-row setpoint-row" },
			adjustable ? h("button", { type: "button", class: "round", "aria-label": t("tile.colder", { name }), onclick: () => step(-0.5) }, icon(icons.minus)) : null,
			setpointText,
			adjustable ? h("button", { type: "button", class: "round", "aria-label": t("tile.warmer", { name }), onclick: () => step(0.5) }, icon(icons.plus)) : null));
	}
	if (fn.kind === "sensor") {
		const valueInfo = info("value");
		if (valueInfo && valueInfo.history) {
			spark = h("div", { class: "tile-spark" });
			controls.push(spark);
			env.history(roles.value).then((data) => {
				if (data) {
					replace(spark, sparkline(data.points, data.from, data.to, typeOf("value") === "boolean"));
				}
			}).catch(() => {});
		}
	}
	if (controls.length) {
		element.appendChild(h("div", { class: "tile-controls" }, controls));
	}
	element.appendChild(error);

	// ---- State ----
	function text() {
		const show = (role) => {
			const d = dp(role);
			if (!d || d.quality === "unknown" || d.value === null) {
				return t("value.unknown");
			}
			return typeof d.value === "number" ? formatNumber(d.value, unitOf(role), digitsOf(role)) : formatValue(d);
		};
		switch (fn.kind) {
			case "light":
			case "switch": {
				const on = bool(has("state") ? "state" : "command");
				return on === null ? t("value.unknown") : on ? t("value.on") : t("value.off");
			}
			case "dimmer": {
				const level = number(has("level") ? "level" : "levelCommand");
				const on = has("state") ? bool("state") : level === null ? null : level > 0;
				if (on === null && level === null) {
					return t("value.unknown");
				}
				if (on === false || level === 0) {
					return t("value.off");
				}
				return level !== null ? formatNumber(level, unitOf("levelCommand") || "%", 0) : t("value.on");
			}
			case "blind":
				return has("position") ? show("position") : t("tile.blind");
			case "climate": {
				const heating = bool("heating") ? ` · ${t("tile.heating")}` : "";
				return `${show("temperature")}${heating}`;
			}
			case "scene":
				return t("tile.scene", { scene: fn.scene || "" });
			case "button": {
				if (has("state")) {
					return show("state");
				}
				const d = dp("press");
				return d && d.quality !== "unknown" ? t("tile.pressed", { ago: formatAgo(live.ageMs(d)) }) : t("tile.not_pressed");
			}
			default:
				return primaryRole ? show(primaryRole) : t("value.unknown");
		}
	}

	function update() {
		const primary = primaryRole ? dp(primaryRole) : undefined;
		const quality = primary ? primary.quality : "unknown";
		let state = text();
		if (quality === "stale" && primary) {
			state += ` · ${t("tile.last_known", { ago: formatAgo(live.ageMs(primary)) })}`;
		}
		if (Object.keys(roles).some((role) => dp(role) && dp(role).pending)) {
			state += ` · ${t("value.waiting")}`;
		}
		stateText.textContent = sliderEditing ? stateText.textContent : state;
		element.classList.toggle("stale", quality === "stale");
		element.classList.toggle("unknown", quality === "unknown" && fn.kind !== "scene" && fn.kind !== "button");
		const on = pressedState ? pressedState() === true : fn.kind === "light" || fn.kind === "switch" ? bool("state") === true : false;
		element.classList.toggle("on", on);
		if (pressedState) {
			iconElement.setAttribute("aria-pressed", on ? "true" : "false");
		}
		if (slider && !sliderEditing) {
			const v = number(sliderRole);
			slider.value = v === null ? 0 : Math.max(0, Math.min(100, v));
		}
		if (setpointText) {
			const v = number(has("setpoint") ? "setpoint" : "setpointCommand") ?? number("setpointCommand");
			setpointText.textContent = t("tile.setpoint", { value: formatNumber(v, unitOf("setpointCommand") || unitOf("temperature"), 1) });
		}
	}

	update();
	return { element, ids: new Set(Object.values(roles)), update, refreshSpark: (data) => {
		if (spark && data) {
			replace(spark, sparkline(data.points, data.from, data.to, typeOf("value") === "boolean"));
		}
	}, sparkId: spark ? roles.value : null };
}
