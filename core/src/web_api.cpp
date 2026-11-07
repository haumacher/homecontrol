/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/web_api.h"

#include "hc/diagnostics.h"
#include "hc/dpt.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "hc/ehome_link.h"
#include "hc/image/image_service.h"
#include "hc/json_document.h"
#include "hc/knx/knx_settings.h"
#include "hc/rules/value.h"
#include "hc/live_feed.h"
#include "hc/language_packs.h"
#include "hc/mqtt.h"
#include "hc/network_settings.h"
#include "hc/signature.h"
#include "hc/tz.h"
#include "hc/update_check.h"
#include "hc/value_json.h"
#include "hc/version.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.web";

/// Room kept for the closing of a paged response.
constexpr size_t kClosingReserve = 96;

bool starts_with(const char* text, const char* prefix) {
	return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

/// Parses a datapoint id (decimal, 0..65535) at `p`; `end` points behind it.
bool parse_id(const char* p, uint16_t& id, const char*& end) {
	if (*p < '0' || *p > '9') {
		return false;
	}
	unsigned long value = 0;
	const char* q = p;
	while (*q >= '0' && *q <= '9') {
		value = value * 10 + static_cast<unsigned long>(*q - '0');
		if (value > 65535) {
			return false;
		}
		++q;
	}
	if (q - p > 1 && *p == '0') {
		return false;
	}
	id = static_cast<uint16_t>(value);
	end = q;
	return true;
}

const char* method_name(HttpMethod method) {
	switch (method) {
	case HttpMethod::get: return "GET";
	case HttpMethod::post: return "POST";
	case HttpMethod::put: return "PUT";
	case HttpMethod::del: return "DELETE";
	case HttpMethod::other: return "?";
	}
	return "?";
}

/// The UI-editable settings and the ones shown read-only.
struct SettingInfo {
	const char* key;
	bool editable;
	bool secret = false; ///< Write-only: the listing says whether it is stored, never the value.
};

constexpr SettingInfo kShownSettings[] = {
	{setting::kTimeZone, true},
	{setting::kNtpServer, true},
	{setting::kUpdateChannel, true},
	{setting::kUiLang, true},
	{setting::kWifiPowerSave, true},
	{setting::kMqttUser, true},
	{setting::kMqttPassword, true, true},
	{knx::setting::kGateway, true},
	{knx::setting::kNat, true},
	{setting::kLedQuiet, true},
	{setting::kWifiSsid, false},
	// The E-Home connection (doc/ehome.md): set by the enrollment, erased by "Disconnect"; the
	// owner's switch. ehome.secret and ehome.socket are never listed.
	{setting::kEhomeUrl, false},
	{setting::kEhomeId, false},
	{setting::kEhomeName, false},
	{setting::kEhomeEnabled, true},
};

const SettingInfo* find_setting(const char* key) {
	for (const SettingInfo& info : kShownSettings) {
		if (std::strcmp(info.key, key) == 0) {
			return &info;
		}
	}
	return nullptr;
}

/// Percent-decodes `length` bytes of a path segment into `out` (NUL-terminated); false if it is
/// empty, malformed, contains a NUL or does not fit.
bool url_decode(const char* text, size_t length, char* out, size_t size) {
	size_t n = 0;
	for (size_t i = 0; i < length; ++i) {
		char c = text[i];
		if (c == '%') {
			auto hex = [](char h) -> int {
				return h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
			};
			if (i + 2 >= length) {
				return false;
			}
			int hi = hex(text[i + 1]);
			int lo = hex(text[i + 2]);
			if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) {
				return false;
			}
			c = static_cast<char>(hi * 16 + lo);
			i += 2;
		}
		if (n + 1 >= size) {
			return false;
		}
		out[n++] = c;
	}
	out[n] = '\0';
	return n > 0;
}

} // namespace

bool query_int(const char* query, const char* name, int64_t& out) {
	if (query == nullptr) {
		return false;
	}
	size_t n = std::strlen(name);
	const char* p = query;
	while (*p != '\0') {
		if (std::strncmp(p, name, n) == 0 && p[n] == '=') {
			const char* v = p + n + 1;
			if (!((*v >= '0' && *v <= '9') || (*v == '-' && v[1] >= '0' && v[1] <= '9'))) {
				return false;
			}
			char* end = nullptr;
			long long value = std::strtoll(v, &end, 10);
			if (*end != '\0' && *end != '&') {
				return false;
			}
			out = static_cast<int64_t>(value);
			return true;
		}
		const char* amp = std::strchr(p, '&');
		if (amp == nullptr) {
			break;
		}
		p = amp + 1;
	}
	return false;
}

bool query_uint(const char* query, const char* name, uint32_t& out) {
	if (query == nullptr) {
		return false;
	}
	size_t n = std::strlen(name);
	const char* p = query;
	while (*p != '\0') {
		if (std::strncmp(p, name, n) == 0 && p[n] == '=') {
			const char* v = p + n + 1;
			if (*v < '0' || *v > '9') {
				return false;
			}
			char* end = nullptr;
			unsigned long value = std::strtoul(v, &end, 10);
			if ((*end != '\0' && *end != '&') || value > 0xFFFFFFFFul) {
				return false;
			}
			out = static_cast<uint32_t>(value);
			return true;
		}
		const char* amp = std::strchr(p, '&');
		if (amp == nullptr) {
			break;
		}
		p = amp + 1;
	}
	return false;
}

WebApi::WebApi(Controller& controller, DatapointModel& model, Auth& auth, WebHost& host)
	: controller_(controller), model_(model), auth_(auth), host_(host) {}

void WebApi::refuse(WebResponse& response, int status, const char* code, const char* text) {
	response.status = status;
	response.content_type = "application/json";
	JsonWriter json(response.body, response.capacity);
	json.begin_object().member("error", text).member("code", code).end_object();
	response.length = json.overflowed() ? 0 : json.length();
}

void WebApi::finish(WebResponse& response, JsonWriter& json) {
	if (json.overflowed()) {
		Log::error(kTag, "response larger than the %u-byte buffer: refused", static_cast<unsigned>(response.capacity));
		refuse(response, 500, "api.response_too_large", "the response does not fit the server's buffer");
		return;
	}
	response.length = json.length();
}

void WebApi::set_session_cookie(WebResponse& response, const char* session, bool remember) {
	if (session == nullptr) {
		std::snprintf(response.set_cookie, sizeof(response.set_cookie),
			"%s=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict", Auth::kCookieName);
	} else if (remember) {
		std::snprintf(response.set_cookie, sizeof(response.set_cookie),
			"%s=%s; Path=/; Max-Age=%u; HttpOnly; SameSite=Strict", Auth::kCookieName, session,
			static_cast<unsigned>(Auth::kRememberedCookieAgeS));
	} else {
		std::snprintf(response.set_cookie, sizeof(response.set_cookie), "%s=%s; Path=/; HttpOnly; SameSite=Strict",
			Auth::kCookieName, session);
	}
}

bool WebApi::ehome_forbidden(const WebRequest& request, const char** reason) {
	const char* path = request.path != nullptr ? request.path : "";
	const char* why = nullptr;
	if (starts_with(path, "/api/auth/")) {
		why = "the E-Home connection does not log in, onboard or change the password";
	} else if (std::strcmp(path, "/api/system/wifi") == 0) {
		why = "the WiFi credentials are changed on site only";
	} else if (std::strcmp(path, "/api/system/factory-reset") == 0) {
		why = "a factory reset is made on site only";
	} else if (std::strcmp(path, "/api/system/update/upload") == 0) {
		why = "firmware never travels over the E-Home connection (the release server or the browser upload)";
	} else if (std::strcmp(path, "/api/tokens") == 0 || starts_with(path, "/api/tokens/")) {
		why = "API tokens are managed on site only";
	} else if (std::strcmp(path, "/api/ehome") == 0 || starts_with(path, "/api/ehome/")) {
		why = "the enrollment is managed on site only";
	} else if (request.method != HttpMethod::get && (std::strcmp(path, "/api/settings/update.channel") == 0 ||
			std::strcmp(path, "/api/settings/update.url") == 0)) {
		why = "the firmware source (update.channel) is chosen on site only";
	} else if (request.method != HttpMethod::get && starts_with(path, "/api/settings/ehome.")) {
		why = "the E-Home settings are the owner's, on site";
	}
	if (why != nullptr && reason != nullptr) {
		*reason = why;
	}
	return why != nullptr;
}

uint8_t WebApi::ehome_signature() const {
	return ehome_ != nullptr ? static_cast<uint8_t>(static_cast<uint8_t>(ehome_->state()) + 1) : 0;
}

bool WebApi::poll_deferred(WebResponse& response) {
	if (response.deferred_kind == kDeferredImageStore) {
		if (image_ == nullptr) {
			refuse(response, 500, "api.not_deferred", "no answer is pending");
			response.deferred = false;
			return true;
		}
		JsonWriter json(response.body, response.capacity);
		json.begin_object();
		switch (image_->store_answer(response.deferred_token, json)) {
			case image::ImageService::StoreAnswer::pending:
				return false;
			case image::ImageService::StoreAnswer::superseded:
				refuse(response, 409, "image.superseded",
					"a later image was deployed before this answer was fetched: GET /api/status and GET /api/image "
					"tell what runs and what is stored");
				break;
			case image::ImageService::StoreAnswer::ready:
				json.end_object();
				response.status = 200;
				finish(response, json);
				break;
		}
		response.deferred = false;
		return true;
	}
	if (ehome_ == nullptr) {
		refuse(response, 500, "api.not_deferred", "no answer is pending");
		return true;
	}
	return ehome_->poll_enroll(response);
}

