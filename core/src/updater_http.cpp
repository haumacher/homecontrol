/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/updater_http.h"

#include <cstdio>
#include <cstring>

#include "hc/update_check.h"

namespace hc {

namespace {

char lower(char c) {
	return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equal_ignoring_case(const char* a, const char* b) {
	for (; *a != '\0' && *b != '\0'; ++a, ++b) {
		if (lower(*a) != lower(*b)) {
			return false;
		}
	}
	return *a == *b;
}

void add(UpdaterHeaders& headers, const char* name, const char* value) {
	if (headers.count < UpdaterHeaders::kMax) {
		headers.items[headers.count].name = name;
		headers.items[headers.count].value = value;
		++headers.count;
	}
}

FirmwareDecision refuse(const char* status, const char* code, const char* why) {
	FirmwareDecision decision;
	decision.status = status;
	decision.code = code;
	std::snprintf(decision.why, sizeof(decision.why), "%s", why);
	return decision;
}

} // namespace

const char* UpdaterHeaders::find(const char* name) const {
	for (size_t i = 0; i < count; ++i) {
		if (equal_ignoring_case(items[i].name, name)) {
			return items[i].value;
		}
	}
	return nullptr;
}

bool cors_origin_echoable(const char* origin) {
	if (origin == nullptr || origin[0] == '\0') {
		return false;
	}
	size_t n = 0;
	for (const char* p = origin; *p != '\0'; ++p, ++n) {
		unsigned char c = static_cast<unsigned char>(*p);
		if (n >= kMaxCorsOrigin || c <= 0x20 || c >= 0x7f) {
			return false;
		}
	}
	return true;
}

UpdaterHeaders updater_response_headers(UpdaterRoute route, const char* origin) {
	UpdaterHeaders headers;
	add(headers, "Cache-Control", "no-store");
	if (route == UpdaterRoute::status || route == UpdaterRoute::firmware) {
		add(headers, "Vary", "Origin");
		if (cors_origin_echoable(origin)) {
			add(headers, "Access-Control-Allow-Origin", origin);
		}
	}
	return headers;
}

UpdaterPreflight updater_preflight(const char* origin) {
	UpdaterPreflight answer;
	answer.status = "204 No Content";
	answer.headers = updater_response_headers(UpdaterRoute::firmware, origin);
	add(answer.headers, "Access-Control-Allow-Methods", "PUT");
	add(answer.headers, "Access-Control-Allow-Headers", "Authorization, Content-Type");
	return answer;
}

FirmwareDecision decide_firmware_request(const FirmwareRequest& request) {
	if (request.installing || request.installed) {
		return refuse("409 Conflict", "updater.busy",
			request.installed ? "the image is installed; the device restarts" : "an upload is running");
	}
	bool by_token = request.token != nullptr && request.token[0] != '\0' && request.authorization != nullptr &&
		upload_authorized(request.token, request.authorization);
	bool by_button = !by_token && request.button_window && (request.test_button_window || !request.app_valid);
	if (!by_token && !by_button) {
		return refuse("401 Unauthorized", "updater.unauthorized",
			request.app_valid
				? "missing or wrong upload token: start the upload from the application's Settings page"
				: "missing or wrong upload token: start the upload from the application's Settings page, or, as "
				  "ota_0 holds no application, hold the device's button for 3 s and send the image within 5 minutes");
	}
	if (request.content_length == 0) {
		return refuse("411 Length Required", "updater.length_required", "the image must be sent with a Content-Length");
	}
	if (request.content_length > request.partition_size) {
		FirmwareDecision decision = refuse("413 Content Too Large", "updater.too_large", "");
		std::snprintf(decision.why, sizeof(decision.why), "the image has %u bytes, the application partition %lu",
			static_cast<unsigned>(request.content_length), static_cast<unsigned long>(request.partition_size));
		return decision;
	}
	FirmwareDecision decision;
	decision.accept = true;
	decision.by_button = by_button;
	return decision;
}

} // namespace hc
