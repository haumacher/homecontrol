/* Copyright (c) 2026 Bernhard Haumacher */
// Rules: each rule of the image with its state, when it was last triggered and the count of its
// problems, its enable switch and the parameters it declares adjustable, each edited within its
// declared range and reset to the image's default. Polled every 10 s; a card being edited is not
// redrawn. The page is the inhabitant's: the printer's texts of the last trigger, action and error
// (spec/rules.md 9.1, technical ids) are shown in the section "Rules" of the System page
// (diagnostics.js, homecontrol #82), which a problem count links to.

import { api, errorText } from "/api.js";
import { h, replace } from "/dom.js";
import { formatAgo, formatDurationShort, formatNumber } from "/format.js";
import { t } from "/i18n.js";
import { label, pages } from "/model.js";

const POLL_MS = 10000;
const DURATION_UNITS = { ms: 1, s: 1000, min: 60000, h: 3600000, d: 86400000 };

function valueText(p, value) {
	if (value === null || value === undefined) {
		return t("value.unknown");
	}
	switch (p.type) {
		case "boolean":
			return value ? t("value.on") : t("value.off");
		case "duration": {
			const scale = DURATION_UNITS[p.unit];
			return scale ? formatNumber(value / scale, p.unit, 2) : formatDurationShort(value / 1000);
		}
		case "integer":
		case "real":
			return formatNumber(value, p.unit, p.type === "integer" ? 0 : 2);
		case "enumeration": {
			const choice = (p.choices || []).find((c) => c.value === value);
			return choice ? label(choice.label, String(value)) : String(value);
		}
		case "timeOfDay":
			return `${String(value.hour).padStart(2, "0")}:${String(value.minute).padStart(2, "0")}`;
		default:
			return typeof value === "object" ? JSON.stringify(value) : String(value);
	}
}

function switchControl(checked, labelText, onchange, disabled) {
	return h("label", { class: "switch" },
		h("input", { type: "checkbox", role: "switch", checked: !!checked, disabled: !!disabled, onchange: (e) => onchange(e.target.checked) }),
		h("span", { class: "switch-track", "aria-hidden": "true" }),
		h("span", { class: "switch-label" }, labelText));
}

/// The editor of one parameter; `commit(body)` sends it and resolves with the new rule.
function parameterRow(rule, p, commit) {
	const id = `p-${rule.id}-${p.id}`;
	const feedback = h("span", { class: "hint", role: "status" });
	const shown = h("output", { class: "param-value", for: id }, valueText(p, p.value));
	const send = async (body) => {
		feedback.className = "hint";
		feedback.textContent = t("rules.saving");
		try {
			await commit(p.id, body);
			feedback.textContent = t("settings.saved"); // the card may be kept while its slider has the focus
		} catch (error) {
			feedback.className = "error-text";
			feedback.textContent = errorText(error);
			shown.textContent = valueText(p, p.value);
		}
	};
	let control = null;
	if (!p.adjustable) {
		control = h("span", { class: "muted" }, t("rules.fixed"));
	} else if (p.type === "boolean") {
		control = switchControl(p.value, valueText(p, p.value), (on) => send({ value: on }));
		control.querySelector("input").id = id;
	} else if (p.choices && p.choices.length) {
		control = h("select", { id, onchange: (e) => send({ value: Number(e.target.value) }) },
			p.choices.map((c) => h("option", { value: c.value, selected: c.value === p.value }, label(c.label, String(c.value)))));
	} else if (p.type === "timeOfDay") {
		const v = p.value || { hour: 0, minute: 0 };
		control = h("input", { type: "time", id, step: 60, value: `${String(v.hour).padStart(2, "0")}:${String(v.minute).padStart(2, "0")}`,
			onchange: (e) => {
				const [hour, minute] = e.target.value.split(":").map(Number);
				send({ value: { hour, minute, second: 0 } });
			} });
	} else if ((p.type === "integer" || p.type === "real" || p.type === "duration") && typeof p.min === "number" && typeof p.max === "number") {
		const step = p.step || (p.type === "real" ? (p.max - p.min) / 100 : p.type === "duration" ? (DURATION_UNITS[p.unit] || 1000) : 1);
		control = h("input", { type: "range", id, class: "slider", min: p.min, max: p.max, step, value: p.value });
		control.addEventListener("input", () => {
			shown.textContent = valueText(p, Number(control.value));
		});
		// Arrow keys fire a change per step: send once the value rests.
		let debounce = null;
		control.addEventListener("change", () => {
			clearTimeout(debounce);
			debounce = setTimeout(() => send({ value: Number(control.value) }), 400);
		});
	} else if (p.type === "integer" || p.type === "real" || p.type === "duration") {
		const scale = p.type === "duration" ? (DURATION_UNITS[p.unit] || 1000) : 1;
		control = h("input", { type: "number", id, value: p.value / scale, step: p.step ? p.step / scale : "any", class: "number",
			onchange: (e) => send({ value: Math.round(Number(e.target.value) * scale * 1000) / 1000 }) });
	} else {
		control = h("span", { class: "muted" }, t("rules.not_editable_here"));
	}
	const range = typeof p.min === "number" && typeof p.max === "number" ? t("rules.range", { min: valueText(p, p.min), max: valueText(p, p.max) }) : "";
	return h("div", { class: "param" },
		h("div", { class: "param-head" },
			h("label", { for: id }, label(p.label, p.id)),
			shown,
			p.overridden ? h("span", { class: "badge" }, t("rules.changed")) : null),
		h("div", { class: "param-control" }, control,
			p.overridden && p.adjustable ? h("button", { type: "button", class: "button secondary small", onclick: () => send({ reset: true }) },
				t("rules.reset", { value: valueText(p, p.default) })) : null),
		h("span", { class: "hint" }, [range, p.overridden ? t("rules.default", { value: valueText(p, p.default) }) : ""].filter(Boolean).join(" · ")),
		feedback);
}