void WebApi::handle(const WebRequest& request, WebResponse& response) {
	response.status = 200;
	response.length = 0;
	response.needs_hash = false;
	response.deferred = false;
	response.deferred_kind = 0;
	response.deferred_token = 0;
	const char* path = request.path != nullptr ? request.path : "";
	bool state_changing = request.method != HttpMethod::get;
	bool csrf_ok = request.requested_with != nullptr && std::strcmp(request.requested_with, kCsrfHeaderValue) == 0;

	// ---- The E-Home principal: a token's rights minus what is done on site only ----
	if (request.ehome) {
		const char* reason = nullptr;
		if (ehome_forbidden(request, &reason)) {
			Log::warn(kTag, "%s %s over the E-Home connection refused: %s", method_name(request.method), path, reason);
			refuse(response, 403, "ehome.forbidden", reason);
			return;
		}
	}

	// ---- Open endpoints (all of /api/auth/ but "log out everywhere") ----
	bool clear_sessions = std::strcmp(path, "/api/auth/sessions/clear") == 0;
	if (starts_with(path, "/api/auth/") && !clear_sessions) {
		const char* what = path + 10;
		bool known = std::strcmp(what, "session") == 0 || std::strcmp(what, "onboard") == 0 ||
			std::strcmp(what, "login") == 0 || std::strcmp(what, "logout") == 0;
		if (!known) {
			refuse(response, 404, "api.not_found", "no such API endpoint");
			return;
		}
		bool is_get = std::strcmp(what, "session") == 0;
		if (is_get != (request.method == HttpMethod::get)) {
			refuse(response, 405, "api.method_not_allowed", is_get ? "use GET" : "use POST");
			return;
		}
		if (state_changing && !csrf_ok) {
			refuse(response, 403, "auth.csrf", "state-changing requests need the header X-Requested-With: HomeControl");
			return;
		}
		if (is_get) {
			auth_session(request, response);
		} else if (std::strcmp(what, "onboard") == 0) {
			auth_onboard(request, response);
		} else if (std::strcmp(what, "login") == 0) {
			auth_login(request, response);
		} else {
			auth_logout(request, response);
		}
		return;
	}

	if (!starts_with(path, "/api/")) {
		refuse(response, 404, "api.not_found", "no such API endpoint");
		return;
	}

	// ---- Everything else needs a principal ----
	if (!auth_.has_password()) {
		refuse(response, 403, auth_status_code(AuthStatus::no_password), auth_status_text(AuthStatus::no_password));
		return;
	}
	bool by_token = false;
	if (request.ehome) {
		by_token = true; // EhomeLink authenticated the socket (spec/api.md "E-Home connection")
	} else if (request.authorization != nullptr) {
		if (auth_.token_from_authorization(request.authorization) < 0) {
			Log::warn(kTag, "%s %s from %s: unknown or revoked token", method_name(request.method), path, request.source);
			refuse(response, 401, "auth.token_invalid", "the bearer token is unknown or revoked");
			return;
		}
		by_token = true;
	} else if (!auth_.session_from_cookie(request.cookie).valid()) {
		refuse(response, 401, "auth.required", "log in first (session expired or missing)");
		return;
	}
	if (state_changing && !by_token && !csrf_ok) {
		refuse(response, 403, "auth.csrf", "state-changing requests need the header X-Requested-With: HomeControl");
		return;
	}

	if (clear_sessions) {
		if (request.method != HttpMethod::post) {
			refuse(response, 405, "api.method_not_allowed", "use POST");
			return;
		}
		auth_sessions_clear(request, response);
		return;
	}
	if (std::strcmp(path, "/api/status") == 0) {
		if (request.method != HttpMethod::get) {
			refuse(response, 405, "api.method_not_allowed", "use GET");
			return;
		}
		status(response);
		return;
	}
	if (std::strcmp(path, "/api/log") == 0) {
		if (request.method != HttpMethod::get) {
			refuse(response, 405, "api.method_not_allowed", "use GET");
			return;
		}
		log(request, response);
		return;
	}
	if (std::strcmp(path, "/api/settings") == 0) {
		if (request.method != HttpMethod::get) {
			refuse(response, 405, "api.method_not_allowed", "use GET");
			return;
		}
		settings(response);
		return;
	}
	if (starts_with(path, "/api/settings/")) {
		if (request.method != HttpMethod::put) {
			refuse(response, 405, "api.method_not_allowed", "use PUT");
			return;
		}
		put_setting(path + 14, request, response);
		return;
	}
	if (std::strcmp(path, "/api/coredump") == 0) {
		if (request.method == HttpMethod::del) {
			clear_coredump(response);
			return;
		}
		if (request.method != HttpMethod::get) {
			refuse(response, 405, "api.method_not_allowed", "use GET or DELETE");
			return;
		}
		coredump(response);
		return;
	}
	// ---- The System page (web_api_diagnostics.cpp) ----
	if (std::strcmp(path, "/api/system/diagnostics") == 0) {
		diagnostics(request, response);
		return;
	}
	// ---- The console's commands (web_api_system.cpp) ----
	if (starts_with(path, "/api/system/")) {
		system(path + 12, request, response);
		return;
	}
	if (std::strcmp(path, "/api/tokens") == 0 || starts_with(path, "/api/tokens/")) {
		tokens(path + 11, request, response);
		return;
	}
	if (std::strcmp(path, "/api/zigbee") == 0 || starts_with(path, "/api/zigbee/")) {
		zigbee(path + 11, request, response);
		return;
	}
	if (starts_with(path, "/api/external/")) {
		external_fetch(path + 14, request, response);
		return;
	}
	if (std::strcmp(path, "/api/lang/sync") == 0) {
		lang_sync(request, response);
		return;
	}
	if (std::strcmp(path, "/api/mqtt/discovery") == 0) {
		mqtt_discovery(request, response);
		return;
	}
	if (std::strcmp(path, "/api/knx/search") == 0) {
		knx_search(request, response);
		return;
	}
	if (std::strcmp(path, "/api/ehome") == 0 || starts_with(path, "/api/ehome/")) { // ehome_link.cpp
		if (ehome_ == nullptr) {
			refuse(response, 501, "ehome.unavailable", "this build has no E-Home connection");
			return;
		}
		ehome_->handle_route(path + 10, request, response);
		return;
	}
	if (std::strcmp(path, "/api/monitor") == 0) { // the monitor stream (monitor_feed.cpp)
		handle_monitor_request(feed_ != nullptr ? &feed_->monitor() : nullptr, request, response);
		return;
	}
	if (std::strcmp(path, "/api/image") == 0) {
		image(request, response);
		return;
	}
	if (std::strcmp(path, "/api/datapoints") == 0) {
		if (request.method != HttpMethod::get) {
			refuse(response, 405, "api.method_not_allowed", "use GET");
			return;
		}
		datapoints(request, response);
		return;
	}
	if (starts_with(path, "/api/datapoints/")) {
		uint16_t id = 0;
		const char* end = nullptr;
		if (!parse_id(path + 16, id, end)) {
			refuse(response, 404, "dp.unknown_id", "no datapoint with this id");
			return;
		}
		if (*end == '\0') {
			if (request.method != HttpMethod::get) {
				refuse(response, 405, "api.method_not_allowed", "use GET");
				return;
			}
			datapoint(id, response);
			return;
		}
		if (std::strcmp(end, "/command") == 0) {
			if (request.method != HttpMethod::post) {
				refuse(response, 405, "api.method_not_allowed", "use POST");
				return;
			}
			command(id, request, response);
			return;
		}
	}
	// ---- What the dashboard reads of the image ----
	bool model_path = std::strcmp(path, "/api/model") == 0;
	bool pages_path = std::strcmp(path, "/api/pages") == 0;
	bool history_path = std::strcmp(path, "/api/history") == 0;
	bool rules_path = std::strcmp(path, "/api/rules") == 0 || starts_with(path, "/api/rules/");
	bool schedules_path = std::strcmp(path, "/api/schedules") == 0 || starts_with(path, "/api/schedules/");
	bool blocks_path = std::strcmp(path, "/api/blocks") == 0 || starts_with(path, "/api/blocks/");
	if (!model_path && !pages_path && !history_path && !rules_path && !schedules_path && !blocks_path) {
		refuse(response, 404, "api.not_found", "no such API endpoint");
		return;
	}
	if (image_ == nullptr) {
		refuse(response, 404, "image.unavailable", "this build has no runtime image service");
		return;
	}
	if (model_path || pages_path || history_path || std::strcmp(path, "/api/rules") == 0 ||
		std::strcmp(path, "/api/schedules") == 0) {
		if (request.method != HttpMethod::get) {
			refuse(response, 405, "api.method_not_allowed", "use GET");
			return;
		}
		if (model_path) {
			ui_model(request, response);
		} else if (pages_path) {
			ui_pages(request, response);
		} else if (history_path) {
			history(request, response);
		} else if (rules_path) {
			rules(request, response);
		} else {
			schedules(request, response);
		}
		return;
	}
	if (blocks_path) {
		// GET /api/blocks, GET /api/blocks/types, PUT /api/blocks/<id>
		bool list = std::strcmp(path, "/api/blocks") == 0;
		bool types = std::strcmp(path, "/api/blocks/types") == 0;
		if (list || types) {
			if (request.method != HttpMethod::get) {
				refuse(response, 405, "api.method_not_allowed", "use GET");
				return;
			}
			if (list) {
				blocks_list(request, response);
			} else {
				block_types(request, response);
			}
			return;
		}
		const char* rest = path + 12;
		char id[48];
		if (std::strchr(rest, '/') != nullptr || !url_decode(rest, std::strlen(rest), id, sizeof(id))) {
			refuse(response, 404, "api.not_found", "no such API endpoint (PUT /api/blocks/<id>)");
			return;
		}
		if (request.method != HttpMethod::put) {
			refuse(response, 405, "api.method_not_allowed", "use PUT");
			return;
		}
		put_block(id, request, response);
		return;
	}
	char first[48];
	char second[48];
	if (rules_path) {
		// /api/rules/<id>/parameters/<parameter>
		const char* rest = path + 11;
		const char* slash = std::strchr(rest, '/');
		const char* tail = slash != nullptr ? slash : rest + std::strlen(rest);
		bool shaped = slash != nullptr && starts_with(slash, "/parameters/") && slash[12] != '\0' &&
			std::strchr(slash + 12, '/') == nullptr;
		if (!shaped || !url_decode(rest, static_cast<size_t>(tail - rest), first, sizeof(first)) ||
			!url_decode(slash + 12, std::strlen(slash + 12), second, sizeof(second))) {
			refuse(response, 404, "api.not_found", "no such API endpoint (PUT /api/rules/<rule>/parameters/<parameter>)");
			return;
		}
		if (request.method != HttpMethod::put) {
			refuse(response, 405, "api.method_not_allowed", "use PUT");
			return;
		}
		rule_parameter(first, second, request, response);
		return;
	}
	// /api/schedules/<id> (PUT) and /api/schedules/<id>/override (DELETE)
	const char* rest = path + 15;
	const char* slash = std::strchr(rest, '/');
	bool override_path = slash != nullptr && std::strcmp(slash, "/override") == 0;
	size_t id_length = slash != nullptr ? static_cast<size_t>(slash - rest) : std::strlen(rest);
	if ((slash != nullptr && !override_path) || !url_decode(rest, id_length, first, sizeof(first))) {
		refuse(response, 404, "api.not_found",
			"no such API endpoint (PUT /api/schedules/<id>, DELETE /api/schedules/<id>/override)");
		return;
	}
	if (override_path) {
		if (request.method != HttpMethod::del) {
			refuse(response, 405, "api.method_not_allowed", "use DELETE");
			return;
		}
		reset_schedule(first, response);
		return;
	}
	if (request.method != HttpMethod::put) {
		refuse(response, 405, "api.method_not_allowed", "use PUT");
		return;
	}
	put_schedule(first, request, response);
}

// ---- Authentication ---------------------------------------------------------------------------

void WebApi::auth_session(const WebRequest& request, WebResponse& response) {
	const char* via = nullptr;
	if (request.authorization != nullptr && auth_.token_from_authorization(request.authorization) >= 0) {
		via = "token";
	} else if (auth_.session_from_cookie(request.cookie).valid()) {
		via = "session";
	}
	JsonWriter json(response.body, response.capacity);
	json.begin_object()
		.member("onboarded", auth_.has_password())
		.member("authenticated", via != nullptr);
	json.key("via");
	if (via != nullptr) {
		json.string(via);
	} else {
		json.null();
	}
	json.member("device", host_.device_name())
		.member("version", kVersion)
		.member("build", build_id())
		.member("ws_port", static_cast<uint32_t>(host_.websocket_port()))
		.member("min_password", static_cast<uint32_t>(Auth::kMinPassword));
	char lang[setting::kMaxUiLangLength + 1];
	read_ui_lang(controller_.settings(), lang, kTag);
	json.member("lang", lang).end_object();
	finish(response, json);
}

bool WebApi::read_password(const WebRequest& request, WebResponse& response) {
	JsonSpan body;
	JsonSpan member;
	const char* error = nullptr;
	if (!json_parse(request.body, request.body_length, body, &error) || body.kind != JsonSpan::Kind::object) {
		refuse(response, 400, "api.bad_json", error != nullptr ? error : "the body must be a JSON object");
		return false;
	}
	size_t length = 0;
	if (!json_member(body, "password", member) || member.kind != JsonSpan::Kind::string) {
		refuse(response, 400, "auth.password_missing", "the body needs {\"password\": \"...\"}");
		return false;
	}
	if (!json_string(member, response.password, sizeof(response.password), &length) ||
		!Auth::password_valid(response.password, length, &error)) {
		std::memset(response.password, 0, sizeof(response.password));
		refuse(response, 400, auth_status_code(AuthStatus::password_invalid), auth_status_text(AuthStatus::password_invalid));
		return false;
	}
	response.password_length = length;
	return true;
}

bool WebApi::read_remember(const WebRequest& request, WebResponse& response, bool& remember) {
	remember = false;
	JsonSpan body;
	JsonSpan member;
	if (!json_parse(request.body, request.body_length, body, nullptr) || body.kind != JsonSpan::Kind::object ||
		!json_member(body, "remember", member) || member.kind == JsonSpan::Kind::null) {
		return true; // a bad body was refused by read_password() in the first pass
	}
	if (!json_bool(member, remember)) {
		refuse(response, 400, "auth.remember_invalid", "\"remember\" must be true or false");
		return false;
	}
	return true;
}

