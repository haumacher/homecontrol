/* Copyright (c) 2026 Bernhard Haumacher */
// The shell: finds out whether the device is onboarded and the browser logged in, routes between the
// pages (history API; the device serves index.html for every path without an extension) and holds
// the live connection. The control pages of the image (GET /api/pages) come first in the navigation,
// the first one as the start page "/", the others at /page/<id>; without them the generated overview
// is the start page.

import { api, onAuthProblem, errorText } from "/api.js";
import { h, icon, icons, replace } from "/dom.js";
import { t, applyI18n, setLanguage } from "/i18n.js";
import { Live } from "/live.js";
import { renderLogin, renderOnboarding } from "/auth.js";
import { renderOverview } from "/overview.js";
import { loadPages, renderPage } from "/pages.js";
import { label } from "/model.js";
import { renderSettings } from "/settings.js";
import { renderLog } from "/log.js";
import { renderDiagnostics } from "/diagnostics.js";
import { renderRules } from "/rules.js";
import { renderSchedules } from "/schedules.js";
import { renderZigbee } from "/zigbee.js";
import { applyTheme } from "/theme.js";

const routes = {
	"/": { render: renderOverview, title: "overview.title" },
	"/rules": { render: renderRules, title: "rules.title" },
	"/schedules": { render: renderSchedules, title: "schedules.title" },
	"/zigbee": { render: renderZigbee, title: "zigbee.title" },
	"/settings": { render: renderSettings, title: "settings.title" },
	"/log": { render: renderLog, title: "log.title" },
	"/system": { render: renderDiagnostics, title: "settings.system" },
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
let pageList = [];
let pagesLoaded = false;
let pagesKey = null; // project/revision the pages were read for

/// Reads the control pages again (after the login and whenever another image runs).
async function refreshPages() {
	try {
		pageList = await loadPages();
	} catch (error) {
		pageList = []; // the overview stays; its own load shows the reason
	}
	pagesLoaded = true;
}

function pagePath(index) {
	return index === 0 ? "/" : `/page/${encodeURIComponent(pageList[index].id)}`;
}

/// One navigation entry per page, in image order, before the overview's (hidden while there are pages).
function buildNav() {
	const nav = document.getElementById("nav");
	const overview = document.getElementById("nav-overview");
	for (const link of nav.querySelectorAll(".nav-page")) {
		link.remove();
	}
	overview.hidden = pageList.length > 0;
	pageList.forEach((page, index) => {
		const path = pagePath(index);
		nav.insertBefore(h("a", { href: path, "data-route": path, class: "nav-item nav-page" },
			icon(icons[page.icon] || icons.grid), h("span", null, label(page.label, page.id))), overview);
	});
}

/// The route of `path`: a static one, a control page, or none.
function resolve(path) {
	const pageRoute = (page) => ({ render: (content, context) => renderPage(content, context, page), text: label(page.label, page.id) });
	if (path === "/" && pageList.length) {
		return pageRoute(pageList[0]);
	}
	if (path.startsWith("/page/")) {
		let id = "";
		try {
			id = decodeURIComponent(path.slice(6));
		} catch (e) {
			return null;
		}
		const index = pageList.findIndex((page) => page.id === id);
		return index > 0 ? pageRoute(pageList[index]) : null;
	}
	return routes[path] || null;
}

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
	const session = ctx.session;
	if (session && session.authenticated && !pagesLoaded) {
		refreshPages().then(render);
		return;
	}
	buildNav();
	let path = location.pathname;
	if (!resolve(path)) {
		path = "/";
		history.replaceState(null, "", "/");
	}
	if (session && !session.onboarded && path !== "/onboarding") {
		return navigate("/onboarding", true);
	}
	if (session && session.onboarded && !session.authenticated && path !== "/login") {
		return navigate("/login", true);
	}
	if (session && session.authenticated && (path === "/login" || path === "/onboarding")) {
		return navigate("/", true);
	}
	const route = resolve(path);
	app.classList.toggle("bare", !!route.bare);
	for (const link of document.querySelectorAll("[data-route]")) {
		link.classList.toggle("active", link.dataset.route === path);
	}
	title.textContent = ctx.deviceName || t("app.title");
	const routeTitle = route.text || (route.title ? t(route.title) : "");
	document.title = routeTitle ? `${routeTitle} – ${ctx.deviceName || t("app.title")}` : t("app.title");
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
	ctx.live.addEventListener("status", async (e) => {
		const image = e.detail.image;
		const key = image && image.loaded ? `${image.project}/${image.revision}` : "";
		if (pagesKey === null) {
			pagesKey = key;
			return;
		}
		if (key !== pagesKey) {
			// Another image runs: its pages replace the navigation; a page shown is drawn again.
			pagesKey = key;
			const before = JSON.stringify(pageList);
			await refreshPages();
			const path = location.pathname;
			if (JSON.stringify(pageList) !== before && (path === "/" || path.startsWith("/page/"))) {
				render();
			} else {
				buildNav();
			}
		}
	});
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
			pagesLoaded = false;
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
