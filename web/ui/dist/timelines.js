/* Copyright (c) 2026 Bernhard Haumacher */
// Time programs on the schedules page (spec/rules.md 2.8, 8.7; issue #84): the rules with a
// timeline, each with its current state and next switching point, and the editor of the
// inhabitant's timeline: day plans with their points (a clock time or a sun event with an offset and
// optional bounds, and the state by its label) and the assignment of plans to weekdays. The timeline
// is replaced as a whole (PUT /api/rules/<rule>/timeline) and can return to the project's (DELETE);
// read-only where the project fixes it. A refusal shows the device's reason and where.

import { api, errorText } from "/api.js";
import { h, replace } from "/dom.js";
import { formatRelative, formatWhen } from "/format.js";
import { t } from "/i18n.js";
import { label, pages } from "/model.js";

const SUN_EVENTS = ["dawn", "sunrise", "sunset", "dusk"];

function two(n) {
	return String(n).padStart(2, "0");
}

function timeText(v) {
	return `${two(v.hour)}:${two(v.minute)}${v.second ? `:${two(v.second)}` : ""}`;
}

function parseTime(text) {
	const [hour, minute, second] = text.split(":").map(Number);
	return { hour, minute, second: second || 0 };
}

/// The state's label by its id (the rule's `states` of GET /api/rules).
function stateLabel(rule, id) {
	const state = (rule.states || []).find((s) => s.id === id);
	return state ? label(state.label, state.id) : id;
}

/// A point for people: "06:00", "Sunset +60 min, not after 22:00".
function momentText(point) {
	if (point.time) {
		return timeText(point.time);
	}
	const parts = [t(`timelines.sun.${point.sun}`)];
	if (point.offset) {
		const minutes = point.offset / 60000;
		parts[0] += ` ${minutes > 0 ? "+" : ""}${minutes} min`;
	}
	if (point.notBefore) {
		parts.push(`${t("timelines.not_before")} ${timeText(point.notBefore)}`);
	}
	if (point.notAfter) {
		parts.push(`${t("timelines.not_after")} ${timeText(point.notAfter)}`);
	}
	return parts.join(", ");
}

/// The draft's day mapping: the default plan and, per weekday 1..7, its plan or null (the default).
function draftOf(timeline) {
	const days = [null, null, null, null, null, null, null];
	let fallback = timeline.plans.length ? timeline.plans[0].id : null;
	for (const a of timeline.assignments) {
		if (!a.days) {
			fallback = a.plan;
		} else {
			for (const d of a.days) {
				days[d - 1] = a.plan;
			}
		}
	}
	return {
		plans: timeline.plans.map((p) => ({ id: p.id, label: p.label, points: p.points.map((pt) => ({ ...pt })) })),
		fallback,
		days,
	};
}

/// The timeline to send: the plans and the assignments, the default first, then one per plan with its days.
function timelineOf(draft) {
	const assignments = [{ plan: draft.fallback }];
	for (const plan of draft.plans) {
		const days = [];
		draft.days.forEach((p, i) => {
			if (p === plan.id && p !== draft.fallback) {
				days.push(i + 1);
			}
		});
		if (days.length) {
			assignments.push({ days, plan: plan.id });
		}
	}
	return { plans: draft.plans, assignments };
}

function pointRow(rule, point, index, plan, readOnly, redraw) {
	const kind = point.time ? "clock" : point.sun;
	const kindSelect = h("select", { disabled: readOnly, "aria-label": t("timelines.moment"),
		onchange: (e) => {
			const next = e.target.value;
			for (const k of ["time", "sun", "offset", "notBefore", "notAfter"]) {
				delete point[k];
			}
			if (next === "clock") {
				point.time = { hour: 6, minute: 0, second: 0 };
			} else {
				point.sun = next;
			}
			redraw();
		} },
		h("option", { value: "clock", selected: kind === "clock" }, t("timelines.clock")),
		SUN_EVENTS.map((e) => h("option", { value: e, selected: kind === e }, t(`timelines.sun.${e}`))));
	const controls = [kindSelect];
	if (point.time) {
		controls.push(h("input", { type: "time", step: point.time.second ? 1 : 60, value: timeText(point.time), disabled: readOnly,
			"aria-label": t("timelines.moment"),
			onchange: (e) => {
				if (e.target.value) {
					point.time = parseTime(e.target.value);
				}
			} }));
	} else {
		controls.push(h("label", { class: "inline" }, t("timelines.offset"),
			h("input", { type: "number", class: "number", step: 1, min: -720, max: 720, value: (point.offset || 0) / 60000, disabled: readOnly,
				onchange: (e) => {
					const minutes = Math.round(Number(e.target.value) || 0);
					if (minutes) {
						point.offset = minutes * 60000;
					} else {
						delete point.offset;
					}
				} })));
		for (const bound of ["notBefore", "notAfter"]) {
			controls.push(h("label", { class: "inline" }, t(bound === "notBefore" ? "timelines.not_before" : "timelines.not_after"),
				h("input", { type: "time", step: 60, value: point[bound] ? timeText(point[bound]) : "", disabled: readOnly,
					onchange: (e) => {
						if (e.target.value) {
							point[bound] = parseTime(e.target.value);
						} else {
							delete point[bound];
						}
					} })));
		}
	}
	controls.push(h("select", { disabled: readOnly, "aria-label": t("timelines.state_label"),
		onchange: (e) => {
			point.state = e.target.value;
		} },
		(rule.states || []).map((s) => h("option", { value: s.id, selected: s.id === point.state }, label(s.label, s.id)))));
	if (!readOnly) {
		controls.push(h("button", { type: "button", class: "button secondary small", onclick: () => {
			plan.points.splice(index, 1);
			redraw();
		} }, t("timelines.remove_point")));
	}
	return h("li", { class: "timeline-point" }, controls);
}