void WebApi::auth_onboard(const WebRequest& request, WebResponse& response) {
	if (request.hash == nullptr) {
		if (auth_.has_password()) {
			refuse(response, 409, auth_status_code(AuthStatus::password_set), auth_status_text(AuthStatus::password_set));
			return;
		}
		bool remember = false;
		if (!read_password(request, response)) {
			return;
		}
		if (!read_remember(request, response, remember)) {
			std::memset(response.password, 0, sizeof(response.password));
			return;
		}
		AuthStatus status = auth_.begin_onboarding(response.hash_params);
		if (status != AuthStatus::ok) {
			std::memset(response.password, 0, sizeof(response.password));
			refuse(response, status == AuthStatus::password_set ? 409 : 500, auth_status_code(status),
				auth_status_text(status));
			return;
		}
		response.needs_hash = true;
		return;
	}
	char session[Auth::kSessionText];
	bool remember = false;
	if (!read_remember(request, response, remember)) {
		return;
	}
	AuthStatus status =
		auth_.finish_onboarding(request.hash->params, request.hash->key, request.source, session, remember);
	if (status != AuthStatus::ok) {
		refuse(response, status == AuthStatus::password_set ? 409 : 500, auth_status_code(status), auth_status_text(status));
		return;
	}
	set_session_cookie(response, session, remember);
	std::memset(session, 0, sizeof(session));
	JsonWriter json(response.body, response.capacity);
	json.begin_object().member("ok", true).end_object();
	finish(response, json);
}

void WebApi::auth_login(const WebRequest& request, WebResponse& response) {
	uint32_t retry_after = 0;
	if (request.hash == nullptr) {
		bool remember = false;
		if (!read_password(request, response)) {
			return;
		}
		if (!read_remember(request, response, remember)) {
			std::memset(response.password, 0, sizeof(response.password));
			return;
		}
		AuthStatus status = auth_.begin_login(request.source, response.hash_params, retry_after);
		if (status != AuthStatus::ok) {
			std::memset(response.password, 0, sizeof(response.password));
			response.retry_after_s = retry_after;
			refuse(response, status == AuthStatus::rate_limited ? 429 : 409, auth_status_code(status),
				auth_status_text(status));
			return;
		}
		response.needs_hash = true;
		return;
	}
	char session[Auth::kSessionText];
	uint32_t failures = 0;
	bool remember = false;
	if (!read_remember(request, response, remember)) {
		return;
	}
	AuthStatus status = auth_.finish_login(request.source, request.hash->params, request.hash->key, session,
		retry_after, failures, remember);
	if (status == AuthStatus::ok) {
		set_session_cookie(response, session, remember);
		std::memset(session, 0, sizeof(session));
		JsonWriter json(response.body, response.capacity);
		json.begin_object().member("ok", true).end_object();
		finish(response, json);
		return;
	}
	int code = 500;
	switch (status) {
	case AuthStatus::wrong_password: code = 401; break;
	case AuthStatus::rate_limited: code = 429; break;
	case AuthStatus::no_password:
	case AuthStatus::password_changed: code = 409; break;
	default: break;
	}
	response.status = code;
	response.retry_after_s = retry_after;
	JsonWriter json(response.body, response.capacity);
	json.begin_object()
		.member("error", auth_status_text(status))
		.member("code", auth_status_code(status));
	if (status == AuthStatus::wrong_password) {
		json.member("failures", failures);
		json.member("free_failures", Auth::kFreeFailures);
	}
	if (retry_after > 0) {
		json.member("retry_after_s", retry_after);
	}
	json.end_object();
	response.length = json.overflowed() ? 0 : json.length();
}

void WebApi::auth_logout(const WebRequest& request, WebResponse& response) {
	SessionRef ref = auth_.session_from_cookie(request.cookie);
	AuthStatus status = AuthStatus::ok;
	if (ref.valid()) {
		status = auth_.end_session(ref);
		Log::info(kTag, "logout from %s", request.source);
	}
	set_session_cookie(response, nullptr);
	if (status != AuthStatus::ok) {
		// Ended until the restart; the stored record could not be erased (logged by Auth).
		refuse(response, 500, auth_status_code(status), auth_status_text(status));
		return;
	}
	JsonWriter json(response.body, response.capacity);
	json.begin_object().member("ok", true).end_object();
	finish(response, json);
}

void WebApi::auth_sessions_clear(const WebRequest& request, WebResponse& response) {
	size_t ended = 0;
	AuthStatus status = auth_.clear_sessions(&ended);
	Log::warn(kTag, "log out everywhere from %s: %u session(s) ended", request.source, static_cast<unsigned>(ended));
	set_session_cookie(response, nullptr); // the caller's own session ended too
	if (status != AuthStatus::ok) {
		refuse(response, 500, auth_status_code(status), auth_status_text(status));
		return;
	}
	JsonWriter json(response.body, response.capacity);
	json.begin_object().member("ok", true).member("ended", static_cast<uint32_t>(ended)).end_object();
	finish(response, json);
}

// ---- Status -----------------------------------------------------------------------------------

void WebApi::write_status(JsonWriter& json, bool full) {
	Controller& ctl = controller_;
	const Connectivity& net = ctl.connectivity();
	json.key("device").begin_object()
		.member("name", host_.device_name())
		.member("version", kVersion)
		.member("build", build_id())
		.member("ui", host_.ui_version())
		.member("schema", image::schema_version_text())
		.end_object();
	json.member("uptime_s", ctl.uptime_ms() / 1000);
	write_boot_status(json, ctl.diagnostics());
	char mdns[80];
	std::snprintf(mdns, sizeof(mdns), "%s.local", host_.device_name());
	json.key("network").begin_object()
		.member("state", link_state_name(net.state()))
		.member("ip", net.is_up() ? net.ip() : "")
		.member("mode", network_mode_name(net.mode()))
		.member("pairing", net.pairing_active())
		.member("mdns", mdns)
		.member("mdns_announced", host_.mdns_announced())
		.end_object();
	char local[64];
	bool synchronized = ctl.format_wall_time(local, sizeof(local));
	json.key("time").begin_object()
		.member("synchronized", synchronized)
		.member("local", local)
		.member("unix", ctl.clock().wall_time().seconds)
		.member("tz", ctl.time_zone())
		.member("ntp_server", host_.ntp_server())
		.end_object();
	MemoryInfo memory = ctl.system().memory();
	json.key("memory").begin_object()
		.member("free", memory.free_bytes)
		.member("min_free", memory.min_free_bytes)
		.end_object();
	const Improv& improv = ctl.improv();
	json.key("improv").begin_object().member("active", improv.active());
	if (improv.active()) {
		uint64_t now = ctl.clock().monotonic_ms();
		uint64_t ends = improv.ends_at();
		json.member("state", improv::state_name(improv.state()));
		json.member("ends_in_s", ends > now ? (ends - now) / 1000 : 0);
	}
	json.end_object();
	json.key("update").begin_object();
	host_.write_update_state(json);
	json.end_object();
	json.member("led", led_status_name(ctl.led_status()));
	json.key("web").begin_object()
		.member("sessions", static_cast<uint32_t>(auth_.session_count()))
		.member("remembered", static_cast<uint32_t>(auth_.remembered_count()))
		.member("tokens", static_cast<uint32_t>(auth_.token_count()))
		.member("ws_clients", static_cast<uint32_t>(feed_ != nullptr ? feed_->client_count() : 0))
		.end_object();
	json.member("datapoints", static_cast<uint32_t>(model_.size()));
	if (image_ != nullptr) {
		json.key("image").begin_object();
		image_->write_status(json, full);
		json.end_object();
	}
	if (ehome_ != nullptr) {
		json.key("ehome");
		ehome_->write_status(json);
	}
}

void WebApi::status(WebResponse& response) {
	JsonWriter json(response.body, response.capacity);
	json.begin_object();
	write_status(json, true);
	write_status_details(json);
	json.end_object();
	finish(response, json);
}

// ---- Datapoints -------------------------------------------------------------------------------

void WebApi::write_datapoint(JsonWriter& json, const Datapoint& dp) {
	json.begin_object()
		.member("id", static_cast<uint32_t>(dp.id()))
		.member("name", dp.name())
		.member("type", value_type_name(dp.type()))
		.member("flags", static_cast<uint32_t>(dp.flags()))
		.member("writable", dp.writable());
	json.key("technology");
	if (dp.technology() != nullptr) {
		json.string(dp.technology()->name());
	} else {
		json.null();
	}
	json.key("value");
	write_value_json(json, dp.value());
	json.member("quality", quality_name(dp.quality()))
		.member("updated_ms", dp.updated_ms())
		.member("pending", dp.pending())
		.end_object();
}

void WebApi::datapoints(const WebRequest& request, WebResponse& response) {
	uint32_t from = 0;
	query_uint(request.query, "from", from);
	JsonWriter json(response.body, response.capacity);
	json.reserve(kClosingReserve);
	json.begin_object()
		.member("now_ms", controller_.clock().monotonic_ms())
		.member("count", static_cast<uint32_t>(model_.size()))
		.member("from", from);
	json.key("datapoints").begin_array();
	size_t id = from;
	size_t written = 0;
	for (; id < model_.size() && written < kPageSize; ++id) {
		JsonWriter::Mark mark = json.mark();
		write_datapoint(json, *model_.get(static_cast<uint16_t>(id)));
		if (json.overflowed()) {
			json.reset(mark);
			break;
		}
		++written;
	}
	if (written == 0 && id < model_.size()) {
		refuse(response, 500, "api.response_too_large", "a datapoint does not fit the server's buffer");
		return;
	}
	json.release();
	json.end_array();
	json.key("next");
	if (id < model_.size()) {
		json.number(static_cast<uint32_t>(id));
	} else {
		json.null();
	}
	json.end_object();
	finish(response, json);
}

void WebApi::datapoint(uint16_t id, WebResponse& response) {
	const Datapoint* dp = model_.get(id);
	if (dp == nullptr) {
		refuse(response, 404, "dp.unknown_id", "no datapoint with this id");
		return;
	}
	JsonWriter json(response.body, response.capacity);
	write_datapoint(json, *dp);
	finish(response, json);
}

namespace {

int command_http_status(DatapointStatus status) {
	switch (status) {
	case DatapointStatus::ok: return 200;
	case DatapointStatus::unknown_id: return 404;
	case DatapointStatus::wrong_type: return 422;
	case DatapointStatus::not_writable: return 409;
	case DatapointStatus::refused: return 409;
	case DatapointStatus::busy: return 503;
	default: return 500;
	}
}

} // namespace

const char* datapoint_status_code(DatapointStatus status) {
	switch (status) {
	case DatapointStatus::ok: return "ok";
	case DatapointStatus::unknown_id: return "dp.unknown_id";
	case DatapointStatus::wrong_type: return "dp.wrong_type";
	case DatapointStatus::not_writable: return "dp.not_writable";
	case DatapointStatus::not_owner: return "dp.not_owner";
	case DatapointStatus::refused: return "dp.refused";
	case DatapointStatus::busy: return "dp.busy";
	case DatapointStatus::invalid_model: return "dp.invalid_model";
	case DatapointStatus::out_of_memory: return "dp.out_of_memory";
	case DatapointStatus::capacity: return "dp.capacity";
	}
	return "dp.unknown";
}

const char* datapoint_command_reason(DatapointStatus status) {
	switch (status) {
	case DatapointStatus::unknown_id: return "no datapoint with this id";
	case DatapointStatus::wrong_type: return "the value does not fit the datapoint's type";
	case DatapointStatus::not_writable: return "the datapoint is not writable";
	case DatapointStatus::refused: return "the technology refused the command (see the log)";
	case DatapointStatus::busy: return "the model is busy: try again";
	default: return datapoint_status_name(status);
	}
}

void WebApi::command(uint16_t id, const WebRequest& request, WebResponse& response) {
	const Datapoint* dp = model_.get(id);
	if (dp == nullptr) {
		refuse(response, 404, "dp.unknown_id", "no datapoint with this id");
		return;
	}
	JsonSpan body;
	JsonSpan member;
	const char* error = nullptr;
	if (!json_parse(request.body, request.body_length, body, &error) || body.kind != JsonSpan::Kind::object) {
		refuse(response, 400, "api.bad_json", error != nullptr ? error : "the body must be a JSON object");
		return;
	}
	if (!json_member(body, "value", member)) {
		refuse(response, 400, "dp.value_missing", "the body needs {\"value\": ...}");
		return;
	}
	if (!dp->writable()) {
		refuse(response, 409, "dp.not_writable", "the datapoint is not writable");
		return;
	}
	std::optional<Value> value = read_value_json(member, dp->type(), &error);
	if (!value) {
		refuse(response, 422, "dp.wrong_type", error);
		return;
	}
	MonitorOrigin origin(feed_ != nullptr ? &feed_->monitor() : nullptr, MonitorEvent::CommandOrigin::api,
		request.ehome ? MonitorEvent::Via::ehome
		: request.authorization != nullptr ? MonitorEvent::Via::token : MonitorEvent::Via::session);
	DatapointStatus status = model_.command(id, *value);
	if (status != DatapointStatus::ok) {
		refuse(response, command_http_status(status), datapoint_status_code(status), datapoint_command_reason(status));
		return;
	}
	JsonWriter json(response.body, response.capacity);
	json.begin_object().member("ok", true).key("datapoint");
	write_datapoint(json, *model_.get(id));
	json.end_object();
	finish(response, json);
}

