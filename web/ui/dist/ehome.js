/* Copyright (c) 2026 Bernhard Haumacher */
// The E-Home card of the Settings page (issue #41, doc/ehome.md, spec/api.md "E-Home connection"):
// the connection's state from the live status (its `ehome` member), the owner's switch
// (ehome.enabled), "Connect to E-Home" with the address and the one-time code E-Home shows, and
// "Disconnect", which forgets the enrollment; the installation key's fingerprint and, when E-Home
// offers a new key, its fingerprint with "Accept key" (issue #35, spec/api.md "Installation key").
// Every refusal shows its reason.

import { api, errorText } from "/api.js";
import { h, replace } from "/dom.js";
import { formatDuration } from "/format.js";
import { t, tv } from "/i18n.js";

const kBadge = { connected: "ok", connecting: "warn", failed: "warn", off: "" };

/// The state line: off, connecting, connected (since), failed with E-Home's or the network's reason.
function stateLine(e) {
	const state = e.state || "off";
	const parts = [h("span", { class: `badge ${kBadge[state] || ""}` }, tv("ehome.state.", state))];
	if (state === "connected" && e.since_s !== undefined) {
		parts.push(" ", h("span", { class: "hint" }, t("ehome.since", { duration: formatDuration(e.since_s) })));
	}
	if (e.error) {
		parts.push(h("p", { class: "error-text" }, e.error));
	}
	return h("div", { class: "ehome-state", role: "status" }, parts);
}

function connectForm(onDone) {
	const url = h("input", { type: "url", id: "ehome-url", required: true, spellcheck: "false", autocomplete: "off",
		placeholder: "https://ehome.example" });
	const code = h("input", { type: "text", id: "ehome-code", required: true, spellcheck: "false", autocomplete: "off",
		maxlength: 32 });
	const feedback = h("p", { class: "hint", role: "status" }, t("ehome.connect.hint"));
	const submit = h("button", { class: "button", type: "submit" }, t("ehome.connect"));
	return h("form", {
		onsubmit: async (event) => {
			event.preventDefault();
			submit.disabled = true;
			feedback.className = "hint";
			feedback.textContent = t("ehome.connecting");
			try {
				const result = await api("POST", "/api/ehome/enroll", { url: url.value.trim(), code: code.value.trim() });
				feedback.textContent = t("ehome.connected_to", { installation: result.installation || "" });
				onDone();
			} catch (error) {
				feedback.className = "error-text";
				feedback.textContent = errorText(error);
			} finally {
				submit.disabled = false;
			}
		},
	},
	h("div", { class: "field" }, h("label", { for: "ehome-url" }, t("ehome.address")), url),
	h("div", { class: "field" }, h("label", { for: "ehome-code" }, t("ehome.code")), code),
	h("div", { class: "actions" }, submit),
	feedback);
}

/// A new installation key E-Home offered: its fingerprint, to compare with E-Home's, and "Accept key".
/// The fingerprint goes with the request, so the device accepts only the key the owner saw.
function pendingKey(fingerprint, onDone) {
	const feedback = h("p", { class: "hint", role: "status" });
	const accept = h("button", { type: "button", class: "button small" }, t("ehome.key.accept"));
	accept.addEventListener("click", async () => {
		if (!window.confirm(t("ehome.key.accept.confirm", { fingerprint }))) {
			return;
		}
		accept.disabled = true;
		try {
			await api("POST", "/api/ehome/key/accept", { fingerprint });
			feedback.className = "hint";
			feedback.textContent = t("ehome.key.accepted", { fingerprint });
			onDone();
		} catch (error) {
			feedback.className = "error-text";
			feedback.textContent = errorText(error);
		} finally {
			accept.disabled = false;
		}
	});
	return h("div", { class: "ehome-key", role: "alert" },
		h("dl", { class: "rows" }, h("dt", null, t("ehome.key.pending")), h("dd", null, h("code", null, fingerprint))),
		h("p", { class: "hint" }, t("ehome.key.pending.hint")),
		h("div", { class: "actions" }, accept),
		feedback);
}