function planSection(rule, draft, plan, readOnly, redraw) {
	const points = plan.points.length
		? h("ul", { class: "timeline-points" }, plan.points.map((p, i) => pointRow(rule, p, i, plan, readOnly, redraw)))
		: h("p", { class: "muted" }, t("timelines.empty_plan"));
	return h("fieldset", { class: "timeline-plan" },
		h("legend", null, label(plan.label, plan.id)),
		readOnly ? null : h("label", { class: "inline" }, t("timelines.plan_label"),
			h("input", { type: "text", maxlength: 63, value: label(plan.label, plan.id), onchange: (e) => {
				const text = e.target.value.trim();
				if (text && text !== label(plan.label, plan.id)) {
					plan.label = text;
				}
			} })),
		points,
		readOnly ? null : h("div", { class: "timeline-actions" },
			h("button", { type: "button", class: "button secondary small", disabled: plan.points.length >= 16, onclick: () => {
				const last = plan.points[plan.points.length - 1];
				plan.points.push({ time: { hour: last && last.time ? Math.min(last.time.hour + 1, 23) : 6, minute: 0, second: 0 },
					state: (rule.states && rule.states[0] && rule.states[0].id) || "" });
				redraw();
			} }, t("timelines.add_point")),
			draft.plans.length > 1 ? h("button", { type: "button", class: "button secondary small", onclick: () => {
				draft.plans.splice(draft.plans.indexOf(plan), 1);
				const first = draft.plans[0].id;
				if (draft.fallback === plan.id) {
					draft.fallback = first;
				}
				draft.days = draft.days.map((d) => (d === plan.id ? null : d));
				redraw();
			} }, t("timelines.remove_plan")) : null));
}

function assignmentSection(draft, readOnly, redraw) {
	const options = (selected, withDefault) => [
		withDefault ? h("option", { value: "", selected: selected === null }, t("timelines.day_default")) : null,
		draft.plans.map((p) => h("option", { value: p.id, selected: p.id === selected }, label(p.label, p.id))),
	];
	return h("fieldset", { class: "timeline-assign" },
		h("legend", null, t("timelines.assign")),
		h("label", { class: "inline" }, t("timelines.default_plan"),
			h("select", { disabled: readOnly, onchange: (e) => {
				draft.fallback = e.target.value;
				redraw();
			} }, options(draft.fallback, false))),
		h("div", { class: "timeline-days" }, [1, 2, 3, 4, 5, 6, 7].map((d) => h("label", { class: "inline" }, t(`schedules.day.${d}`),
			h("select", { disabled: readOnly, onchange: (e) => {
				draft.days[d - 1] = e.target.value || null;
			} }, options(draft.days[d - 1], true))))));
}