// ---- Log --------------------------------------------------------------------------------------

namespace {

struct LogWriter : LogLineVisitor {
	explicit LogWriter(JsonWriter& json_) : json(json_) {}

	bool line(const LogLine& line) override {
		JsonWriter::Mark mark = json.mark();
		json.begin_object()
			.member("seq", line.seq)
			.member("ms", line.ms)
			.member("level", log_level_letter(line.level))
			.member("tag", line.tag)
			.member("message", line.message)
			.end_object();
		if (json.overflowed()) {
			json.reset(mark);
			more = true;
			return false;
		}
		last = line.seq;
		return true;
	}

	JsonWriter& json;
	uint32_t last = 0;
	bool more = false;
};

} // namespace

void WebApi::log(const WebRequest& request, WebResponse& response) {
	uint32_t after = 0;
	query_uint(request.query, "after", after);
	// Read in place under the ring's busy flag (no copy of the ring): lines other threads log while
	// the response is written are dropped and counted, as during the console's copy.
	const LogRing& ring = controller_.log_ring();
	uint64_t now_ms = controller_.clock().monotonic_ms();
	JsonWriter json(response.body, response.capacity);
	json.reserve(kClosingReserve);
	LogWriter writer(json);
	bool held = false;
	for (int attempt = 0; attempt < 8 && !held; ++attempt) {
		LogRing::ReadLock lock(ring);
		held = lock.held();
		if (held) {
			json.begin_object()
				.member("now_ms", now_ms)
				.member("first", ring.first_seq())
				.member("last_seq", ring.last_seq())
				.member("dropped", ring.dropped());
			json.key("lines").begin_array();
			ring.read(after, writer);
		}
	}
	if (!held) {
		refuse(response, 503, "log.busy", "the log is being written: try again");
		return;
	}
	json.release();
	json.end_array();
	json.member("more", writer.more);
	json.member("next_after", writer.last != 0 ? writer.last : after);
	json.end_object();
	finish(response, json);
}

// ---- Settings ---------------------------------------------------------------------------------

void WebApi::settings(WebResponse& response) {
	JsonWriter json(response.body, response.capacity);
	json.begin_object().key("settings").begin_array();
	Settings& store = controller_.settings();
	for (const SettingInfo& info : kShownSettings) {
		char value[Settings::kMaxStringLength + 1] = {};
		SettingsStatus status = store.get_string(info.key, value, sizeof(value));
		const char* fallback = "";
		if (std::strcmp(info.key, setting::kTimeZone) == 0) {
			fallback = setting::kDefaultTimeZone;
			std::snprintf(value, sizeof(value), "%s", controller_.time_zone());
			status = SettingsStatus::ok;
		} else if (std::strcmp(info.key, setting::kNtpServer) == 0) {
			fallback = setting::kDefaultNtpServer;
		} else if (std::strcmp(info.key, setting::kUpdateChannel) == 0) {
			fallback = setting::kUpdateChannelStable;
			std::snprintf(value, sizeof(value), "%s", update_channel(store));
			status = SettingsStatus::ok;
		} else if (std::strcmp(info.key, setting::kUiLang) == 0) {
			fallback = setting::kDefaultUiLang;
			char lang[setting::kMaxUiLangLength + 1];
			read_ui_lang(store, lang, kTag);
			std::snprintf(value, sizeof(value), "%s", lang);
			status = SettingsStatus::ok;
		} else if (std::strcmp(info.key, setting::kWifiPowerSave) == 0) {
			fallback = wifi_power_save_name(setting::kDefaultWifiPowerSave);
			std::snprintf(value, sizeof(value), "%s", wifi_power_save_name(read_wifi_power_save(store, kTag)));
			status = SettingsStatus::ok;
		} else if (std::strcmp(info.key, setting::kEhomeEnabled) == 0) {
			fallback = "false";
			bool on = false;
			store.get_bool(info.key, on);
			std::snprintf(value, sizeof(value), "%s", on ? "true" : "false");
			status = SettingsStatus::ok;
		} else if (std::strcmp(info.key, knx::setting::kNat) == 0) {
			// A boolean setting, shown as the console's on/off.
			fallback = "off";
			bool nat = false;
			store.get_bool(info.key, nat);
			std::snprintf(value, sizeof(value), "%s", nat ? "on" : "off");
			status = SettingsStatus::ok;
		} else if (std::strcmp(info.key, setting::kLedQuiet) == 0) {
			// A boolean setting shown as on/off; the mode in use (an unreadable value is off).
			fallback = "off";
			std::snprintf(value, sizeof(value), "%s", controller_.led_quiet() ? "on" : "off");
			status = SettingsStatus::ok;
		}
		json.begin_object().member("key", info.key);
		json.key("value");
		if (status == SettingsStatus::ok && !info.secret) {
			json.string(value);
		} else {
			json.null();
		}
		json.member("default", fallback).member("editable", info.editable);
		if (info.secret) {
			json.member("secret", true).member("stored", status == SettingsStatus::ok);
		}
		json.end_object();
	}
	json.end_array();
	if (packs_ != nullptr) {
		json.key("i18n").begin_object();
		packs_->write_json(json);
		json.end_object();
	}
	json.end_object();
	finish(response, json);
}

bool WebApi::apply_setting(const char* key, const char* value, const char** code, const char** error) {
	const SettingInfo* info = find_setting(key);
	if (info == nullptr) {
		*code = "settings.unknown";
		*error = "no such setting";
		return false;
	}
	if (!info->editable) {
		*code = "settings.not_editable";
		*error = "this setting cannot be changed from the web UI (use the console)";
		return false;
	}
	*code = "settings.invalid";
	if (std::strcmp(key, setting::kTimeZone) == 0) {
		return controller_.set_time_zone(value, error);
	}
	if (std::strcmp(key, setting::kNtpServer) == 0) {
		if (!host_name_valid(value, error)) {
			return false;
		}
		SettingsStatus status = controller_.settings().set_string(setting::kNtpServer, value);
		if (status != SettingsStatus::ok) {
			*code = "settings.storage_error";
			*error = settings_status_name(status);
			return false;
		}
		Log::info(kTag, "NTP server set to %s", value);
		host_.ntp_server_changed();
		return true;
	}
	if (std::strcmp(key, setting::kUpdateChannel) == 0) {
		if (!update_channel_valid(value, error)) {
			return false;
		}
		if (!store_update_channel(controller_.settings(), value, error)) {
			*code = "settings.storage_error";
			return false;
		}
		Log::info(kTag, "update channel set to %s", value);
		if (packs_ != nullptr) {
			packs_->language_changed(); // the packs come from the channel's release
		}
		return true;
	}
	if (std::strcmp(key, setting::kUiLang) == 0) {
		if (!ui_lang_valid(value, error)) {
			return false;
		}
		if (!store_ui_lang(controller_.settings(), value, error)) {
			*code = "settings.storage_error";
			return false;
		}
		Log::info(kTag, "UI language set to %s", value);
		if (packs_ != nullptr) {
			packs_->language_changed();
		}
		return true;
	}
	if (std::strcmp(key, setting::kMqttUser) == 0 || std::strcmp(key, setting::kMqttPassword) == 0) {
		bool password = std::strcmp(key, setting::kMqttPassword) == 0;
		SettingsStatus status;
		if (value[0] == '\0') {
			status = controller_.settings().erase(key);
			if (status == SettingsStatus::not_found) {
				status = SettingsStatus::ok;
			}
		} else {
			if (std::strlen(value) > Mqtt::kMaxUser) {
				*error = password ? "the password is longer than 63 bytes" : "the user name is longer than 63 bytes";
				return false;
			}
			status = Settings::check_string(value);
			if (status != SettingsStatus::ok) {
				*error = settings_status_name(status);
				return false;
			}
			status = controller_.settings().set_string(key, value);
		}
		if (status != SettingsStatus::ok) {
			*code = "settings.storage_error";
			*error = settings_status_name(status);
			return false;
		}
		Log::info(kTag, "MQTT %s %s", password ? "password" : "user name", value[0] == '\0' ? "removed" : "set");
		if (mqtt_ != nullptr) {
			mqtt_->reconnect(password ? "the broker password changed" : "the broker user name changed");
		}
		return true;
	}
	if (std::strcmp(key, knx::setting::kGateway) == 0) {
		if (!knx::apply_gateway_setting(controller_.settings(), knx_, value, error)) {
			return false;
		}
		Log::info(kTag, "KNX gateway %s%s", value[0] == '\0' ? "cleared" : "set to ", value);
		return true;
	}
	if (std::strcmp(key, knx::setting::kNat) == 0) {
		bool nat = std::strcmp(value, "on") == 0 || std::strcmp(value, "true") == 0;
		if (!nat && std::strcmp(value, "off") != 0 && std::strcmp(value, "false") != 0) {
			*error = "knx.nat is on or off";
			return false;
		}
		if (!knx::apply_nat_setting(controller_.settings(), knx_, nat, error)) {
			*code = "settings.storage_error";
			return false;
		}
		Log::info(kTag, "KNX NAT mode %s", nat ? "on" : "off");
		return true;
	}
	if (std::strcmp(key, setting::kLedQuiet) == 0) {
		bool quiet = std::strcmp(value, "on") == 0 || std::strcmp(value, "true") == 0;
		if (!quiet && std::strcmp(value, "off") != 0 && std::strcmp(value, "false") != 0) {
			*error = "led.quiet is on or off";
			return false;
		}
		if (!controller_.set_led_quiet(quiet, error)) {
			*code = "settings.storage_error";
			return false;
		}
		return true;
	}
	if (std::strcmp(key, setting::kEhomeEnabled) == 0) {
		bool on = std::strcmp(value, "true") == 0 || std::strcmp(value, "on") == 0;
		if (!on && std::strcmp(value, "false") != 0 && std::strcmp(value, "off") != 0) {
			*error = "ehome.enabled is true or false";
			return false;
		}
		if (on && !ehome_enrolled(controller_.settings())) {
			*code = "ehome.not_enrolled";
			*error = "the controller is not enrolled with E-Home: connect to E-Home first";
			return false;
		}
		SettingsStatus status = controller_.settings().set_bool(setting::kEhomeEnabled, on);
		if (status != SettingsStatus::ok) {
			*code = "settings.storage_error";
			*error = settings_status_name(status);
			return false;
		}
		Log::info(kTag, "E-Home connection switched %s", on ? "on" : "off");
		if (ehome_ != nullptr) {
			ehome_->enabled_changed();
		}
		return true;
	}
	if (std::strcmp(key, setting::kWifiPowerSave) == 0) {
		if (!wifi_power_save_valid(value, error)) {
			return false;
		}
		if (!store_wifi_power_save(controller_.settings(), value, error)) {
			*code = "settings.storage_error";
			return false;
		}
		Log::info(kTag, "WiFi power save set to %s", value);
		controller_.connectivity().driver().power_save_changed();
		return true;
	}
	*code = "settings.unknown";
	*error = "no such setting";
	return false;
}

