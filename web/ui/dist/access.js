/* Copyright (c) 2026 Bernhard Haumacher */
// What the UI offers for the session of GET /api/auth/session (spec/api.md "Roles", homecontrol #91):
// onboarding, the login page, the anonymous pages without sign-in (the image's pages of minimum role
// anonymous, with a "Sign in" button), or everything after the sign-in. Pure: app.js routes by it,
// the node test (web/ui/test/access.test.mjs) checks it.

/// "onboarding", "login", "anonymous" or "full"; null before the session is known.
export function accessMode(session) {
	if (!session) {
		return null;
	}
	if (!session.onboarded) {
		return "onboarding";
	}
	if (session.authenticated) {
		return "full";
	}
	return session.anonymous_pages ? "anonymous" : "login";
}

/// Whether `path` is shown in `mode`; `pageCount` is the number of control pages loaded. Without sign-in
/// only the control pages ("/" and "/page/<id>") and the login page; signed in everything but the
/// login and the onboarding.
export function routeAllowed(mode, path, pageCount) {
	switch (mode) {
		case "onboarding":
			return path === "/onboarding";
		case "login":
			return path === "/login";
		case "anonymous":
			return path === "/login" || (pageCount > 0 && (path === "/" || path.startsWith("/page/")));
		case "full":
			return path !== "/login" && path !== "/onboarding";
		default:
			return true;
	}
}

/// Where `mode` goes when `path` is not shown there.
export function homeOf(mode, pageCount) {
	switch (mode) {
		case "onboarding":
			return "/onboarding";
		case "login":
			return "/login";
		case "anonymous":
			return pageCount > 0 ? "/" : "/login";
		default:
			return "/";
	}
}

/// Whether the live connection runs in `mode` (anonymous: restricted to its pages' datapoints).
export function liveRuns(mode) {
	return mode === "full" || mode === "anonymous";
}
