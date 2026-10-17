/* Copyright (c) 2026 Bernhard Haumacher */
// The shell: finds out whether the device is onboarded and the browser logged in, routes between the
// pages (history API; the device serves index.html for every path without an extension) and holds
// the live connection.

import { api, onAuthProblem, errorText } from "/api.js";
import { h, replace } from "/dom.js";
import { t, applyI18n, setLanguage } from "/i18n.js";
import { Live } from "/live.js";
import { renderLogin, renderOnboarding } from "/auth.js";
import { renderOverview } from "/overview.js";
import { renderSettings } from "/settings.js";
import { renderLog } from "/log.js";
import { renderRules } from "/rules.js";
import { renderSchedules } from "/schedules.js";
import { applyTheme } from "/theme.js";

const routes = {
	"/": { render: renderOverview, title: "overview.title" },
	"/rules": { render: renderRules, title: "rules.title" },
	"/schedules": { render: renderSchedules, title: "schedules.title" },
	"/settings": { render: renderSettings, title: "settings.title" },
	"/log": { render: renderLog, title: "log.title" },
	"/login": { render: renderLogin, bare: true },
	"/onboarding": { render: renderOnboarding, bare: true },
};

const app = document.getElementById("app");
const content = document.getElementById("content");
const title = document.getElementById("title");
const liveBadge = document.getElementById("live");
const sidebar = document.getElementById("sidebar");
const scrim = document.getElementById("scrim");

const ctx = {
	session: null,
	live: new Live(),
	navigate,
	refreshSession,
	setLanguage: changeLanguage,
	deviceName: "",
};

let cleanup = null;

function setDrawer(open) {
	sidebar.classList.toggle("open", open);
	scrim.hidden = !open;
}

async function refreshSession() {
	ctx.session = await api("GET", "/api/auth/session");
	ctx.deviceName = ctx.session.device || "";
	return ctx.session;
}

/// Switches the UI to `lang` (after the device stored it as ui.lang) and redraws the page.
async function changeLanguage(lang) {
	const found = await setLanguage(lang);
	render();
	return found;
}

function navigate(path, replaceState = false) {
	if (location.pathname !== path) {
		if (replaceState) {
			history.replaceState(null, "", path);
		} else {
			history.pushState(null, "", path);
		}
	}
	render();
}

function render() {
	setDrawer(false);
	let path = location.pathname;
	if (!routes[path]) {
		path = "/";
		history.replaceState(null, "", "/");
	}
	const session = ctx.session;
	if (session && !session.onboarded && path !== "/onboarding") {
		return navigate("/onboarding", true);
	}
	if (session && session.onboarded && !session.authenticated && path !== "/login") {
		return navigate("/login", true);
	}
	if (session && session.authenticated && (path === "/login" || path === "/onboarding")) {
		return navigate("/", true);
	}
	const route = routes[path];
	app.classList.toggle("bare", !!route.bare);
	for (const link of document.querySelectorAll("[data-route]")) {
		link.classList.toggle("active", link.dataset.route === path);
	}
	title.textContent = ctx.deviceName || t("app.title");
	document.title = route.title ? `${t(route.title)} – ${ctx.deviceName || t("app.title")}` : t("app.title");
	if (cleanup) {
		cleanup();
		cleanup = null;
	}
	content.textContent = "";
	if (session && session.authenticated && ctx.live.stopped !== false) {
		ctx.live.start(session.ws_port);
	}
	cleanup = route.render(content, ctx) || null;
}

function showLive(state, reason) {
	liveBadge.hidden = !(ctx.session && ctx.session.authenticated);
	liveBadge.classList.toggle("connected", state === "connected");
	if (state === "connected") {
		liveBadge.textContent = t("live.connected");
	} else if (state === "refused") {
		liveBadge.textContent = t("live.refused", { reason: reason ? errorText({ code: reason }) : "" });
	} else {
		liveBadge.textContent = t("live.connecting");
	}
}

async function start() {
	applyTheme();
	applyI18n();
	ctx.live.stopped = true;
	ctx.live.addEventListener("state", (e) => showLive(e.detail.state, e.detail.reason));
	ctx.live.addEventListener("unauthorized", async () => {
		ctx.live.stop();
		try {
			await refreshSession();
		} catch (e) {
			// stays on the current page; the next API call shows the reason
		}
		render();
	});
	onAuthProblem(
		async () => {
			ctx.live.stop();
			await refreshSession().catch(() => {});
			render();
		},
		async () => {
			ctx.live.stop();
			await refreshSession().catch(() => {});
			render();
		},
	);
	document.addEventListener("click", (event) => {
		const link = event.target.closest("a[data-route]");
		if (link && !event.ctrlKey && !event.metaKey && !event.shiftKey) {
			event.preventDefault();
			navigate(link.dataset.route);
		}
	});
	window.addEventListener("popstate", render);
	document.getElementById("menu-button").addEventListener("click", () => setDrawer(true));
	scrim.addEventListener("click", () => setDrawer(false));
	for (;;) {
		try {
			await refreshSession();
			await setLanguage(ctx.session.lang);
			break;
		} catch (error) {
			replace(content, h("p", { class: "message error" }, errorText(error)), h("p", { class: "muted" }, t("app.offline")));
			await new Promise((resolve) => setTimeout(resolve, 3000));
		}
	}
	render();
}

start();