function ruleCard(rule, nowMs, commit) {
	const ago = (ms) => formatAgo(nowMs - ms);
	const rows = [];
	if (rule.state) {
		rows.push([t("rules.state"), rule.state]);
	}
	rows.push([t("rules.last_triggered"), rule.lastTrigger ? ago(rule.lastTrigger.ms) : t("rules.never")]);
	for (const timer of rule.timers || []) {
		rows.push([t("rules.timer", { id: timer.id }), t("rules.timer_left", { duration: formatDurationShort(timer.remaining_ms / 1000) })]);
	}
	if (rule.errors > 0) {
		rows.push([t("rules.errors"), [h("span", { class: "error-text" }, t("rules.errors_value", { count: rule.errors })), " · ",
			h("a", { href: "/system", "data-route": "/system", class: "rule-details" }, t("rules.errors_details", { page: t("settings.system") }))]]);
	}
	const enable = rule.enabledAdjustable
		? switchControl(rule.enabled, rule.enabled ? t("rules.enabled") : t("rules.disabled"), (on) => commit("enabled", { value: on }).catch((error) => {
			feedback.textContent = errorText(error);
		}))
		: h("span", { class: `badge ${rule.enabled ? "ok" : "warn"}` }, rule.enabled ? t("rules.enabled") : t("rules.disabled"));
	const feedback = h("p", { class: "error-text", role: "status" });
	return h("section", { class: "card rule", "data-rule": rule.id, "aria-labelledby": `rule-${rule.id}` },
		h("div", { class: "card-head" }, h("h2", { id: `rule-${rule.id}` }, label(rule.name, rule.id)), enable),
		feedback,
		h("dl", { class: "rows" }, rows.map(([k, v]) => [h("dt", null, k), h("dd", null, v)])),
		rule.parameters.length ? h("div", { class: "params" }, rule.parameters.map((p) => parameterRow(rule, p, commit))) : null);
}

export function renderRules(content) {
	const list = h("div", { class: "cards rules" }, h("p", { class: "muted" }, t("app.loading")));
	const message = h("p", { class: "message error", role: "alert", hidden: true });
	const cards = new Map();
	let timer = null;
	let stopped = false;

	const place = (rule, nowMs) => {
		const commit = async (parameter, body) => {
			const answer = await api("PUT", `/api/rules/${encodeURIComponent(rule.id)}/parameters/${encodeURIComponent(parameter)}`, body);
			place(answer.rule, answer.now_ms);
		};
		const card = ruleCard(rule, nowMs, commit);
		const old = cards.get(rule.id);
		if (old && old.isConnected) {
			if (old.contains(document.activeElement) && document.activeElement.type === "range") {
				return; // being dragged: the next poll draws it
			}
			const focusedId = old.contains(document.activeElement) ? document.activeElement.id : null;
			old.replaceWith(card);
			if (focusedId) {
				const again = card.querySelector(`[id="${focusedId}"]`);
				again && again.focus();
			}
		} else {
			list.appendChild(card);
		}
		cards.set(rule.id, card);
	};

	async function load() {
		try {
			const all = [];
			let nowMs = 0;
			await pages("/api/rules", (page) => {
				all.push(...page.rules);
				nowMs = page.now_ms;
			});
			if (stopped) {
				return;
			}
			message.hidden = true;
			if (all.length === 0) {
				cards.clear();
				replace(list, h("div", { class: "empty" }, t("rules.empty")));
			} else {
				if (!cards.size) {
					list.textContent = "";
				}
				const ids = new Set(all.map((r) => r.id));
				for (const [id, card] of cards) {
					if (!ids.has(id)) {
						card.remove();
						cards.delete(id);
					}
				}
				all.forEach((rule) => place(rule, nowMs));
			}
		} catch (error) {
			message.textContent = error && error.code === "image.unavailable" ? t("rules.no_image_service") : errorText(error);
			message.hidden = false;
			replace(list);
		}
		if (!stopped) {
			timer = setTimeout(load, POLL_MS);
		}
	}

	content.append(h("p", { class: "muted intro" }, t("rules.intro")), message, list);
	load();
	return () => {
		stopped = true;
		clearTimeout(timer);
	};
}
