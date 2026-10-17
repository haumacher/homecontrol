/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/web_api.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hc/live_feed.h"
#include "hc/network_settings.h"
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
};

constexpr SettingInfo kShownSettings[] = {
	{setting::kTimeZone, true},
	{setting::kNtpServer, true},
	{setting::kUpdateUrl, true},
	{setting::kWifiSsid, false},
};

const SettingInfo* find_setting(const char* key) {
	for (const SettingInfo& info : kShownSettings) {
		if (std::strcmp(info.key, key) == 0) {
			return &info;
		}
	}
	return nullptr;
}

} // namespace

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
	: controller_(controller), model_(model), auth_(auth), host_(host), log_copy_(nullptr) {}

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

void WebApi::set_session_cookie(WebResponse& response, const char* session) {
	if (session == nullptr) {
		std::snprintf(response.set_cookie, sizeof(response.set_cookie),
			"%s=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict", Auth::kCookieName);
	} else {
		std::snprintf(response.set_cookie, sizeof(response.set_cookie), "%s=%s; Path=/; HttpOnly; SameSite=Strict",
			Auth::kCookieName, session);
	}
}

void WebApi::handle(const WebRequest& request, WebResponse& response) {
	response.status = 200;
	response.length = 0;
	response.needs_hash = false;
	const char* path = request.path != nullptr ? request.path : "";
	bool state_changing = request.method != HttpMethod::get;
	bool csrf_ok = request.requested_with != nullptr && std::strcmp(request.requested_with, kCsrfHeaderValue) == 0;

	// ---- Open endpoints ----
	if (starts_with(path, "/api/auth/")) {
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
	if (request.authorization != nullptr) {
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
	refuse(response, 404, "api.not_found", "no such API endpoint");
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
		.member("ws_port", static_cast<uint32_t>(host_.websocket_port()))
		.member("min_password", static_cast<uint32_t>(Auth::kMinPassword))
		.end_object();
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

void WebApi::auth_onboard(const WebRequest& request, WebResponse& response) {
	if (request.hash == nullptr) {
		if (auth_.has_password()) {
			refuse(response, 409, auth_status_code(AuthStatus::password_set), auth_status_text(AuthStatus::password_set));
			return;
		}
		if (!read_password(request, response)) {
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
	AuthStatus status = auth_.finish_onboarding(request.hash->params, request.hash->key, request.source, session);
	if (status != AuthStatus::ok) {
		refuse(response, status == AuthStatus::password_set ? 409 : 500, auth_status_code(status), auth_status_text(status));
		return;
	}
	set_session_cookie(response, session);
	JsonWriter json(response.body, response.capacity);
	json.begin_object().member("ok", true).end_object();
	finish(response, json);
}

void WebApi::auth_login(const WebRequest& request, WebResponse& response) {
	uint32_t retry_after = 0;
	if (request.hash == nullptr) {
		if (!read_password(request, response)) {
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
	AuthStatus status =
		auth_.finish_login(request.source, request.hash->params, request.hash->key, session, retry_after, failures);
	if (status == AuthStatus::ok) {
		set_session_cookie(response, session);
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
	if (ref.valid()) {
		auth_.end_session(ref);
		Log::info(kTag, "logout from %s", request.source);
	}
	set_session_cookie(response, nullptr);
	JsonWriter json(response.body, response.capacity);
	json.begin_object().member("ok", true).end_object();
	finish(response, json);
}

// ---- Status -----------------------------------------------------------------------------------

void WebApi::write_status(JsonWriter& json) {
	Controller& ctl = controller_;
	const Connectivity& net = ctl.connectivity();
	json.key("device").begin_object()
		.member("name", host_.device_name())
		.member("version", kVersion)
		.member("build", build_id())
		.member("ui", host_.ui_version())
		.end_object();
	json.member("uptime_s", ctl.uptime_ms() / 1000);
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
		.member("tokens", static_cast<uint32_t>(auth_.token_count()))
		.member("ws_clients", static_cast<uint32_t>(feed_ != nullptr ? feed_->client_count() : 0))
		.end_object();
	json.member("datapoints", static_cast<uint32_t>(model_.size()));
}

void WebApi::status(WebResponse& response) {
	JsonWriter json(response.body, response.capacity);
	json.begin_object();
	write_status(json);
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
	json.key("value");
	write_value_json(json, dp.value());
	json.member("quality", quality_name(dp.quality()))
		.member("updated_ms", dp.updated_ms())
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
	bool copied = false;
	for (int attempt = 0; attempt < 8 && !copied; ++attempt) {
		copied = controller_.log_ring().copy_to(log_copy_);
	}
	if (!copied) {
		refuse(response, 503, "log.busy", "the log is being written: try again");
		return;
	}
	JsonWriter json(response.body, response.capacity);
	json.reserve(kClosingReserve);
	json.begin_object()
		.member("now_ms", controller_.clock().monotonic_ms())
		.member("first", log_copy_.first_seq())
		.member("last_seq", log_copy_.last_seq())
		.member("dropped", log_copy_.dropped());
	json.key("lines").begin_array();
	LogWriter writer(json);
	log_copy_.read(after, writer);
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
		}
		json.begin_object().member("key", info.key);
		json.key("value");
		if (status == SettingsStatus::ok) {
			json.string(value);
		} else {
			json.null();
		}
		json.member("default", fallback).member("editable", info.editable).end_object();
	}
	json.end_array().end_object();
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
	if (std::strcmp(key, setting::kUpdateUrl) == 0) {
		return host_.set_update_url(value, error);
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

} // namespace hc