void WebApi::put_setting(const char* key, const WebRequest& request, WebResponse& response) {
	const SettingInfo* info = find_setting(key);
	if (info == nullptr) {
		refuse(response, 404, "settings.unknown", "no such setting");
		return;
	}
	if (!info->editable) {
		refuse(response, 403, "settings.not_editable", "this setting cannot be changed from the web UI (use the console)");
		return;
	}
	JsonSpan body;
	JsonSpan member;
	const char* error = nullptr;
	if (!json_parse(request.body, request.body_length, body, &error) || body.kind != JsonSpan::Kind::object) {
		refuse(response, 400, "api.bad_json", error != nullptr ? error : "the body must be a JSON object");
		return;
	}
	char value[Settings::kMaxStringLength + 1];
	if (!json_member(body, "value", member) || !json_string(member, value, sizeof(value))) {
		refuse(response, 400, "settings.value_missing", "the body needs {\"value\": \"...\"} (at most 127 bytes)");
		return;
	}
	const char* code = nullptr;
	if (!apply_setting(key, value, &code, &error)) {
		Log::warn(kTag, "setting %s refused: %s", key, error);
		refuse(response, std::strcmp(code, "settings.storage_error") == 0 ? 500 : 422, code, error);
		return;
	}
	settings(response);
}

// ---- Runtime image ----------------------------------------------------------------------------

void WebApi::set_image(image::ImageService* image) {
	image_ = image;
	if (image_ != nullptr) {
		image_->set_changed(Callback::member<WebApi, &WebApi::image_changed>(this));
	}
}

void WebApi::image_changed() {
	if (feed_ != nullptr) {
		feed_->resync();
	}
}

const char* WebApi::body_refusal(HttpMethod method, const char* path) const {
	if (image_ != nullptr && method == HttpMethod::put && path != nullptr && std::strcmp(path, "/api/image") == 0 &&
		image_->store_pending()) {
		return "the previous image is still being stored; deploy again in a moment";
	}
	return nullptr;
}

void WebApi::image_stored() {
	if (ehome_ != nullptr) {
		ehome_->deferred_ready(); // an answer over the E-Home connection goes out at once
	}
}

ImageStaging* WebApi::body_staging(HttpMethod method, const char* path) const {
	if (image_ != nullptr && method == HttpMethod::put && path != nullptr && std::strcmp(path, "/api/image") == 0) {
		return image_->staging();
	}
	return nullptr;
}

size_t WebApi::body_limit(HttpMethod method, const char* path) const {
	if (method == HttpMethod::put && path != nullptr && std::strcmp(path, "/api/image") == 0) {
		return image_ != nullptr ? image_->receivable() : image::kMaxImageBytes;
	}
	return kMaxBody;
}

void WebApi::body_too_long(HttpMethod method, const char* path, size_t bytes, char* text, size_t size) const {
	size_t limit = body_limit(method, path);
	if (method == HttpMethod::put && path != nullptr && std::strcmp(path, "/api/image") == 0) {
		if (limit < image::kMaxImageBytes) {
			std::snprintf(text, size, "the image has %u bytes; the device receives at most %u (its staging area)",
				static_cast<unsigned>(bytes), static_cast<unsigned>(limit));
		} else {
			std::snprintf(text, size, "the image has %u bytes; the device receives at most %u",
				static_cast<unsigned>(bytes), static_cast<unsigned>(limit));
		}
		return;
	}
	std::snprintf(text, size, "the request body is longer than %u bytes", static_cast<unsigned>(limit));
}

namespace {

/// Streamed documents: the stored image's token with this bit cleared, the core dump with it set.
constexpr uint32_t kCoreDumpStream = 0x80000000u;

} // namespace

bool WebApi::read_stream(uint32_t token, size_t offset, char* buffer, size_t size, size_t& length) {
	if ((token & kCoreDumpStream) != 0) {
		const Diagnostics* diagnostics = controller_.diagnostics();
		size_t total = diagnostics != nullptr ? diagnostics->coredump_size() : 0;
		if (offset >= total) {
			return false;
		}
		size_t piece = size < total - offset ? size : total - offset;
		if (!diagnostics->coredump_read(offset, buffer, piece)) {
			return false;
		}
		length = piece;
		return true;
	}
	if (image_ == nullptr) {
		return false;
	}
	uint32_t current = image_->store_token();
	return (current & ~kCoreDumpStream) == token && image_->read_stored(current, offset, buffer, size, length);
}

void WebApi::coredump(WebResponse& response) {
	const Diagnostics* diagnostics = controller_.diagnostics();
	size_t bytes = diagnostics != nullptr ? diagnostics->coredump_size() : 0;
	if (bytes == 0) {
		refuse(response, 404, "coredump.none", "no core dump is stored (doc/diagnostics.md)");
		return;
	}
	response.content_type = "application/octet-stream";
	response.stream = true;
	response.stream_length = bytes;
	response.stream_token = kCoreDumpStream;
	response.length = 0;
}

void WebApi::image(const WebRequest& request, WebResponse& response) {
	if (image_ == nullptr) {
		refuse(response, 404, "image.unavailable", "this build has no runtime image service");
		return;
	}
	if (request.method == HttpMethod::get) {
		size_t bytes = 0;
		if (!image_->stored(bytes)) {
			refuse(response, 404, "image.none", "no runtime image is stored");
			return;
		}
		response.stream = true;
		response.stream_length = bytes;
		response.stream_token = image_->store_token() & ~kCoreDumpStream;
		response.length = 0;
		return;
	}
	if (request.method == HttpMethod::del) {
		const char* error = nullptr;
		if (image_->store_pending()) {
			Log::warn(kTag, "DELETE /api/image from %s refused: the image is still being stored", request.source);
			refuse(response, 503, "image.busy", "the image is still being stored; remove it in a moment");
			response.retry_after_s = kBusyRetryS;
			return;
		}
		if (!image_->unload(&error)) {
			refuse(response, 500, "image.storage_error", error);
			return;
		}
		JsonWriter json(response.body, response.capacity);
		json.begin_object().member("ok", true).member("loaded", false).end_object();
		finish(response, json);
		return;
	}
	if (request.method != HttpMethod::put) {
		refuse(response, 405, "api.method_not_allowed", "use GET, PUT or DELETE");
		return;
	}
	if (request.body == nullptr || request.body_length == 0) {
		refuse(response, 400, "image.missing", "the body must be the runtime image (JSON)");
		return;
	}
	uint8_t signature[kEd25519SignatureSize];
	bool signed_image = request.image_signature != nullptr;
	if (signed_image && (std::strlen(request.image_signature) != 2 * kEd25519SignatureSize ||
		!parse_signature(request.image_signature, 2 * kEd25519SignatureSize, signature))) {
		Log::warn(kTag, "PUT /api/image from %s refused: the signature is not 128 hex digits", request.source);
		refuse(response, 400, "image.bad_signature",
			request.ehome ? "\"signature\" is the image's Ed25519 signature in 128 hex digits"
						  : "X-Image-Signature is the image's Ed25519 signature in 128 hex digits");
		return;
	}
	Log::info(kTag, "PUT /api/image from %s: %u bytes%s", request.source, static_cast<unsigned>(request.body_length),
		signed_image ? ", signed" : "");
	bool ok = image_->load(request.body, request.body_length, image::Source::api, true,
		signed_image ? signature : nullptr, request.body_holder, Callback::member<WebApi, &WebApi::image_stored>(this));
	if (ok && image_->store_pending()) {
		// The image runs; its store runs off the loop (homecontrol #68): the answer waits for it.
		response.deferred = true;
		response.deferred_kind = kDeferredImageStore;
		response.deferred_token = image_->store_ticket();
		response.length = 0;
		return;
	}
	JsonWriter json(response.body, response.capacity);
	json.begin_object();
	if (ok) {
		image_->write_info(json);
	} else {
		switch (image_->report().kind) {
			case image::Report::Kind::syntax: response.status = 400; break;
			case image::Report::Kind::too_large: response.status = 413; break;
			case image::Report::Kind::no_memory: response.status = 507; break;
			case image::Report::Kind::apply: response.status = 500; break;
			// 403, not 401: the UI takes a 401 for an expired login.
			case image::Report::Kind::unsigned_image: response.status = 403; break;
			case image::Report::Kind::bad_signature: response.status = 403; break;
			case image::Report::Kind::busy:
				response.status = 503;
				response.retry_after_s = kBusyRetryS;
				break;
			default: response.status = 422; break;
		}
		image_->write_report(json);
	}
	json.end_object();
	finish(response, json);
}

// ---- The dashboard's view of the image ---------------------------------------------------------

namespace {

/// A parameter value in JSON: boolean, number (integer, real, enumeration, duration in ms), string,
/// {"hour","minute","second"} for a time of day, {"year","month","day"} for a date, {"control",
/// "value"} for a controlled value; the canonical literal text (a string) for the rest; null for no
/// value.
void write_val(JsonWriter& json, const rules::Val& v) {
	if (!v.has()) {
		json.null();
		return;
	}
	switch (v.type) {
		case rules::Type::boolean: json.boolean(v.u.b); return;
		case rules::Type::integer:
		case rules::Type::duration: json.number(v.u.i); return;
		case rules::Type::real: json.number(v.u.r); return;
		case rules::Type::enumeration: json.number(v.u.e); return;
		case rules::Type::string: json.string(v.str()); return;
		case rules::Type::time_of_day:
			json.begin_object()
				.member("hour", static_cast<uint32_t>(v.u.tod.hour))
				.member("minute", static_cast<uint32_t>(v.u.tod.minute))
				.member("second", static_cast<uint32_t>(v.u.tod.second))
				.end_object();
			return;
		case rules::Type::date:
			json.begin_object()
				.member("year", static_cast<uint32_t>(v.u.date.year))
				.member("month", static_cast<uint32_t>(v.u.date.month))
				.member("day", static_cast<uint32_t>(v.u.date.day))
				.end_object();
			return;
		case rules::Type::controlled:
			json.begin_object()
				.member("control", v.u.ctl.control)
				.member("value", static_cast<uint32_t>(v.u.ctl.value))
				.end_object();
			return;
		default: {
			char text[96];
			rules::literal_text(v, text, sizeof(text));
			json.string(text);
			return;
		}
	}
}

/// {"text", "ms"} of a diagnostics event, or null.
void write_event(JsonWriter& json, const char* name, bool has, const char* text, uint64_t ms) {
	json.key(name);
	if (!has) {
		json.null();
		return;
	}
	json.begin_object().member("text", text).member("ms", ms).end_object();
}

/// A number with float precision (history samples are 32-bit), as JSON.
void write_sample(JsonWriter& json, double value) {
	char text[24];
	int n = std::snprintf(text, sizeof(text), "%.7g", value);
	json.raw(text, n > 0 ? static_cast<size_t>(n) : 0);
}

int parameter_http_status(rules::ParameterStatus status) {
	switch (status) {
		case rules::ParameterStatus::unknown_rule:
		case rules::ParameterStatus::unknown_parameter: return 404;
		case rules::ParameterStatus::not_adjustable: return 403;
		case rules::ParameterStatus::invalid_value: return 422;
		default: return 500;
	}
}

const char* parameter_code(rules::ParameterStatus status) {
	switch (status) {
		case rules::ParameterStatus::unknown_rule: return "rule.unknown";
		case rules::ParameterStatus::unknown_parameter: return "rule.unknown_parameter";
		case rules::ParameterStatus::not_adjustable: return "rule.not_adjustable";
		case rules::ParameterStatus::invalid_value: return "rule.invalid_value";
		default: return "rule.refused";
	}
}

/// Parses "3,4,17" into `ids`; false if malformed or more than `max`.
bool parse_ids(const char* query, uint16_t* ids, size_t max, size_t& count) {
	count = 0;
	const char* p = query;
	const char* v = nullptr;
	while (p != nullptr && *p != '\0') {
		if (std::strncmp(p, "ids=", 4) == 0) {
			v = p + 4;
			break;
		}
		p = std::strchr(p, '&');
		p = p != nullptr ? p + 1 : nullptr;
	}
	if (v == nullptr) {
		return false;
	}
	for (;;) {
		uint16_t id = 0;
		const char* end = nullptr;
		if (count == max || !parse_id(v, id, end)) {
			return false;
		}
		ids[count++] = id;
		if (*end == ',') {
			v = end + 1;
			continue;
		}
		return *end == '\0' || *end == '&';
	}
}

} // namespace

namespace {

void write_label(JsonWriter& json, const rules::RuleSet& set, const char* key, uint32_t text, uint32_t map);

} // namespace