function enrolledControls(e, address, onDone) {
	const feedback = h("p", { class: "hint", role: "status" });
	const toggle = h("input", { type: "checkbox", role: "switch", checked: !!e.enabled });
	const label = h("span", { class: "switch-label" }, t(e.enabled ? "ehome.switch.on" : "ehome.switch.off"));
	toggle.addEventListener("change", async () => {
		toggle.disabled = true;
		feedback.className = "hint";
		feedback.textContent = "";
		try {
			await api("PUT", "/api/settings/ehome.enabled", { value: toggle.checked ? "true" : "false" });
			label.textContent = t(toggle.checked ? "ehome.switch.on" : "ehome.switch.off");
		} catch (error) {
			toggle.checked = !toggle.checked;
			feedback.className = "error-text";
			feedback.textContent = errorText(error);
		} finally {
			toggle.disabled = false;
		}
	});
	const disconnect = h("button", { type: "button", class: "button small danger" }, t("ehome.disconnect"));
	disconnect.addEventListener("click", async () => {
		const kept = e.key && e.key !== "damaged" ? ` ${t("ehome.disconnect.key_kept", { fingerprint: e.key })}` : "";
		if (!window.confirm(t("ehome.disconnect.confirm", { installation: e.installation || "" }) + kept)) {
			return;
		}
		disconnect.disabled = true;
		try {
			await api("DELETE", "/api/ehome");
			onDone();
		} catch (error) {
			feedback.className = "error-text";
			feedback.textContent = errorText(error);
		} finally {
			disconnect.disabled = false;
		}
	});
	const keyText = e.key === "damaged" ? t("ehome.key.damaged") : e.key ? h("code", null, e.key) : t("ehome.key.none");
	return [
		h("dl", { class: "rows" },
			h("dt", null, t("ehome.installation")), h("dd", null, e.installation || "—"),
			h("dt", null, t("ehome.address")), h("dd", null, address || "—"),
			h("dt", null, t("ehome.key")), h("dd", null, keyText)),
		e.pending_key ? pendingKey(e.pending_key, onDone) : null,
		h("label", { class: "switch" }, toggle, h("span", { class: "switch-track", "aria-hidden": "true" }), label),
		h("p", { class: "hint" }, t("ehome.switch.hint")),
		h("div", { class: "actions" }, disconnect),
		feedback,
	];
}

/// The card; `address` is the ehome.url setting (from /api/settings), `status` the live status.
/// Returns { element, update(status), setAddress(url) }: the state line follows every status, the
/// controls are rebuilt only when the enrollment or the switch changes (a form being filled in stays).
export function ehomeCard(reloadSettings) {
	const state = h("div", null);
	const body = h("div", null, h("p", { class: "muted" }, t("app.loading")));
	const element = h("section", { class: "card" }, h("h2", null, t("ehome.title")), state, body);
	let address = "";
	let shown = null;
	let last = null;
	const render = () => {
		const e = last || {};
		const key = `${!!e.enrolled}/${!!e.enabled}/${e.installation || ""}/${address}/${e.key || ""}/${e.pending_key || ""}`;
		if (key === shown) {
			return;
		}
		shown = key;
		// A key kept from an earlier pairing stays in force after "Disconnect" (spec/api.md "What clears the key").
		const keptKey = !e.enrolled && e.key
			? h("p", { class: "hint" }, e.key === "damaged" ? t("ehome.key.damaged") : t("ehome.key.kept", { fingerprint: e.key }))
			: null;
		replace(body, e.enrolled ? enrolledControls(e, address, reloadSettings) : [keptKey, connectForm(reloadSettings)]);
	};
	return {
		element,
		update(status) {
			if (!status || !status.ehome) {
				return;
			}
			last = status.ehome;
			replace(state, stateLine(last));
			render();
		},
		setAddress(url) {
			address = url || "";
			if (last) {
				render();
			}
		},
	};
}
