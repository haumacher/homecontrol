/* Copyright (c) 2026 Bernhard Haumacher */
// Schedules: the image's schedule entries with their enable switches (where the image lets the
// inhabitant switch them), the next firing and the last one, and the members the image declares
// adjustable (spec/schedules.md 2.1, issue #58): a time picker on the member's step, day toggles,
// offsets and intervals in minutes, each within its bounds; a changed value is marked and the entry
// can return to the project's values. Polled every 30 s.

import { api, errorText } from "/api.js";
import { h, replace } from "/dom.js";
import { formatRelative, formatWhen } from "/format.js";
import { t, has } from "/i18n.js";
import { pages } from "/model.js";

const POLL_MS = 30000;

const TIME_MEMBERS = ["time", "notBefore", "notAfter", "from", "until"];

function two(n) {
	return String(n).padStart(2, "0");
}

/// A time of day for an <input type="time">: seconds only when the step needs them.
function clockText(v, seconds) {
	return `${two(v.hour)}:${two(v.minute)}${seconds ? `:${two(v.second)}` : ""}`;
}

function parseClock(text) {
	const [hour, minute, second] = text.split(":").map(Number);
	return { hour, minute, second: second || 0 };
}

function dateTimeText(v) {
	return `${v.year}-${two(v.month)}-${two(v.day)}T${two(v.hour)}:${two(v.minute)}`;
}

function parseDateTime(text) {
	const [date, time] = text.split("T");
	const [year, month, day] = date.split("-").map(Number);
	return { year, month, day, ...parseClock(time) };
}

/// A member's value for people: 06:30, Mo–Fr, −30 min, 2026-12-31 23:59.
function valueText(member, v) {
	if (TIME_MEMBERS.includes(member)) {
		return clockText(v, v.second !== 0);
	}
	if (member === "days") {
		return v.map((d) => t(`schedules.day.${d}`)).join(" ");
	}
	if (member === "at") {
		return `${v.year}-${two(v.month)}-${two(v.day)} ${clockText(v, v.second !== 0)}`;
	}
	const minutes = v / 60000;
	return t("schedules.minutes", { n: member === "offset" && minutes > 0 ? `+${minutes}` : String(minutes) });
}

function memberControl(entry, member, id, send) {
	const value = entry.values[member];
	const bound = entry.bounds[member];
	if (TIME_MEMBERS.includes(member)) {
		// The step in seconds; the browser's default of a minute when the image gives none.
		const step = bound && bound.step ? bound.step : 60;
		const seconds = step % 60 !== 0 || value.second !== 0;
		return h("input", { type: "time", id, step, value: clockText(value, seconds),
			min: bound ? clockText(bound.min, seconds) : undefined, max: bound ? clockText(bound.max, seconds) : undefined,
			onchange: (e) => e.target.value && send({ [member]: parseClock(e.target.value) }) });
	}
	if (member === "days") {
		const chosen = new Set(value);
		return h("div", { class: "segmented day-toggles", role: "group", id, "aria-label": t("schedules.member.days") },
			[1, 2, 3, 4, 5, 6, 7].map((day) => h("button", { type: "button", "aria-pressed": chosen.has(day) ? "true" : "false",
				title: t(`schedules.day.${day}`),
				onclick: () => {
					const next = new Set(chosen);
					next.has(day) ? next.delete(day) : next.add(day);
					send({ days: [...next].sort() });
				} }, t(`schedules.day.${day}`))));
	}
	if (member === "at") {
		return h("input", { type: "datetime-local", id, step: 60, value: dateTimeText(value),
			min: bound ? dateTimeText(bound.min) : undefined, max: bound ? dateTimeText(bound.max) : undefined,
			onchange: (e) => e.target.value && send({ at: parseDateTime(e.target.value) }) });
	}
	// offset, every: milliseconds, edited in minutes.
	const step = bound && bound.step ? bound.step / 60000 : 1;
	return h("input", { type: "number", id, class: "number", value: value / 60000, step,
		min: bound ? bound.min / 60000 : undefined, max: bound ? bound.max / 60000 : undefined,
		onchange: (e) => send({ [member]: Math.round(Number(e.target.value) * 60) * 1000 }) });
}