/// The editor of one rule's timeline; `done(rule)` after a save or a reset.
function editor(rule, timeline, done) {
	const readOnly = !timeline.adjustable;
	const draft = draftOf(timeline);
	const box = h("div", { class: "timeline-editor" });
	const feedback = h("p", { class: "error-text", role: "status" });
	let planCounter = draft.plans.length;
	const redraw = () => {
		replace(box,
			readOnly ? h("p", { class: "muted" }, t("timelines.fixed")) : null,
			draft.plans.map((p) => planSection(rule, draft, p, readOnly, redraw)),
			readOnly || draft.plans.length >= 8 ? null : h("button", { type: "button", class: "button secondary small", onclick: () => {
				let id;
				do {
					planCounter += 1;
					id = `plan${planCounter}`;
				} while (draft.plans.some((p) => p.id === id));
				draft.plans.push({ id, label: t("timelines.new_plan", { n: planCounter }), points: [] });
				redraw();
			} }, t("timelines.add_plan")),
			assignmentSection(draft, readOnly, redraw),
			readOnly ? null : h("div", { class: "timeline-actions" },
				h("button", { type: "button", class: "button", onclick: async (e) => {
					e.target.disabled = true;
					feedback.className = "hint";
					feedback.textContent = t("rules.saving");
					try {
						const answer = await api("PUT", `/api/rules/${encodeURIComponent(rule.id)}/timeline`, { timeline: timelineOf(draft) });
						feedback.textContent = t("timelines.saved");
						done(answer.rule);
					} catch (error) {
						feedback.className = "error-text";
						const where = error && error.body && error.body.path ? ` ${t("timelines.at", { path: error.body.path })}` : "";
						feedback.textContent = `${errorText(error)}${where}`;
						e.target.disabled = false;
					}
				} }, t("timelines.save")),
				timeline.overridden ? h("button", { type: "button", class: "button secondary", onclick: async (e) => {
					e.target.disabled = true;
					try {
						const answer = await api("DELETE", `/api/rules/${encodeURIComponent(rule.id)}/timeline`);
						done(answer.rule);
					} catch (error) {
						feedback.className = "error-text";
						feedback.textContent = errorText(error);
						e.target.disabled = false;
					}
				} }, t("timelines.reset")) : null),
			feedback);
	};
	redraw();
	return box;
}

async function loadTimeline(rule) {
	const timeline = { plans: [], assignments: [], adjustable: true, overridden: false };
	await pages(`/api/rules/${encodeURIComponent(rule.id)}/timeline`, (page) => {
		timeline.plans.push(...page.plans);
		timeline.assignments = page.assignments;
		timeline.adjustable = page.adjustable;
		timeline.overridden = page.overridden;
	});
	return timeline;
}

function programCard(rule, open, onOpen, onDone) {
	const now = Date.now() / 1000;
	const next = rule.timeline.next
		? t("timelines.next", { state: stateLabel(rule, rule.timeline.next.state),
			when: `${formatWhen(rule.timeline.next.at, now)} (${formatRelative(rule.timeline.next.at, now)})` })
		: t("timelines.next_none");
	const body = h("div", { class: "timeline-body" });
	const card = h("li", { class: "schedule timeline", "data-rule": rule.id },
		h("div", { class: "schedule-row" },
			h("div", { class: "schedule-text" },
				h("span", { class: "schedule-name" }, label(rule.name, rule.id)),
				h("span", null, t("timelines.state", { state: rule.state ? stateLabel(rule, rule.state) : t("value.unknown") })),
				h("span", { class: "muted" }, next),
				!rule.enabled ? h("span", { class: "muted" }, t("rules.disabled")) : null),
			rule.timeline.overridden ? h("span", { class: "badge" }, t("timelines.changed")) : null,
			h("button", { type: "button", class: "button secondary small", "aria-expanded": open ? "true" : "false",
				onclick: () => onOpen(open ? null : rule.id) },
			open ? t("timelines.close") : rule.timeline.adjustable ? t("timelines.edit") : t("timelines.view"))),
		body);
	if (open) {
		body.append(h("p", { class: "muted" }, t("app.loading")));
		loadTimeline(rule).then((timeline) => replace(body, editor(rule, timeline, onDone)))
			.catch((error) => replace(body, h("p", { class: "error-text" }, errorText(error))));
	}
	return card;
}

/// The section of time programs; returns {element, load(), stop()}: load() polls nothing while an
/// editor is open, so the inhabitant's draft is kept.
export function timelinesSection() {
	const list = h("ul", { class: "card schedule-list timelines" });
	const section = h("section", { class: "timelines-section", hidden: true },
		h("h2", null, t("timelines.title")), h("p", { class: "muted intro" }, t("timelines.intro")), list);
	let rules = [];
	let open = null;
	const draw = () => {
		section.hidden = rules.length === 0;
		replace(list, rules.map((r) => programCard(r, r.id === open, (id) => {
			open = id;
			draw();
		}, (rule) => {
			rules = rules.map((r) => (r.id === rule.id ? rule : r));
			open = null;
			draw();
		})));
	};
	return {
		element: section,
		async load() {
			if (open !== null) {
				return; // an editor is open: keep the draft
			}
			const all = [];
			await pages("/api/rules", (page) => all.push(...page.rules.filter((r) => r.timeline)));
			rules = all;
			draw();
		},
	};
}