void WebApi::ui_model(const WebRequest& request, WebResponse& response) {
	const image::ModelView& view = image_->view();
	JsonWriter json(response.body, response.capacity);
	if (!image_->loaded()) {
		json.begin_object().member("loaded", false).end_object();
		finish(response, json);
		return;
	}
	if (view.no_memory()) {
		refuse(response, 507, "model.no_memory", "the device had not enough memory for the UI model of this image");
		return;
	}
	uint32_t from = 0;
	query_uint(request.query, "from", from);
	const image::ImageInfo& info = image_->info();
	size_t locations = view.location_count();
	size_t functions = view.function_count();
	size_t datapoints = view.datapoint_count() < model_.size() ? view.datapoint_count() : model_.size();
	size_t total = locations + functions + datapoints;
	json.reserve(kClosingReserve);
	json.begin_object()
		.member("loaded", true)
		.member("project", info.project_id)
		.member("name", info.name)
		.member("revision", info.revision)
		.member("generatedAt", info.generated_at)
		.member("generator", info.generator)
		.member("source", image::source_name(image_->source()))
		.member("stored", image_->stored_ok())
		.member("loaded_ms", image_->loaded_ms())
		.member("now_ms", controller_.clock().monotonic_ms())
		.member("generation", view.generation())
		.member("total", static_cast<uint32_t>(total))
		.member("from", from);
	size_t index = from;
	size_t written = 0;
	bool full = false;
	// The three lists share one index space: locations, then functions, then datapoints.
	json.key("locations").begin_array();
	for (; !full && index < locations; ++index) {
		JsonWriter::Mark mark = json.mark();
		const image::ModelView::Location& l = view.location(index);
		json.begin_object().member("id", l.id).key("label").raw(l.label, std::strlen(l.label));
		if (l.kind[0] != '\0') {
			json.member("kind", l.kind);
		}
		if (l.parent >= 0) {
			json.member("parent", view.location(static_cast<size_t>(l.parent)).id);
		}
		json.end_object();
		if (json.overflowed()) {
			json.reset(mark);
			full = true;
			break;
		}
		++written;
	}
	json.end_array();
	json.key("functions").begin_array();
	for (; !full && index >= locations && index < locations + functions; ++index) {
		JsonWriter::Mark mark = json.mark();
		const image::ModelView::Function& f = view.function(index - locations);
		json.begin_object().member("id", f.id).key("label").raw(f.label, std::strlen(f.label)).member("kind", f.kind);
		if (f.room >= 0) {
			json.member("room", view.location(static_cast<size_t>(f.room)).id);
		}
		if (f.unit[0] != '\0') {
			json.member("unit", f.unit);
		}
		if (f.scene > 0) {
			json.member("scene", static_cast<uint32_t>(f.scene));
		}
		if (f.presses != nullptr) {
			json.key("presses").raw(f.presses, std::strlen(f.presses));
		}
		json.key("datapoints").begin_object();
		for (size_t r = 0; r < f.role_count; ++r) {
			json.member(f.roles[r].name, static_cast<uint32_t>(f.roles[r].datapoint));
		}
		json.end_object().end_object();
		if (json.overflowed()) {
			json.reset(mark);
			full = true;
			break;
		}
		++written;
	}
	json.end_array();
	json.key("datapoints").begin_array();
	for (; !full && index >= locations + functions && index < total; ++index) {
		JsonWriter::Mark mark = json.mark();
		size_t id = index - locations - functions;
		const image::ModelView::DatapointInfo& d = view.datapoint(id);
		const Datapoint* dp = model_.get(static_cast<uint16_t>(id));
		json.begin_object().member("id", static_cast<uint32_t>(id)).member("name", dp != nullptr ? dp->name() : "");
		if (dp != nullptr) {
			json.member("type", value_type_name(dp->type())).member("writable", dp->writable());
		}
		// A rule parameter's datapoint (spec/rules.md 8.6) takes the parameter's label and unit; the
		// enable switch the rule's name.
		const rules::Engine& engine = image_->engine();
		size_t param_rule = 0, param_index = 0;
		bool parameter = dp != nullptr && engine.parameter_of(dp->id(), param_rule, param_index);
		const rules::RuleSet& set = engine.rules();
		if (d.label != nullptr) {
			json.key("label").raw(d.label, std::strlen(d.label));
		} else if (parameter) {
			const rules::Rule& rule = set.rule(param_rule);
			const rules::Parameter& decl = set.parameter(rule.first_parameter + param_index);
			if (param_index == 0) {
				write_label(json, set, "label", rule.name, rule.name_json);
			} else {
				write_label(json, set, "label", decl.label, decl.label_json);
			}
		}
		if (d.unit != nullptr) {
			json.member("unit", d.unit);
		} else if (parameter) {
			const rules::Parameter& decl = set.parameter(set.rule(param_rule).first_parameter + param_index);
			if (decl.type == rules::Type::duration) {
				json.member("unit", "ms");
			} else if (set.str(decl.unit)[0] != '\0') {
				json.member("unit", set.str(decl.unit));
			}
		}
		json.key("technology");
		if (dp != nullptr && dp->technology() != nullptr) {
			json.string(dp->technology()->name());
		} else {
			json.null();
		}
		if (d.binding != nullptr) {
			json.key("binding").raw(d.binding, std::strlen(d.binding));
		}
		if (parameter) {
			const rules::Rule& rule = set.rule(param_rule);
			const rules::Parameter& decl = set.parameter(rule.first_parameter + param_index);
			json.key("parameter")
				.begin_object()
				.member("rule", set.str(rule.id))
				.member("parameter", set.str(decl.id))
				.end_object();
			// The parameter's range and choices, for the control of a page element (spec/api.md).
			if (decl.min != rules::kNone) {
				json.key("min");
				write_val(json, set.constant(decl.min));
			}
			if (decl.max != rules::kNone) {
				json.key("max");
				write_val(json, set.constant(decl.max));
			}
			double step = view.parameter_step(set.str(rule.id), set.str(decl.id));
			if (step > 0) {
				json.member("step", step);
			}
			if (decl.choice_count > 0) {
				json.key("choices").begin_array();
				for (size_t c = 0; c < decl.choice_count; ++c) {
					const rules::Choice& choice = set.choice(decl.first_choice + c);
					json.begin_object().key("value");
					write_val(json, set.constant(choice.value));
					write_label(json, set, "label", choice.label, choice.label_json);
					json.end_object();
				}
				json.end_array();
			}
		} else if (d.binding != nullptr) {
			// The fixed range of a scaled KNX type (5.001 percent, 5.003 angle).
			JsonSpan binding;
			JsonSpan knx;
			JsonSpan dpt;
			if (json_parse(d.binding, std::strlen(d.binding), binding) && json_member(binding, "knx", knx) &&
				json_member(knx, "dpt", dpt)) {
				char text[16];
				std::optional<Dpt> type = json_string(dpt, text, sizeof(text)) ? Dpt::parse(text) : std::nullopt;
				if (type && type->main == 5 && (type->sub == 1 || type->sub == 3)) {
					json.member("min", static_cast<uint32_t>(0)).member("max", static_cast<uint32_t>(type->sub == 1 ? 100 : 360));
				}
			}
		}
		json.member("history", dp != nullptr && dp->has_history());
		json.end_object();
		if (json.overflowed()) {
			json.reset(mark);
			full = true;
			break;
		}
		++written;
	}
	json.end_array();
	if (written == 0 && index < total) {
		refuse(response, 500, "api.response_too_large", "an entry of the model does not fit the server's buffer");
		return;
	}
	json.release();
	json.key("next");
	if (index < total) {
		json.number(static_cast<uint32_t>(index));
	} else {
		json.null();
	}
	json.end_object();
	finish(response, json);
}

void WebApi::ui_pages(const WebRequest& request, WebResponse& response) {
	const image::ModelView& view = image_->view();
	if (image_->loaded() && view.no_memory()) {
		refuse(response, 507, "model.no_memory", "the device had not enough memory for the UI model of this image");
		return;
	}
	uint32_t from = 0;
	query_uint(request.query, "from", from);
	bool loaded = image_->loaded();
	// Five lists in one index space: pages, sections, elements, series, reference lines.
	const size_t counts[5] = {loaded ? view.page_count() : 0, loaded ? view.section_count() : 0,
		loaded ? view.element_count() : 0, loaded ? view.series_count() : 0, loaded ? view.reference_count() : 0};
	static const char* const kLists[5] = {"pages", "sections", "elements", "series", "references"};
	size_t total = 0;
	for (size_t count : counts) {
		total += count;
	}
	JsonWriter json(response.body, response.capacity);
	// The closing: the lists after a full one (empty) and `next`.
	json.reserve(kClosingReserve + 64);
	json.begin_object()
		.member("loaded", loaded)
		.member("generation", view.generation())
		.member("total", static_cast<uint32_t>(total))
		.member("from", from);
	auto label = [&json](const char* text) {
		if (text != nullptr) {
			json.key("label").raw(text, std::strlen(text));
		}
	};
	auto axis = [](uint8_t a) { return a == 0 ? "left" : "right"; };
	size_t index = from;
	size_t written = 0;
	bool full = false;
	size_t base = 0;
	for (size_t list = 0; list < 5; ++list) {
		json.key(kLists[list]).begin_array();
		for (; !full && index >= base && index < base + counts[list]; ++index) {
			JsonWriter::Mark mark = json.mark();
			size_t i = index - base;
			json.begin_object();
			if (list == 0) {
				const image::ModelView::Page& p = view.page(i);
				json.member("id", p.id != nullptr ? p.id : "");
				label(p.label);
				if (p.icon != nullptr) {
					json.member("icon", p.icon);
				}
				json.member("sections", static_cast<uint32_t>(p.sections));
			} else if (list == 1) {
				const image::ModelView::Section& s = view.section(i);
				json.member("page", static_cast<uint32_t>(s.page));
				label(s.label);
				json.member("elements", static_cast<uint32_t>(s.elements));
			} else if (list == 2) {
				const image::ModelView::Element& e = view.element(i);
				json.member("section", static_cast<uint32_t>(e.section));
				label(e.label);
				if (e.kind == image::ModelView::ElementKind::function) {
					json.member("function", view.function(e.target).id);
				} else if (e.kind == image::ModelView::ElementKind::datapoint) {
					json.member("datapoint", static_cast<uint32_t>(e.target));
					if (e.read_only) {
						json.member("readOnly", true);
					}
				} else {
					const image::ModelView::Chart& c = view.chart(e.target);
					json.key("chart").begin_object();
					label(c.label);
					json.key("width");
					if (c.width == 0) {
						json.string("full");
					} else {
						json.number(static_cast<uint32_t>(c.width));
					}
					json.member("height", static_cast<uint32_t>(c.height))
						.member("range", image::chart_range_name(c.range));
					json.key("presets").begin_array();
					for (size_t r = 0; r < image::kChartRanges; ++r) {
						if ((c.presets & (1u << r)) != 0) {
							json.string(image::chart_range_name(r));
						}
					}
					json.end_array();
					json.key("axes").begin_object();
					for (int a = 0; a < 2; ++a) {
						json.key(axis(static_cast<uint8_t>(a))).begin_object().key("min");
						if ((c.fixed & (1u << (2 * a))) != 0) {
							json.number(c.min[a]);
						} else {
							json.null();
						}
						json.key("max");
						if ((c.fixed & (2u << (2 * a))) != 0) {
							json.number(c.max[a]);
						} else {
							json.null();
						}
						json.end_object();
					}
					json.end_object().end_object();
				}
			} else if (list == 3) {
				const image::ModelView::Series& r = view.series(i);
				json.member("element", static_cast<uint32_t>(r.element))
					.member("datapoint", static_cast<uint32_t>(r.datapoint));
				label(r.label);
				json.member("axis", axis(r.axis)).member("consolidation", image::consolidation_name(r.consolidation));
			} else {
				const image::ModelView::Reference& r = view.reference(i);
				json.member("element", static_cast<uint32_t>(r.element));
				if (r.constant) {
					json.member("value", r.value);
				} else {
					json.member("datapoint", static_cast<uint32_t>(r.datapoint));
				}
				label(r.label);
				json.member("axis", axis(r.axis));
			}
			json.end_object();
			if (json.overflowed()) {
				json.reset(mark);
				json.release(); // the closing: this list's end, the empty lists after it, `next`
				full = true;
				break;
			}
			++written;
		}
		json.end_array();
		base += counts[list];
	}
	if (written == 0 && index < total) {
		refuse(response, 500, "api.response_too_large", "an entry of the pages does not fit the server's buffer");
		return;
	}
	json.release();
	json.key("next");
	if (index < total) {
		json.number(static_cast<uint32_t>(index));
	} else {
		json.null();
	}
	json.end_object();
	finish(response, json);
}

