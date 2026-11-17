/* Copyright (c) 2026 Bernhard Haumacher */
// What the UI offers for a session (web/ui/dist/access.js, spec/api.md "Roles", homecontrol #91), run
// by ctest as WebUi.Access: `node --test web/ui/test/access.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { accessMode, routeAllowed, homeOf, liveRuns } from "../dist/access.js";

const anonymous = { onboarded: true, authenticated: false, role: "anonymous", anonymous_pages: true };
const locked = { onboarded: true, authenticated: false, role: "anonymous", anonymous_pages: false };
const signedIn = { onboarded: true, authenticated: true, role: "administrator", anonymous_pages: true };
const fresh = { onboarded: false, authenticated: false, role: "anonymous", anonymous_pages: false };

test("the mode follows the session: onboarding, login, the anonymous pages, everything", () => {
	assert.equal(accessMode(null), null);
	assert.equal(accessMode(fresh), "onboarding");
	assert.equal(accessMode(locked), "login");
	assert.equal(accessMode(anonymous), "anonymous");
	assert.equal(accessMode(signedIn), "full");
	// A firmware before #91 reports no anonymous_pages: the login page as before.
	assert.equal(accessMode({ onboarded: true, authenticated: false }), "login");
	// Not onboarded wins over anything an image could grant.
	assert.equal(accessMode({ ...fresh, anonymous_pages: true }), "onboarding");
});

test("without sign-in only the control pages and the login page are shown", () => {
	for (const path of ["/", "/page/garden", "/login"]) {
		assert.equal(routeAllowed("anonymous", path, 2), true, path);
	}
	for (const path of ["/rules", "/schedules", "/zigbee", "/settings", "/log", "/system", "/onboarding"]) {
		assert.equal(routeAllowed("anonymous", path, 2), false, path);
	}
	// No page loaded (the image changed meanwhile): the login page.
	assert.equal(routeAllowed("anonymous", "/", 0), false);
	assert.equal(homeOf("anonymous", 0), "/login");
	assert.equal(homeOf("anonymous", 3), "/");
});

test("the login wall without anonymous pages, everything after the sign-in", () => {
	assert.equal(routeAllowed("login", "/", 0), false);
	assert.equal(routeAllowed("login", "/login", 0), true);
	assert.equal(homeOf("login", 0), "/login");
	for (const path of ["/", "/rules", "/settings", "/system", "/page/x"]) {
		assert.equal(routeAllowed("full", path, 1), true, path);
	}
	assert.equal(routeAllowed("full", "/login", 1), false);
	assert.equal(homeOf("onboarding", 0), "/onboarding");
	assert.equal(routeAllowed("onboarding", "/", 0), false);
});

test("the live connection runs signed in and for the anonymous pages", () => {
	assert.equal(liveRuns("full"), true);
	assert.equal(liveRuns("anonymous"), true);
	assert.equal(liveRuns("login"), false);
	assert.equal(liveRuns("onboarding"), false);
});
