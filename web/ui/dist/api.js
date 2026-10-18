/* Copyright (c) 2026 Bernhard Haumacher */
// The JSON API (spec/api.md). Every state-changing request carries X-Requested-With: HomeControl,
// which a cross-site form cannot set; the session cookie travels by itself (HttpOnly).

import { t, has } from "/i18n.js";

export class ApiError extends Error {
	constructor(status, code, reason, body) {
		super(reason || code || String(status));
		this.status = status;
		this.code = code;
		this.reason = reason;
		this.body = body || {};
	}
}

/// Calls the API; resolves with the parsed body, rejects with an ApiError. `body` is serialized as
/// JSON, unless `extra.raw` says it is sent as it is (the bytes of a file: a signed runtime image
/// must arrive byte for byte); `extra.headers` adds request headers.
export async function api(method, path, body, extra = {}) {
	const options = { method, headers: { ...(extra.headers || {}) }, credentials: "same-origin" };
	if (method !== "GET") {
		options.headers["X-Requested-With"] = "HomeControl";
	}
	if (body !== undefined) {
		options.headers["Content-Type"] = "application/json";
		options.body = extra.raw ? body : JSON.stringify(body);
	}
	let response;
	try {
		response = await fetch(path, options);
	} catch (e) {
		throw new ApiError(0, "error.network", null);
	}
	let data = null;
	try {
		data = await response.json();
	} catch (e) {
		data = null;
	}
	if (!response.ok) {
		const error = new ApiError(response.status, data && data.code, data && data.error, data);
		error.retryAfter = Number(response.headers.get("Retry-After")) || (data && data.retry_after_s) || 0;
		if (response.status === 401 && error.code !== "auth.wrong_password" && listeners.unauthorized) {
			listeners.unauthorized(error);
		}
		if (error.code === "auth.onboarding_required" && listeners.onboarding) {
			listeners.onboarding(error);
		}
		throw error;
	}
	return data;
}

const listeners = {};

/// Registers what happens on 401 (go to the login) and on "onboarding required".
export function onAuthProblem(unauthorized, onboarding) {
	listeners.unauthorized = unauthorized;
	listeners.onboarding = onboarding;
}

/// The text to show for an error: the translation of its code, else the device's English reason.
export function errorText(error, params) {
	if (error && error.code && has(error.code)) {
		return t(error.code, { reason: error.reason || "", ...params });
	}
	if (error && error.reason) {
		return error.reason;
	}
	if (error && error.code === "error.network") {
		return t("error.network");
	}
	return t("error.unknown", { status: (error && error.status) || "?" });
}