void WebApi::history(const WebRequest& request, WebResponse& response) {
	uint16_t ids[kMaxHistoryIds];
	size_t count = 0;
	if (!parse_ids(request.query, ids, kMaxHistoryIds, count)) {
		refuse(response, 400, "history.bad_query", "ids=<datapoint id>[,<id>...] is missing or malformed (at most 8)");
		return;
	}
	WallTime wall = controller_.clock().wall_time();
	int64_t to = 0;
	int64_t from = 0;
	bool has_to = query_int(request.query, "to", to);
	bool has_from = query_int(request.query, "from", from);
	if ((!has_to || !has_from) && !wall.synchronized) {
		refuse(response, 409, "history.no_time", "the wall clock is not synchronized: give from and to");
		return;
	}
	if (!has_to) {
		to = wall.seconds + 1;
	}
	if (!has_from) {
		from = to - 86400;
	}
	uint32_t points = 48;
	if (std::strstr(request.query, "points=") != nullptr && !query_uint(request.query, "points", points)) {
		points = 0;
	}
	if (to <= from || points == 0 || points * count > kMaxHistoryPoints) {
		refuse(response, 400, "history.bad_query",
			"needs from < to and 1 <= points, with points times the number of ids at most 160");
		return;
	}
	History& hist = image_->history();
	int64_t width = (to - from + points - 1) / points;
	JsonWriter json(response.body, response.capacity);
	// Held back for closing what is open when the points do not fit (spec/api.md "history": a
	// series ends early with "truncated"): the array, the series and the document, and the
	// members of each series still to come.
	constexpr size_t kClosing = 32;
	constexpr size_t kSeriesHead = 96;
	json.reserve(kClosing + count * kSeriesHead);
	json.begin_object()
		.member("from", from)
		.member("to", to)
		.member("width_s", width);
	json.key("series").begin_array();
	bool full = false;
	for (size_t i = 0; i < count; ++i) {
		json.release();
		json.reserve(kClosing + (count - i - 1) * kSeriesHead);
		json.begin_object().member("id", static_cast<uint32_t>(ids[i]));
		// Writes the buckets as points: [t, value] for a single sample, [t, last, min, max, count,
		// mean] for a consolidated bucket, and on_s for a boolean.
		class Points : public HistorySink {
		public:
			Points(JsonWriter& json, bool boolean) : json_(json), boolean_(boolean) {}
			bool bucket(const HistoryBucket& b) override {
				JsonWriter::Mark mark = json_.mark();
				json_.begin_array().number(b.t);
				write_sample(json_, b.last);
				if (!b.single) {
					write_sample(json_, b.min);
					write_sample(json_, b.max);
					json_.number(b.count);
					write_sample(json_, b.mean);
					if (boolean_) {
						json_.number(static_cast<int64_t>(std::llround(b.on_s)));
					}
				}
				json_.end_array();
				if (json_.overflowed()) {
					json_.reset(mark);
					truncated = true;
					return false;
				}
				return true;
			}
			bool truncated = false;

		private:
			JsonWriter& json_;
			bool boolean_;
		};
		if (!hist.has_history(ids[i])) {
			json.member("code", "history.none").member("error", "this datapoint has no history").end_object();
			continue;
		}
		HistoryAnswer answer;
		// The store that answers is known before the points (it follows from the range and the specs).
		hist.store_for(ids[i], from, to, static_cast<uint32_t>(points), answer);
		json.key("store");
		if (answer.resolution_s == 0) {
			json.string("raw");
		} else {
			json.number(answer.resolution_s);
		}
		json.member("retention_s", answer.retention_s);
		json.key("points").begin_array();
		Points sink(json, answer.boolean);
		if (!full) {
			hist.query(ids[i], from, to, static_cast<uint32_t>(points), sink, answer);
		}
		json.end_array();
		if (full || sink.truncated) {
			json.member("truncated", true);
			full = true;
		}
		json.end_object();
	}
	json.release();
	json.end_array().end_object();
	finish(response, json);
}

namespace {

/// A label of the rule set (spec/rules.md 2.2): the image's language map as it came, or the text.
void write_label(JsonWriter& json, const rules::RuleSet& set, const char* key, uint32_t text, uint32_t map) {
	json.key(key);
	const char* raw = set.label_json(map);
	if (raw != nullptr) {
		json.raw(raw, std::strlen(raw));
	} else {
		json.string(set.str(text));
	}
}

} // namespace

void WebApi::write_rule(JsonWriter& json, size_t index) {
	rules::Engine& engine = image_->engine();
	const rules::RuleSet& set = engine.rules();
	const rules::Rule& rule = set.rule(index);
	rules::RuleDiagnostics d;
	engine.diagnostics(index, d);
	const rules::Parameter& enabled = set.parameter(rule.first_parameter);
	json.begin_object().member("id", d.id);
	write_label(json, set, "name", rule.name, rule.name_json);
	json.member("enabled", engine.enabled(index))
		.member("enabledAdjustable", enabled.adjustable)
		.member("enabledOverridden", engine.parameter_overridden(index, 0))
		.member("firings", d.firings);
	json.key("state");
	if (d.state != nullptr) {
		json.string(d.state);
	} else {
		json.null();
	}
	write_event(json, "lastTrigger", d.has_trigger, d.last_trigger, d.last_trigger_ms);
	write_event(json, "lastAction", d.has_action, d.last_action, d.last_action_ms);
	json.member("errors", d.errors);
	json.key("lastError");
	if (d.has_error) {
		json.begin_object()
			.member("code", rules::log_code_name(d.last_error_code))
			.member("text", d.last_error_text)
			.member("ms", d.last_error_ms)
			.end_object();
	} else {
		json.null();
	}
	json.key("timers").begin_array();
	for (size_t t = 0; t < rule.timer_count; ++t) {
		int64_t remaining = 0;
		if (engine.timer_remaining(index, t, remaining)) {
			json.begin_object()
				.member("id", set.str(set.timer(rule.first_timer + t).id))
				.member("remaining_ms", remaining)
				.end_object();
		}
	}
	json.end_array();
	json.key("parameters").begin_array();
	const image::ModelView& view = image_->view();
	for (size_t p = 1; p < rule.parameter_count; ++p) {
		const rules::Parameter& decl = set.parameter(rule.first_parameter + p);
		const char* id = set.str(decl.id);
		json.begin_object().member("id", id);
		write_label(json, set, "label", decl.label, decl.label_json);
		json.member("type", rules::type_name(decl.type))
			.member("unit", set.str(decl.unit))
			.member("adjustable", decl.adjustable)
			.member("overridden", engine.parameter_overridden(index, p));
		json.key("value");
		write_val(json, engine.parameter_value(index, p));
		json.key("default");
		write_val(json, set.constant(decl.default_value));
		if (decl.min != rules::kNone) {
			json.key("min");
			write_val(json, set.constant(decl.min));
		}
		if (decl.max != rules::kNone) {
			json.key("max");
			write_val(json, set.constant(decl.max));
		}
		double step = view.parameter_step(d.id, id);
		if (step > 0) {
			json.member("step", step);
		}
		if (decl.choice_count > 0) {
			json.key("choices").begin_array();
			for (size_t c = 0; c < decl.choice_count; ++c) {
				const rules::Choice& choice = set.choice(decl.first_choice + c);
				json.begin_object().key("value");
				write_val(json, set.constant(choice.value));
				write_label(json, set, "label", choice.label, choice.label_json);
				json.end_object();
			}
			json.end_array();
		}
		json.end_object();
	}
	json.end_array();
	json.end_object();
}

void WebApi::rules(const WebRequest& request, WebResponse& response) {
	uint32_t from = 0;
	query_uint(request.query, "from", from);
	rules::Engine& engine = image_->engine();
	size_t count = engine.loaded() ? engine.rules().rule_count() : 0;
	JsonWriter json(response.body, response.capacity);
	json.reserve(kClosingReserve);
	json.begin_object()
		.member("now_ms", controller_.clock().monotonic_ms())
		.member("count", static_cast<uint32_t>(count))
		.member("from", from);
	json.key("rules").begin_array();
	size_t index = from;
	size_t written = 0;
	for (; index < count; ++index) {
		JsonWriter::Mark mark = json.mark();
		write_rule(json, index);
		if (json.overflowed()) {
			json.reset(mark);
			break;
		}
		++written;
	}
	if (written == 0 && index < count) {
		refuse(response, 500, "api.response_too_large", "a rule does not fit the server's buffer");
		return;
	}
	json.release();
	json.end_array();
	json.key("next");
	if (index < count) {
		json.number(static_cast<uint32_t>(index));
	} else {
		json.null();
	}
	json.end_object();
	finish(response, json);
}

void WebApi::rule_parameter(const char* rule, const char* parameter, const WebRequest& request,
	WebResponse& response) {
	rules::Engine& engine = image_->engine();
	int index = engine.loaded() ? engine.rules().find_rule(rule) : -1;
	if (index < 0) {
		refuse(response, 404, "rule.unknown", "no rule with this id");
		return;
	}
	JsonSpan body;
	JsonSpan member;
	const char* error = nullptr;
	if (!json_parse(request.body, request.body_length, body, &error) || body.kind != JsonSpan::Kind::object) {
		refuse(response, 400, "api.bad_json", error != nullptr ? error : "the body must be a JSON object");
		return;
	}
	bool reset = false;
	rules::ParameterStatus status = rules::ParameterStatus::ok;
	const char* reason = nullptr;
	if (json_member(body, "reset", member) && json_bool(member, reset) && reset) {
		status = engine.reset_parameter(rule, parameter, &reason);
	} else if (json_member(body, "value", member)) {
		JsonSpanDocument document;
		status = engine.set_parameter(rule, parameter, document.value(member), &reason);
	} else {
		refuse(response, 400, "rule.value_missing", "the body needs {\"value\": ...} or {\"reset\": true}");
		return;
	}
	if (status != rules::ParameterStatus::ok) {
		Log::warn(kTag, "parameter %s/%s refused: %s", rule, parameter, reason != nullptr ? reason : "?");
		refuse(response, parameter_http_status(status), parameter_code(status),
			reason != nullptr ? reason : rules::parameter_status_name(status));
		return;
	}
	Log::info(kTag, "parameter %s/%s %s from the API", rule, parameter, reset ? "reset" : "set");
	JsonWriter json(response.body, response.capacity);
	json.begin_object().member("ok", true).member("now_ms", controller_.clock().monotonic_ms()).key("rule");
	write_rule(json, static_cast<size_t>(index));
	json.end_object();
	finish(response, json);
}

