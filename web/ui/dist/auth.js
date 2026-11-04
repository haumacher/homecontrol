/* Copyright (c) 2026 Bernhard Haumacher */
// Onboarding (setting the device password once) and login.

import { api, errorText } from "/api.js";
import { h, icon } from "/dom.js";
import { t } from "/i18n.js";

const kMaxPassword = 64;
const logoPath = "M12 3 2 11h3v9h5v-6h4v6h5v-9h3z";

function utf8Length(text) {
	return new TextEncoder().encode(text).length;
}

/// The device name as the user name password managers file the password under (hidden).
function username(ctx) {
	return h("input", { type: "text", name: "username", autocomplete: "username", value: ctx.deviceName || "",
		hidden: true, readonly: true, tabindex: "-1", "aria-hidden": "true" });
}

/// The checkbox "Stay logged in" (homecontrol #53), unchecked: the device remembers the session
/// across restarts until logout or "log out everywhere".
function rememberBox() {
	const box = h("input", { type: "checkbox", id: "remember" });
	return { box, element: h("label", { class: "check", for: "remember" }, box, t("auth.remember")) };
}

function card(...children) {
	const logo = icon(logoPath);
	logo.classList.add("logo");
	return h("div", { class: "card auth-card" }, logo, ...children);
}

export function renderOnboarding(content, ctx) {
	const min = (ctx.session && ctx.session.min_password) || 8;
	const message = h("p", { class: "message error", role: "alert", hidden: true });
	const password = h("input", { type: "password", id: "pw", autocomplete: "new-password", required: true, autofocus: true });
	const repeat = h("input", { type: "password", id: "pw2", autocomplete: "new-password", required: true });
	const submit = h("button", { class: "button wide", type: "submit" }, t("auth.onboarding.submit"));
	const remember = rememberBox();
	const show = (text) => {
		message.textContent = text;
		message.hidden = !text;
	};
	const form = h(
		"form",
		{
			novalidate: true,
			onsubmit: async (event) => {
				event.preventDefault();
				const length = utf8Length(password.value);
				if (length < min) {
					return show(t("auth.password.too_short", { min }));
				}
				if (length > kMaxPassword) {
					return show(t("auth.password.too_long"));
				}
				if (password.value !== repeat.value) {
					return show(t("auth.password.mismatch"));
				}
				show("");
				submit.disabled = true;
				submit.textContent = t("auth.login.working");
				try {
					await api("POST", "/api/auth/onboard", { password: password.value, remember: remember.box.checked });
					await ctx.refreshSession();
					ctx.navigate("/", true);
				} catch (error) {
					show(errorText(error));
					if (error.code === "auth.password_set") {
						await ctx.refreshSession().catch(() => {});
						ctx.navigate("/login", true);
					}
				} finally {
					submit.disabled = false;
					submit.textContent = t("auth.onboarding.submit");
				}
			},
		},
		username(ctx),
		h("div", { class: "field" }, h("label", { for: "pw" }, t("auth.password")), password,
			h("span", { class: "hint" }, t("auth.password.hint", { min }))),
		h("div", { class: "field" }, h("label", { for: "pw2" }, t("auth.password.repeat")), repeat),
		remember.element,
		message,
		submit,
	);
	content.appendChild(card(h("h2", null, t("auth.onboarding.title")), h("p", null, t("auth.onboarding.text")), form));
	password.focus();
}

export function renderLogin(content, ctx) {
	const message = h("p", { class: "message error", role: "alert", hidden: true });
	const password = h("input", { type: "password", id: "pw", autocomplete: "current-password", required: true, autofocus: true });
	const submit = h("button", { class: "button wide", type: "submit" }, t("auth.login.submit"));
	const remember = rememberBox();
	let countdown = null;
	const show = (text) => {
		message.textContent = text;
		message.hidden = !text;
	};
	const wait = (base, seconds) => {
		clearInterval(countdown);
		submit.disabled = true;
		let left = seconds;
		const tick = () => {
			show(`${base} ${t("auth.login.wait", { seconds: left })}`);
			if (left-- <= 0) {
				clearInterval(countdown);
				submit.disabled = false;
				show(base);
			}
		};
		tick();
		countdown = setInterval(tick, 1000);
	};
	const form = h(
		"form",
		{
			novalidate: true,
			onsubmit: async (event) => {
				event.preventDefault();
				show("");
				submit.disabled = true;
				submit.textContent = t("auth.login.working");
				try {
					await api("POST", "/api/auth/login", { password: password.value, remember: remember.box.checked });
					await ctx.refreshSession();
					ctx.navigate("/", true);
				} catch (error) {
					submit.disabled = false;
					let text = errorText(error);
					const body = error.body || {};
					if (error.code === "auth.wrong_password" && body.free_failures && body.failures < body.free_failures) {
						text += " " + t("auth.login.attempts", { left: body.free_failures - body.failures });
					}
					if (error.retryAfter > 0) {
						wait(text, error.retryAfter);
					} else {
						show(text);
					}
					if (error.code === "auth.onboarding_required") {
						await ctx.refreshSession().catch(() => {});
						ctx.navigate("/onboarding", true);
					}
					password.select();
				} finally {
					submit.textContent = t("auth.login.submit");
				}
			},
		},
		username(ctx),
		h("div", { class: "field" }, h("label", { for: "pw" }, t("auth.password")), password),
		remember.element,
		message,
		submit,
	);
	content.appendChild(card(h("h2", null, t("auth.login.title")),
		h("p", null, t("auth.login.text", { device: ctx.deviceName })), form));
	password.focus();
	return () => clearInterval(countdown);
}
