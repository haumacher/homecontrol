/* Copyright (c) 2026 Bernhard Haumacher */
// Schedules: the image's schedule entries with their enable switches (where the image lets the
// inhabitant switch them), the next firing and the last one. Polled every 30 s.

import { api, errorText } from "/api.js";
import { h, replace } from "/dom.js";
import { formatRelative, formatWhen } from "/format.js";
import { t, has } from "/i18n.js";
import { pages } from "/model.js";

const POLL_MS = 30000;

function entryRow(entry, now, synchronized, onToggle) {
	const id = `schedule-${entry.id}`;
	const feedback = h("span", { class: "error-text", role: "status" });
	const next = !entry.enabled ? t("schedules.skipped")
		: entry.nextFiring !== null ? t("schedules.next", { when: `${formatWhen(entry.nextFiring, now)} (${formatRelative(entry.nextFiring, now)})` })
		: t("schedules.next", { when: synchronized ? t("schedules.none") : t("schedules.no_time") });
	const toggle = h("input", { type: "checkbox", role: "switch", id, checked: entry.enabled, disabled: !entry.adjustable,
		onchange: async (e) => {
			e.target.disabled = true;
			feedback.textContent = "";
			try {
				await onToggle(entry, e.target.checked);
			} catch (error) {
				e.target.checked = entry.enabled;
				feedback.textContent = errorText(error);
				e.target.disabled = false;
			}
		} });
	return h("li", { class: "schedule" },
		h("div", { class: "schedule-text" },
			h("label", { for: id, class: "schedule-name" }, entry.label || entry.id),
			h("span", { class: "muted" }, has(`schedule.kind.${entry.kind}`) ? t(`schedule.kind.${entry.kind}`) : entry.kind),
			h("span", null, next),
			entry.lastFired !== null ? h("span", { class: "muted" }, t("schedules.last", { when: formatWhen(entry.lastFired, now) })) : null,
			!entry.adjustable ? h("span", { class: "muted" }, t("schedules.fixed")) : null,
			feedback),
		h("label", { class: "switch" }, toggle, h("span", { class: "switch-track", "aria-hidden": "true" }),
			h("span", { class: "switch-label" }, entry.enabled ? t("schedules.on") : t("schedules.off"))));
}

export function renderSchedules(content) {
	const list = h("ul", { class: "card schedule-list" }, h("li", { class: "muted" }, t("app.loading")));
	const message = h("p", { class: "message error", role: "alert", hidden: true });
	let timer = null;
	let stopped = false;

	async function toggle(entry, enabled) {
		await api("PUT", `/api/schedules/${encodeURIComponent(entry.id)}`, { enabled });
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
				// leave the focused switch alone; the next poll redraws
			} else {
				const focused = list.contains(document.activeElement) ? document.activeElement.id : null;
				replace(list, all.length ? all.map((e) => entryRow(e, now, synchronized, toggle)) : h("li", { class: "empty" }, t("schedules.empty")));
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