namespace {

void write_clock(JsonWriter& json, int64_t seconds) {
	json.begin_object()
		.member("hour", static_cast<uint32_t>(seconds / 3600))
		.member("minute", static_cast<uint32_t>(seconds % 3600 / 60))
		.member("second", static_cast<uint32_t>(seconds % 60))
		.end_object();
}

/// A member's scalar in its JSON form of the image (spec/schedules.md 3): a time of day as
/// {"hour","minute","second"}, days as [1..7], offset and every in milliseconds, at as
/// {"year","month","day","hour","minute","second"}.
void write_member(JsonWriter& json, uint16_t member, int64_t value) {
	if (ScheduleMember::is_time(member)) {
		write_clock(json, value);
	} else if (member == ScheduleMember::kDays) {
		json.begin_array();
		for (unsigned day = 1; day <= 7; ++day) {
			if ((value & Weekdays::bit(day)) != 0) {
				json.number(static_cast<uint32_t>(day));
			}
		}
		json.end_array();
	} else if (member == ScheduleMember::kAt) {
		Date date = date_from_days(value / 86400);
		int64_t rest = value % 86400;
		json.begin_object()
			.member("year", static_cast<uint32_t>(date.year))
			.member("month", static_cast<uint32_t>(date.month))
			.member("day", static_cast<uint32_t>(date.day))
			.member("hour", static_cast<uint32_t>(rest / 3600))
			.member("minute", static_cast<uint32_t>(rest % 3600 / 60))
			.member("second", static_cast<uint32_t>(rest % 60))
			.end_object();
	} else {
		json.number(value * 1000); // offset, every: milliseconds
	}
}

void write_members(JsonWriter& json, const char* name, uint16_t members) {
	json.key(name).begin_array();
	for (size_t i = 0; i < ScheduleMember::kCount; ++i) {
		if ((members & ScheduleMember::bit(i)) != 0) {
			json.string(ScheduleMember::name(ScheduleMember::bit(i)));
		}
	}
	json.end_array();
}

bool integer_member(const JsonSpan& object, const char* name, int64_t min, int64_t max, int64_t& out) {
	JsonSpan span;
	double value = 0;
	if (!json_member(object, name, span) || span.kind != JsonSpan::Kind::number || !json_number(span, value) ||
		std::floor(value) != value || value < static_cast<double>(min) || value > static_cast<double>(max)) {
		return false;
	}
	out = static_cast<int64_t>(value);
	return true;
}

/// Reads a member's value from its JSON form (write_member()) into `values`; false with `error`
/// set when it is not a value of the member's form.
bool read_member(const JsonSpan& span, uint16_t member, ScheduleSpec& values, const char*& error) {
	if (ScheduleMember::is_time(member) || member == ScheduleMember::kAt) {
		int64_t year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
		bool at = member == ScheduleMember::kAt;
		if (span.kind != JsonSpan::Kind::object || !integer_member(span, "hour", 0, 23, hour) ||
			!integer_member(span, "minute", 0, 59, minute) || !integer_member(span, "second", 0, 59, second) ||
			(at && (!integer_member(span, "year", 1970, 2155, year) || !integer_member(span, "month", 1, 12, month) ||
				!integer_member(span, "day", 1, 31, day) ||
				day > static_cast<int64_t>(days_in_month(static_cast<int32_t>(year), static_cast<unsigned>(month)))))) {
			error = at ? "not a date and time {\"year\", \"month\", \"day\", \"hour\", \"minute\", \"second\"}"
				: "not a time of day {\"hour\", \"minute\", \"second\"}";
			return false;
		}
		int64_t seconds = hour * 3600 + minute * 60 + second;
		if (at) {
			seconds += days_from_civil(static_cast<int32_t>(year), static_cast<unsigned>(month),
				static_cast<unsigned>(day)) * 86400;
		}
		set_schedule_member_value(values, member, seconds);
		return true;
	}
	if (member == ScheduleMember::kDays) {
		if (span.kind != JsonSpan::Kind::array) {
			error = "days must be an array of 1 (Monday) .. 7 (Sunday)";
			return false;
		}
		uint8_t days = 0;
		size_t position = 0;
		JsonSpan item;
		while (json_next_item(span, position, item)) {
			double day = 0;
			if (item.kind != JsonSpan::Kind::number || !json_number(item, day) || std::floor(day) != day || day < 1 ||
				day > 7 || (days & Weekdays::bit(static_cast<unsigned>(day))) != 0) {
				error = "days must list each of 1 (Monday) .. 7 (Sunday) at most once";
				return false;
			}
			days = static_cast<uint8_t>(days | Weekdays::bit(static_cast<unsigned>(day)));
		}
		values.days = days; // empty: refused by the scheduler as malformed
		return true;
	}
	// offset, every: milliseconds, whole seconds (every: whole minutes).
	double ms = 0;
	int64_t unit = member == ScheduleMember::kEvery ? 60000 : 1000;
	if (span.kind != JsonSpan::Kind::number || !json_number(span, ms) || std::floor(ms) != ms || ms < -43200000.0 ||
		ms > 43200000.0 || static_cast<int64_t>(ms) % unit != 0) {
		error = member == ScheduleMember::kEvery ? "every must be milliseconds, a whole number of minutes"
			: "offset must be milliseconds, a whole number of seconds, at most 12 h";
		return false;
	}
	int64_t seconds = static_cast<int64_t>(ms) / 1000;
	if (member == ScheduleMember::kEvery && seconds < 60) {
		error = "every must be at least a minute";
		return false;
	}
	set_schedule_member_value(values, member, seconds);
	return true;
}

void refuse_member(WebResponse& response, int status, const char* code, const char* text, uint16_t member) {
	response.status = status;
	response.content_type = "application/json";
	JsonWriter json(response.body, response.capacity);
	json.begin_object().member("error", text).member("code", code);
	if (member != 0) {
		json.member("member", ScheduleMember::name(member));
	}
	json.end_object();
	response.length = json.overflowed() ? 0 : json.length();
}

} // namespace

void WebApi::write_schedule(JsonWriter& json, size_t index) {
	Scheduler& scheduler = image_->scheduler();
	uint16_t adjustable = scheduler.adjustable(index);
	json.begin_object()
		.member("id", scheduler.id(index))
		.member("label", scheduler.label(index))
		.member("kind", schedule_kind_name(scheduler.kind(index)))
		.member("enabled", scheduler.enabled(index))
		.member("enabledAdjustable", scheduler.enabled_adjustable(index))
		.member("enabledOverridden", scheduler.enabled_overridden(index));
	write_members(json, "adjustable", adjustable);
	write_members(json, "overridden", scheduler.overridden(index));
	const ScheduleSpec& spec = scheduler.spec(index);
	ScheduleSpec image = scheduler.image_spec(index);
	json.key("values").begin_object();
	for (size_t i = 0; i < ScheduleMember::kCount; ++i) {
		uint16_t member = ScheduleMember::bit(i);
		if ((adjustable & member) != 0) {
			json.key(ScheduleMember::name(member));
			write_member(json, member, schedule_member_value(spec, member));
		}
	}
	json.end_object();
	json.key("image").begin_object();
	for (size_t i = 0; i < ScheduleMember::kCount; ++i) {
		uint16_t member = ScheduleMember::bit(i);
		if ((adjustable & member) != 0) {
			json.key(ScheduleMember::name(member));
			write_member(json, member, schedule_member_value(image, member));
		}
	}
	json.end_object();
	json.key("bounds").begin_object();
	for (size_t i = 0; i < ScheduleMember::kCount; ++i) {
		uint16_t member = ScheduleMember::bit(i);
		const ScheduleBound* bound = scheduler.bound(index, member);
		if (bound == nullptr) {
			continue;
		}
		json.key(ScheduleMember::name(member)).begin_object();
		json.key("min");
		write_member(json, member, bound->min);
		json.key("max");
		write_member(json, member, bound->max);
		if (bound->step > 0) {
			// seconds for a time of day, milliseconds for the durations
			json.member("step", ScheduleMember::is_time(member) ? bound->step : bound->step * 1000);
		}
		json.end_object();
	}
	json.end_object();
	std::optional<int64_t> next = scheduler.next_firing(index);
	json.key("nextFiring");
	if (next) {
		json.number(*next);
	} else {
		json.null();
	}
	std::optional<int64_t> last = scheduler.last_fired(index);
	json.key("lastFired");
	if (last) {
		json.number(*last);
	} else {
		json.null();
	}
	json.end_object();
}

void WebApi::schedules(const WebRequest& request, WebResponse& response) {
	uint32_t from = 0;
	query_uint(request.query, "from", from);
	Scheduler& scheduler = image_->scheduler();
	size_t count = scheduler.size();
	WallTime wall = controller_.clock().wall_time();
	JsonWriter json(response.body, response.capacity);
	json.reserve(kClosingReserve);
	json.begin_object()
		.member("synchronized", wall.synchronized)
		.member("now", wall.seconds)
		.member("count", static_cast<uint32_t>(count))
		.member("from", from);
	json.key("schedules").begin_array();
	size_t index = from;
	for (; index < count; ++index) {
		JsonWriter::Mark mark = json.mark();
		write_schedule(json, index);
		if (json.overflowed()) {
			json.reset(mark);
			break;
		}
	}
	json.release();
	json.end_array();
	json.key("next");
	if (index < count) {
		json.number(static_cast<uint32_t>(index));
	} else {
		json.null();
	}
	json.end_object();
	finish(response, json);
}

void WebApi::put_schedule(const char* id, const WebRequest& request, WebResponse& response) {
	Scheduler& scheduler = image_->scheduler();
	std::optional<size_t> index = scheduler.find(id);
	if (!index) {
		refuse(response, 404, "schedule.unknown", "no schedule entry with this id");
		return;
	}
	JsonSpan body;
	JsonSpan member;
	const char* error = nullptr;
	if (!json_parse(request.body, request.body_length, body, &error) || body.kind != JsonSpan::Kind::object) {
		refuse(response, 400, "api.bad_json", error != nullptr ? error : "the body must be a JSON object");
		return;
	}
	// The members: `enabled`, the adjustable members of spec/schedules.md 2.1, or `reset`.
	bool has_enabled = false;
	bool enabled = false;
	bool reset = false;
	uint16_t members = 0;
	ScheduleSpec values = scheduler.spec(*index);
	size_t position = 0;
	JsonSpan key;
	JsonSpan value;
	while (json_next_member(body, position, key, value)) {
		char name[16];
		if (!json_string(key, name, sizeof(name))) {
			name[0] = '\0';
		}
		if (std::strcmp(name, "enabled") == 0) {
			if (!json_bool(value, enabled)) {
				refuse(response, 400, "schedule.value_missing", "enabled must be true or false");
				return;
			}
			has_enabled = true;
			continue;
		}
		if (std::strcmp(name, "reset") == 0) {
			if (!json_bool(value, reset) || !reset) {
				refuse(response, 400, "schedule.malformed", "reset must be true");
				return;
			}
			continue;
		}
		uint16_t bit = ScheduleMember::of_name(name, std::strlen(name));
		if (bit == 0) {
			char text[64];
			std::snprintf(text, sizeof(text), "unknown member '%s'", name);
			refuse(response, 400, "schedule.malformed", text);
			return;
		}
		const char* why = nullptr;
		if (!read_member(value, bit, values, why)) {
			refuse_member(response, 400, "schedule.malformed", why, bit);
			return;
		}
		members = static_cast<uint16_t>(members | bit);
	}
	if (reset) {
		if (has_enabled || members != 0) {
			refuse(response, 400, "schedule.malformed", "reset takes no other members");
			return;
		}
		reset_schedule(id, response);
		return;
	}
	if (!has_enabled && members == 0) {
		refuse(response, 400, "schedule.value_missing",
			"the body needs {\"enabled\": true|false}, adjustable members, or {\"reset\": true}");
		return;
	}
	if (has_enabled && !scheduler.enabled_adjustable(*index)) {
		refuse(response, 403, "schedule.not_adjustable", "the image fixes this schedule's enable switch");
		return;
	}
	if (members != 0) {
		uint16_t culprit = 0;
		const char* why = nullptr;
		ScheduleChange change = scheduler.set_members(id, members, values, &culprit, &why);
		if (change != ScheduleChange::ok) {
			char code[40];
			std::snprintf(code, sizeof(code), "schedule.%s", schedule_change_name(change));
			refuse_member(response, change == ScheduleChange::not_adjustable ? 403 : 400, code,
				why != nullptr ? why : "refused", culprit);
			return;
		}
		Log::info(kTag, "schedule %s changed from the API", id);
	}
	if (has_enabled) {
		if (!scheduler.set_enabled(id, enabled, &error)) {
			refuse(response, 403, "schedule.not_adjustable", error != nullptr ? error : "the entry's switch is not adjustable");
			return;
		}
		Log::info(kTag, "schedule %s %s from the API", id, enabled ? "enabled" : "disabled");
	}
	schedule_changed(*index, response);
}

void WebApi::reset_schedule(const char* id, WebResponse& response) {
	Scheduler& scheduler = image_->scheduler();
	std::optional<size_t> index = scheduler.find(id);
	if (!index || !scheduler.reset_overrides(id)) {
		refuse(response, 404, "schedule.unknown", "no schedule entry with this id");
		return;
	}
	Log::info(kTag, "schedule %s back to the image's values from the API", id);
	schedule_changed(*index, response);
}

void WebApi::schedule_changed(size_t index, WebResponse& response) {
	JsonWriter json(response.body, response.capacity);
	json.begin_object().member("ok", true).key("schedule");
	write_schedule(json, index);
	json.end_object();
	finish(response, json);
}

} // namespace hc
