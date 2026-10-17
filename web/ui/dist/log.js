/* Copyright (c) 2026 Bernhard Haumacher */
// The log view: the lines the controller logged since boot (GET /api/log), polled every 3 s.

import { api, errorText } from "/api.js";
import { h } from "/dom.js";
import { t, language } from "/i18n.js";

const kPollMs = 3000;
const kMaxLines = 1000;

function time(ms, nowMs) {
	// Device monotonic time -> wall clock of this browser (good enough to read the log).
	const date = new Date(Date.now() - (nowMs - ms));
	try {
		return date.toLocaleTimeString(language(), { hour12: false });
	} catch (e) {
		return date.toLocaleTimeString(undefined, { hour12: false });
	}
}

export function renderLog(content) {
	const list = h("div", { class: "log", role: "log" });
	const message = h("p", { class: "message error", role: "alert", hidden: true });
	const dropped = h("p", { class: "message", hidden: true });
	const follow = h("input", { type: "checkbox", id: "follow", checked: true });
	let after = 0;
	let timer = null;
	let stopped = false;
	let count = 0;

	async function poll() {
		try {
			for (;;) {
				const data = await api("GET", `/api/log?after=${after}`);
				message.hidden = true;
				if (data.first > after + 1 && after > 0) {
					list.appendChild(h("div", { class: "log-line muted" }, "…"));
				}
				for (const line of data.lines) {
					list.appendChild(h("div", { class: `log-line level-${line.level}` },
						h("span", { class: "time" }, time(line.ms, data.now_ms)),
						h("span", { class: "level" }, line.level),
						h("span", { class: "tag" }, line.tag),
						h("span", { class: "text" }, line.message)));
					count++;
				}
				while (count > kMaxLines) {
					list.removeChild(list.firstChild);
					count--;
				}
				after = data.next_after;
				dropped.hidden = !data.dropped;
				dropped.textContent = t("log.dropped", { count: data.dropped });
				if (!data.more || stopped) {
					break;
				}
			}
			if (count === 0) {
				list.textContent = "";
				list.appendChild(h("div", { class: "empty" }, t("log.empty")));
			} else if (list.querySelector(".empty")) {
				list.querySelector(".empty").remove();
			}
			if (follow.checked) {
				list.scrollTop = list.scrollHeight;
			}
		} catch (error) {
			message.textContent = errorText(error);
			message.hidden = false;
		}
		if (!stopped) {
			timer = setTimeout(poll, kPollMs);
		}
	}

	content.appendChild(h("div", { class: "toolbar" },
		h("label", null, follow, " ", t("log.follow"))));
	content.appendChild(message);
	content.appendChild(dropped);
	content.appendChild(list);
	poll();
	return () => {
		stopped = true;
		clearTimeout(timer);
	};
}