function memberRow(entry, member, send) {
	const id = `schedule-${entry.id}-${member}`;
	const overridden = entry.overridden.includes(member);
	const bound = entry.bounds[member];
	const hints = [];
	if (bound) {
		hints.push(t("schedules.bounds", { min: valueText(member, bound.min), max: valueText(member, bound.max) }));
	}
	if (overridden) {
		hints.push(t("schedules.project_value", { value: valueText(member, entry.image[member]) }));
	}
	return h("div", { class: "param" },
		h("div", { class: "param-head" },
			h("label", { for: id }, t(`schedules.member.${member}`)),
			overridden ? h("span", { class: "badge" }, t("schedules.changed")) : null),
		h("div", { class: "param-control" }, memberControl(entry, member, id, send)),
		hints.length ? h("span", { class: "hint" }, hints.join(" · ")) : null);
}

function entryRow(entry, now, synchronized, onChange) {
	const id = `schedule-${entry.id}`;
	const feedback = h("span", { class: "error-text", role: "status" });
	const next = !entry.enabled ? t("schedules.skipped")
		: entry.nextFiring !== null ? t("schedules.next", { when: `${formatWhen(entry.nextFiring, now)} (${formatRelative(entry.nextFiring, now)})` })
		: t("schedules.next", { when: synchronized ? t("schedules.none") : t("schedules.no_time") });
	const send = async (body, control) => {
		if (control) {
			control.disabled = true;
		}
		feedback.textContent = "";
		try {
			await onChange(entry, body);
		} catch (error) {
			feedback.textContent = errorText(error);
			if (control) {
				control.disabled = false;
			}
		}
	};
	const toggle = h("input", { type: "checkbox", role: "switch", id, checked: entry.enabled, disabled: !entry.enabledAdjustable,
		onchange: (e) => {
			const wanted = e.target.checked;
			e.target.checked = entry.enabled; // redrawn on success
			send({ enabled: wanted }, e.target);
		} });
	const adjustable = entry.adjustable || [];
	const changed = (entry.overridden && entry.overridden.length > 0) || entry.enabledOverridden;
	return h("li", { class: "schedule" },
		h("div", { class: "schedule-row" },
			h("div", { class: "schedule-text" },
				h("label", { for: id, class: "schedule-name" }, entry.label || entry.id),
				h("span", { class: "muted" }, has(`schedule.kind.${entry.kind}`) ? t(`schedule.kind.${entry.kind}`) : entry.kind),
				h("span", null, next),
				entry.lastFired !== null ? h("span", { class: "muted" }, t("schedules.last", { when: formatWhen(entry.lastFired, now) })) : null,
				!entry.enabledAdjustable ? h("span", { class: "muted" }, t("schedules.fixed")) : null),
			h("label", { class: "switch" }, toggle, h("span", { class: "switch-track", "aria-hidden": "true" }),
				h("span", { class: "switch-label" }, entry.enabled ? t("schedules.on") : t("schedules.off")))),
		adjustable.length ? h("div", { class: "schedule-members" }, adjustable.map((member) => memberRow(entry, member, (body) => send(body)))) : null,
		changed ? h("div", { class: "schedule-reset" },
			h("button", { type: "button", class: "button secondary small", onclick: (e) => send({ reset: true }, e.target) },
				t("schedules.reset"))) : null,
		feedback);
}

export function renderSchedules(content) {
	const list = h("ul", { class: "card schedule-list" }, h("li", { class: "muted" }, t("app.loading")));
	const message = h("p", { class: "message error", role: "alert", hidden: true });
	let timer = null;
	let stopped = false;

	async function change(entry, body) {
		await api("PUT", `/api/schedules/${encodeURIComponent(entry.id)}`, body);
		await load(false);
	}

	async function load(again = true) {
		try {
			const all = [];
			let now = Date.now() / 1000;
			let synchronized = true;
			await pages("/api/schedules", (page) => {
				all.push(...page.schedules);
				now = page.now;
				synchronized = page.synchronized;
			});
			if (stopped) {
				return;
			}
			message.hidden = true;
			if (list.contains(document.activeElement) && again) {
				// leave the focused control alone; the next poll redraws
			} else {
				const focused = list.contains(document.activeElement) ? document.activeElement.id : null;
				replace(list, all.length ? all.map((e) => entryRow(e, now, synchronized, change)) : h("li", { class: "empty" }, t("schedules.empty")));
				const again2 = focused && list.querySelector(`[id="${focused}"]`);
				again2 && again2.focus();
			}
		} catch (error) {
			message.textContent = error && error.code === "image.unavailable" ? t("rules.no_image_service") : errorText(error);
			message.hidden = false;
			replace(list);
		}
		if (again && !stopped) {
			timer = setTimeout(load, POLL_MS);
		}
	}

	content.append(h("p", { class: "muted intro" }, t("schedules.intro")), message, list);
	load();
	return () => {
		stopped = true;
		clearTimeout(timer);
	};
}
