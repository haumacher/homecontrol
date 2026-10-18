/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The routes of hc::WebApi that carry the serial console's commands over the network (issue #38,
// spec/api.md "System", "Tokens", "Zigbee"): an installed device has no cable attached.
#include <cstdio>
#include <cstring>

#include "hc/diagnostics.h"
#include "hc/external_values.h"
#include "hc/knx/telegram_log.h"
#include "hc/knx/tunnel.h"
#include "hc/language_packs.h"
#include "hc/live_feed.h"
#include "hc/mqtt.h"
#include "hc/network_settings.h"
#include "hc/web_api.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.web";

bool starts_with(const char* text, const char* prefix) {
	return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

const char* method_text(HttpMethod method) {
	switch (method) {
		case HttpMethod::get: return "GET";
		case HttpMethod::post: return "POST";
		case HttpMethod::put: return "PUT";
		case HttpMethod::del: return "DELETE";
		default: return "?";
	}
}

/// Refuses and logs why: nothing the console could do declines silently over the network.
void decline(const WebRequest& request, WebResponse& response, int status, const char* code, const char* text) {
	Log::warn(kTag, "%s %s from %s refused (%s): %s", method_text(request.method), request.path, request.source, code,
		text);
	WebApi::refuse(response, status, code, text);
}

bool method_is(const WebRequest& request, WebResponse& response, HttpMethod method) {
	if (request.method == method) {
		return true;
	}
	switch (method) {
		case HttpMethod::get: WebApi::refuse(response, 405, "api.method_not_allowed", "use GET"); break;
		case HttpMethod::post: WebApi::refuse(response, 405, "api.method_not_allowed", "use POST"); break;
		case HttpMethod::put: WebApi::refuse(response, 405, "api.method_not_allowed", "use PUT"); break;
		case HttpMethod::del: WebApi::refuse(response, 405, "api.method_not_allowed", "use DELETE"); break;
		default: WebApi::refuse(response, 405, "api.method_not_allowed", "method not allowed"); break;
	}
	return false;
}

/// Percent-decodes a path segment (no '/', no NUL) into `out`; false if empty, malformed or too long.
bool decode_segment(const char* text, size_t length, char* out, size_t size) {
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
		if (c == '/' || n + 1 >= size) {
			return false;
		}
		out[n++] = c;
	}
	out[n] = '\0';
	return n > 0;
}

/// Whether `tag` may name a log tag: "*" alone, or 1 to Log::kMaxTagLength printable characters
/// without spaces and '*'.
bool log_tag_valid(const char* tag) {
	if (std::strcmp(tag, "*") == 0) {
		return true;
	}
	size_t length = std::strlen(tag);
	if (length == 0 || length > Log::kMaxTagLength) {
		return false;
	}
	for (size_t i = 0; i < length; ++i) {
		unsigned char c = static_cast<unsigned char>(tag[i]);
		if (c <= ' ' || c >= 0x7f || c == '*') {
			return false;
		}
	}
	return true;
}

int auth_http_status(AuthStatus status) {
	switch (status) {
		case AuthStatus::name_invalid: return 422;
		case AuthStatus::name_taken:
		case AuthStatus::capacity: return 409;
		case AuthStatus::not_found: return 404;
		default: return 500;
	}
}

struct BlobCounter : BlobVisitor {
	void blob(const char*, size_t size) override {
		++files;
		bytes += size;
	}
	uint32_t files = 0;
	uint64_t bytes = 0;
};

} // namespace

// ---- Shared ------------------------------------------------------------------------------------

bool WebApi::parse_body(const WebRequest& request, WebResponse& response, JsonSpan& body, bool optional) {
	if (optional && (request.body == nullptr || request.body_length == 0)) {
		body = JsonSpan();
		return true;
	}
	const char* error = nullptr;
	if (!json_parse(request.body, request.body_length, body, &error) || body.kind != JsonSpan::Kind::object) {
		decline(request, response, 400, "api.bad_json", error != nullptr ? error : "the body must be a JSON object");
		return false;
	}
	return true;
}

void WebApi::ok(WebResponse& response) {
	JsonWriter json(response.body, response.capacity);
	json.begin_object().member("ok", true).end_object();
	finish(response, json);
}

// ---- System: restart, factory reset, update, log levels, WiFi -----------------------------------

void WebApi::system(const char* what, const WebRequest& request, WebResponse& response) {
	if (std::strcmp(what, "restart") == 0) {
		if (method_is(request, response, HttpMethod::post)) {
			system_action(SystemAction::restart, request, response);
		}
		return;
	}
	if (std::strcmp(what, "factory-reset") == 0) {
		if (method_is(request, response, HttpMethod::post)) {
			system_action(SystemAction::factory_reset, request, response);
		}
		return;
	}
	if (std::strcmp(what, "update/check") == 0 || std::strcmp(what, "update/now") == 0) {
		if (method_is(request, response, HttpMethod::post)) {
			system_update(what[7] == 'n', request, response);
		}
		return;
	}
	if (std::strcmp(what, "log-level") == 0) {
		if (request.method != HttpMethod::get && request.method != HttpMethod::put) {
			refuse(response, 405, "api.method_not_allowed", "use GET or PUT");
			return;
		}
		log_level(request, response);
		return;
	}
	if (std::strcmp(what, "wifi") == 0) {
		if (method_is(request, response, HttpMethod::put)) {
			wifi(request, response);
		}
		return;
	}
	refuse(response, 404, "api.not_found", "no such API endpoint");
}

void WebApi::system_action(SystemAction action, const WebRequest& request, WebResponse& response) {
	bool reset = action == SystemAction::factory_reset;
	const char* reason = nullptr;
	if (host_.system_action_possible(action, &reason) != HostResult::ok) {
		decline(request, response, 501, "system.unavailable", reason);
		return;
	}
	if (pending_ != Pending::none) {
		decline(request, response, 409, "system.busy", "a restart is already under way");
		return;
	}
	if (reset) {
		JsonSpan body;
		JsonSpan member;
		if (!parse_body(request, response, body)) {
			return;
		}
		char confirm[64];
		if (!json_member(body, "confirm", member) || !json_string(member, confirm, sizeof(confirm))) {
			decline(request, response, 400, "system.confirm_missing",
				"the body needs {\"confirm\": \"<the device's name>\"}");
			return;
		}
		if (std::strcmp(confirm, host_.device_name()) != 0) {
			decline(request, response, 422, "system.confirm_mismatch",
				"the confirmation is not the device's name: nothing was erased");
			return;
		}
	}
	Log::warn(kTag, "%s requested over the API from %s: in %u ms", reset ? "factory reset" : "restart", request.source,
		static_cast<unsigned>(kActionDelayMs));
	pending_ = reset ? Pending::factory_reset : Pending::restart;
	controller_.loop().cancel(action_timer_);
	action_timer_ = controller_.loop().schedule_at(controller_.clock().monotonic_ms() + kActionDelayMs,
		Callback::member<WebApi, &WebApi::action_due>(this));
	JsonWriter json(response.body, response.capacity);
	json.begin_object()
		.member("ok", true)
		.member("action", reset ? "factory-reset" : "restart")
		.member("in_ms", kActionDelayMs)
		.end_object();
	finish(response, json);
}

void WebApi::action_due() {
	action_timer_ = TimerHandle();
	Pending pending = pending_;
	pending_ = Pending::none;
	switch (pending) {
		case Pending::restart: host_.system_action(SystemAction::restart); break;
		case Pending::factory_reset: host_.system_action(SystemAction::factory_reset); break;
		case Pending::wifi: controller_.connectivity().request_connect("new WiFi credentials from the API"); break;
		case Pending::none: break;
	}
}

void WebApi::system_update(bool force, const WebRequest& request, WebResponse& response) {
	JsonSpan body;
	if (!parse_body(request, response, body, true)) {
		return;
	}
	char url[Settings::kMaxStringLength + 1] = {};
	JsonSpan member;
	if (body.kind == JsonSpan::Kind::object && json_member(body, "url", member)) {
		if (!force) {
			decline(request, response, 400, "update.bad_request", "a check uses update.url; only update/now takes a url");
			return;
		}
		if (!json_string(member, url, sizeof(url)) || url[0] == '\0') {
			decline(request, response, 422, "update.invalid", "url must be an http(s) URL of at most 127 bytes");
			return;
		}
	}
	const char* reason = nullptr;
	HostResult result = host_.start_update(force, url[0] != '\0' ? url : nullptr, &reason);
	if (result == HostResult::unavailable) {
		decline(request, response, 501, "system.unavailable", reason);
		return;
	}
	if (result != HostResult::ok) {
		decline(request, response, 409, "update.refused", reason);
		return;
	}
	Log::info(kTag, "%s requested over the API from %s", force ? "update now" : "update check", request.source);
	JsonWriter json(response.body, response.capacity);
	json.begin_object().member("ok", true).member("force", force).end_object();
	finish(response, json);
}

void WebApi::log_level(const WebRequest& request, WebResponse& response) {
	if (request.method == HttpMethod::put) {
		JsonSpan body;
		JsonSpan member;
		if (!parse_body(request, response, body)) {
			return;
		}
		char tag[Log::kMaxTagLength + 2];
		char name[16];
		LogLevel level = LogLevel::info;
		if (!json_member(body, "tag", member) || !json_string(member, tag, sizeof(tag)) || !log_tag_valid(tag)) {
			decline(request, response, 422, "log.invalid",
				"tag must be \"*\" or a log tag of 1 to 23 characters without spaces");
			return;
		}
		if (!json_member(body, "level", member) || !json_string(member, name, sizeof(name)) ||
			!log_level_parse(name, level)) {
			decline(request, response, 422, "log.invalid", "level must be error, warn, info or debug");
			return;
		}
		bool core_tag = std::strcmp(tag, "*") == 0 || starts_with(tag, "hc.");
		const char* error = nullptr;
		if (core_tag && !Log::set_tag_level(tag, level, &error)) {
			decline(request, response, 422, "log.invalid", error);
			return;
		}
		bool platform = host_.set_platform_log_level(tag, level);
		if (!core_tag && !platform) {
			decline(request, response, 422, "log.platform_tag",
				"tags outside hc.* belong to the platform's log, which this platform does not have");
			return;
		}
		Log::info(kTag, "log level of %s set to %s over the API", tag, log_level_name(level));
	}
	JsonWriter json(response.body, response.capacity);
	json.begin_object().member("default", log_level_name(Log::level()));
	json.key("tags").begin_array();
	for (size_t i = 0; i < Log::tag_level_count(); ++i) {
		const char* tag = nullptr;
		LogLevel level = LogLevel::info;
		if (Log::tag_level_at(i, tag, level)) {
			json.begin_object().member("tag", tag).member("level", log_level_name(level)).end_object();
		}
	}
	json.end_array();
	json.member("max_tags", static_cast<uint32_t>(Log::kMaxTagLevels));
	json.member("platform_tags", host_.has_platform_log());
	json.end_object();
	finish(response, json);
}

void WebApi::wifi(const WebRequest& request, WebResponse& response) {
	JsonSpan body;
	JsonSpan member;
	if (!parse_body(request, response, body)) {
		return;
	}
	if (pending_ != Pending::none) {
		decline(request, response, 409, "system.busy", "a restart or reconnect is already under way");
		return;
	}
	char ssid[setting::kMaxSsidLength + 2] = {};
	char psk[72] = {};
	if (!json_member(body, "ssid", member) || !json_string(member, ssid, sizeof(ssid))) {
		decline(request, response, 422, "wifi.invalid", "the body needs {\"ssid\": \"<1 to 32 bytes>\", \"psk\": \"...\"}");
		return;
	}
	if (json_member(body, "psk", member) && member.kind != JsonSpan::Kind::null &&
		!json_string(member, psk, sizeof(psk))) {
		decline(request, response, 422, "wifi.invalid", "the key is 8 to 63 characters or 64 hex digits");
		return;
	}
	const char* error = nullptr;
	if (!store_wifi_credentials(controller_.settings(), ssid, psk, &error)) {
		std::memset(psk, 0, sizeof(psk));
		decline(request, response, 422, "wifi.invalid", error);
		return;
	}
	bool open = psk[0] == '\0';
	std::memset(psk, 0, sizeof(psk)); // never kept, never logged
	Log::warn(kTag, "WiFi credentials for \"%s\" (%s) stored over the API from %s: reconnecting in %u ms", ssid,
		open ? "open network" : "key not shown", request.source, static_cast<unsigned>(kActionDelayMs));
	pending_ = Pending::wifi;
	controller_.loop().cancel(action_timer_);
	action_timer_ = controller_.loop().schedule_at(controller_.clock().monotonic_ms() + kActionDelayMs,
		Callback::member<WebApi, &WebApi::action_due>(this));
	char mdns[80];
	std::snprintf(mdns, sizeof(mdns), "%s.local", host_.device_name());
	JsonWriter json(response.body, response.capacity);
	json.begin_object()
		.member("ok", true)
		.member("ssid", ssid)
		.member("open", open)
		.member("reconnect_in_ms", kActionDelayMs)
		.member("mdns", mdns)
		.end_object();
	finish(response, json);
}

void WebApi::clear_coredump(WebResponse& response) {
	const Diagnostics* diagnostics = controller_.diagnostics();
	if (diagnostics == nullptr || diagnostics->coredump_size() == 0) {
		refuse(response, 404, "coredump.none", "no core dump is stored: nothing to clear");
		return;
	}
	const char* reason = nullptr;
	HostResult result = host_.clear_coredump(&reason);
	if (result == HostResult::unavailable) {
		Log::warn(kTag, "core dump not cleared: %s", reason);
		refuse(response, 501, "system.unavailable", reason);
		return;
	}
	if (result != HostResult::ok) {
		Log::warn(kTag, "core dump not cleared: %s", reason);
		refuse(response, 500, "coredump.clear_failed", reason);
		return;
	}
	Log::info(kTag, "core dump cleared over the API");
	ok(response);
}

// ---- Tokens --------------------------------------------------------------------------------------

void WebApi::tokens(const char* rest, const WebRequest& request, WebResponse& response) {
	if (rest[0] == '\0') {
		if (request.method == HttpMethod::post) {
			JsonSpan body;
			JsonSpan member;
			if (!parse_body(request, response, body)) {
				return;
			}
			char name[Auth::kMaxTokenName + 2] = {};
			if (!json_member(body, "name", member) || !json_string(member, name, sizeof(name))) {
				decline(request, response, 422, auth_status_code(AuthStatus::name_invalid),
					auth_status_text(AuthStatus::name_invalid));
				return;
			}
			char token[Auth::kTokenText];
			AuthStatus status = auth_.create_token(name, token);
			if (status != AuthStatus::ok) {
				decline(request, response, auth_http_status(status), auth_status_code(status), auth_status_text(status));
				return;
			}
			Log::info(kTag, "API token %s created over the API from %s", name, request.source);
			JsonWriter json(response.body, response.capacity);
			json.begin_object().member("ok", true).member("name", name).member("token", token).end_object();
			std::memset(token, 0, sizeof(token));
			finish(response, json);
			return;
		}
		if (!method_is(request, response, HttpMethod::get)) {
			return;
		}
		JsonWriter json(response.body, response.capacity);
		json.begin_object().key("tokens").begin_array();
		for (size_t i = 0; i < Auth::kMaxTokens; ++i) {
			const char* name = auth_.token_name(i);
			if (name != nullptr && name[0] != '\0') {
				json.begin_object().member("name", name).end_object();
			}
		}
		json.end_array().member("max", static_cast<uint32_t>(Auth::kMaxTokens)).end_object();
		finish(response, json);
		return;
	}
	char name[Auth::kMaxTokenName + 2];
	if (rest[0] != '/' || !decode_segment(rest + 1, std::strlen(rest + 1), name, sizeof(name))) {
		refuse(response, 404, auth_status_code(AuthStatus::not_found), auth_status_text(AuthStatus::not_found));
		return;
	}
	if (!method_is(request, response, HttpMethod::del)) {
		return;
	}
	AuthStatus status = auth_.revoke_token(name);
	if (status != AuthStatus::ok) {
		decline(request, response, auth_http_status(status), auth_status_code(status), auth_status_text(status));
		return;
	}
	Log::info(kTag, "API token %s revoked over the API from %s", name, request.source);
	ok(response);
}

// ---- Zigbee -------------------------------------------------------------------------------------

void WebApi::set_zigbee(zigbee::ZigbeeAdapter* adapter, const char* unavailable_reason) {
	if (zigbee_ != nullptr && zigbee_->event_listener() == this) {
		zigbee_->set_event_listener(nullptr);
	}
	zigbee_ = adapter;
	zigbee_reason_ = unavailable_reason;
	if (zigbee_ != nullptr) {
		zigbee_->set_event_listener(this);
	}
}

void WebApi::zigbee_event(const zigbee::ZigbeeEvent& event) {
	if (feed_ != nullptr) {
		feed_->zigbee_event(event);
	}
}

void WebApi::zigbee(const char* rest, const WebRequest& request, WebResponse& response) {
	if (zigbee_ == nullptr) {
		decline(request, response, 501, "system.unavailable",
			zigbee_reason_ != nullptr ? zigbee_reason_ : "this controller has no Zigbee coordinator");
		return;
	}
	if (rest[0] == '\0') {
		if (method_is(request, response, HttpMethod::get)) {
			zigbee_list(request, response);
		}
		return;
	}
	if (std::strcmp(rest, "/export") == 0) {
		if (method_is(request, response, HttpMethod::get)) {
			zigbee_export(request, response);
		}
		return;
	}
	const char* error = nullptr;
	if (std::strcmp(rest, "/permit") == 0) {
		if (!method_is(request, response, HttpMethod::post)) {
			return;
		}
		JsonSpan body;
		JsonSpan member;
		if (!parse_body(request, response, body)) {
			return;
		}
		double seconds = -1;
		if (!json_member(body, "seconds", member) || !json_number(member, seconds) || seconds < 0 ||
			seconds > zigbee::ZigbeeAdapter::kMaxPermitJoinS || seconds != static_cast<double>(static_cast<uint32_t>(seconds))) {
			decline(request, response, 422, "zigbee.invalid", "seconds is a whole number of 0 to 254 (0 closes)");
			return;
		}
		if (!zigbee_->permit_join(static_cast<uint32_t>(seconds), &error)) {
			decline(request, response, 409, "zigbee.refused", error);
			return;
		}
		JsonWriter json(response.body, response.capacity);
		json.begin_object().member("ok", true).member("seconds", static_cast<uint32_t>(seconds)).end_object();
		finish(response, json);
		return;
	}
	// /devices/<ieee>[/identify|/interview|/label]
	if (!starts_with(rest, "/devices/")) {
		refuse(response, 404, "api.not_found", "no such API endpoint");
		return;
	}
	const char* id = rest + 9;
	const char* slash = std::strchr(id, '/');
	size_t length = slash != nullptr ? static_cast<size_t>(slash - id) : std::strlen(id);
	const char* op = slash != nullptr ? slash + 1 : "";
	char text[32];
	zigbee::Ieee ieee = 0;
	if (length == 0 || length >= sizeof(text)) {
		decline(request, response, 404, "zigbee.unknown_device", "no device with this IEEE address");
		return;
	}
	std::memcpy(text, id, length);
	text[length] = '\0';
	if (!zigbee::parse_ieee(text, ieee) || zigbee_->devices().find(ieee) == nullptr) {
		decline(request, response, 404, "zigbee.unknown_device", "no device with this IEEE address");
		return;
	}
	bool done = false;
	if (op[0] == '\0') {
		if (!method_is(request, response, HttpMethod::del)) {
			return;
		}
		if (!zigbee_->remove(ieee, &error)) {
			decline(request, response, 409, "zigbee.refused", error);
			return;
		}
		Log::info(kTag, "Zigbee device %s removed over the API from %s", text, request.source);
		ok(response);
		return;
	}
	if (std::strcmp(op, "identify") == 0) {
		if (!method_is(request, response, HttpMethod::post)) {
			return;
		}
		done = zigbee_->identify(ieee, &error);
	} else if (std::strcmp(op, "interview") == 0) {
		if (!method_is(request, response, HttpMethod::post)) {
			return;
		}
		done = zigbee_->interview(ieee, &error);
	} else if (std::strcmp(op, "label") == 0) {
		if (!method_is(request, response, HttpMethod::put)) {
			return;
		}
		JsonSpan body;
		JsonSpan member;
		if (!parse_body(request, response, body)) {
			return;
		}
		char label[zigbee::ZigbeeDevice::kMaxText * 2 + 2] = {};
		if (!json_member(body, "label", member) || !json_string(member, label, sizeof(label)) ||
			std::strlen(label) > zigbee::ZigbeeDevice::kMaxText) {
			decline(request, response, 422, "zigbee.invalid", "label is a string of at most 32 bytes (\"\" clears it)");
			return;
		}
		done = zigbee_->set_label(ieee, label, &error);
	} else {
		refuse(response, 404, "api.not_found", "no such API endpoint");
		return;
	}
	if (!done) {
		decline(request, response, 409, "zigbee.refused", error);
		return;
	}
	const zigbee::DeviceDatabase& devices = zigbee_->devices();
	JsonWriter json(response.body, response.capacity);
	json.begin_object().member("ok", true);
	for (size_t i = 0; i < devices.size(); ++i) {
		if (devices.at(i).ieee == ieee) {
			json.key("device");
			zigbee_->write_device_json(json, i);
		}
	}
	json.end_object();
	finish(response, json);
}

void WebApi::zigbee_list(const WebRequest& request, WebResponse& response) {
	const zigbee::DeviceDatabase& devices = zigbee_->devices();
	uint32_t from = 0;
	query_uint(request.query, "from", from);
	JsonWriter json(response.body, response.capacity);
	json.reserve(96);
	json.begin_object()
		.member("now_ms", controller_.clock().monotonic_ms())
		.member("generation", zigbee_->generation())
		.member("loaded", zigbee_->loaded());
	json.key("interviewing");
	if (zigbee_->interviewing() != 0) {
		char ieee[zigbee::kIeeeText];
		zigbee::format_ieee(zigbee_->interviewing(), ieee);
		json.string(ieee);
	} else {
		json.null();
	}
	json.key("network").begin_object();
	zigbee_->write_network_json(json);
	json.end_object();
	json.member("count", static_cast<uint32_t>(devices.size()))
		.member("max_devices", static_cast<uint32_t>(zigbee::DeviceDatabase::kMaxDevices))
		.member("from", from);
	json.key("devices").begin_array();
	size_t index = from;
	size_t written = 0;
	for (; index < devices.size() && written < kZigbeePageSize; ++index) {
		JsonWriter::Mark mark = json.mark();
		zigbee_->write_device_json(json, index);
		if (json.overflowed()) {
			json.reset(mark);
			break;
		}
		++written;
	}
	if (written == 0 && index < devices.size()) {
		refuse(response, 500, "api.response_too_large", "a device does not fit the server's buffer");
		return;
	}
	json.release();
	json.end_array();
	json.key("next");
	if (index < devices.size()) {
		json.number(static_cast<uint32_t>(index));
	} else {
		json.null();
	}
	json.end_object();
	finish(response, json);
}

/// The export for E-Home's importer (spec/zigbee.md "Export"): the device objects of
/// write_device_export_json(), paged like the list; the UI joins the pages into one file.
void WebApi::zigbee_export(const WebRequest& request, WebResponse& response) {
	const zigbee::DeviceDatabase& devices = zigbee_->devices();
	uint32_t from = 0;
	query_uint(request.query, "from", from);
	JsonWriter json(response.body, response.capacity);
	json.reserve(96);
	json.begin_object()
		.member("format", "homecontrol-zigbee-export")
		.member("version", static_cast<uint32_t>(1))
		.member("controller", host_.device_name())
		.member("exported", static_cast<int64_t>(controller_.clock().wall_time().seconds));
	json.key("network").begin_object();
	zigbee_->write_network_json(json);
	json.end_object();
	json.member("count", static_cast<uint32_t>(devices.size()))
		.member("max_devices", static_cast<uint32_t>(zigbee::DeviceDatabase::kMaxDevices))
		.member("from", from);
	json.key("devices").begin_array();
	size_t index = from;
	size_t written = 0;
	for (; index < devices.size() && written < kZigbeePageSize; ++index) {
		JsonWriter::Mark mark = json.mark();
		zigbee_->write_device_export_json(json, index);
		if (json.overflowed()) {
			json.reset(mark);
			break;
		}
		++written;
	}
	if (written == 0 && index < devices.size()) {
		refuse(response, 500, "api.response_too_large", "a device's export does not fit the server's buffer");
		return;
	}
	json.release();
	json.end_array();
	json.key("next");
	if (index < devices.size()) {
		json.number(static_cast<uint32_t>(index));
	} else {
		json.null();
	}
	json.end_object();
	finish(response, json);
}

// ---- External values, language packs, MQTT ---------------------------------------------------------

void WebApi::external_fetch(const char* rest, const WebRequest& request, WebResponse& response) {
	const char* slash = std::strchr(rest, '/');
	char id[48];
	if (slash == nullptr || std::strcmp(slash, "/fetch") != 0 ||
		!decode_segment(rest, static_cast<size_t>(slash - rest), id, sizeof(id))) {
		refuse(response, 404, "api.not_found", "no such API endpoint (POST /api/external/<id>/fetch)");
		return;
	}
	if (!method_is(request, response, HttpMethod::post)) {
		return;
	}
	if (external_ == nullptr) {
		decline(request, response, 501, "system.unavailable", "this build has no external values");
		return;
	}
	bool known = false;
	for (size_t i = 0; i < external_->size() && !known; ++i) {
		ExternalSourceStatus status;
		known = external_->status(i, status) && std::strcmp(status.id, id) == 0;
	}
	if (!known) {
		decline(request, response, 404, "external.unknown", "the runtime image has no external source with this id");
		return;
	}
	const char* error = nullptr;
	if (!external_->fetch_now(id, &error)) {
		decline(request, response, 409, "external.refused", error);
		return;
	}
	Log::info(kTag, "fetch of external source %s requested over the API", id);
	ok(response);
}

void WebApi::lang_sync(const WebRequest& request, WebResponse& response) {
	if (!method_is(request, response, HttpMethod::post)) {
		return;
	}
	if (packs_ == nullptr) {
		decline(request, response, 501, "system.unavailable", "this build downloads no language packs");
		return;
	}
	const char* error = nullptr;
	if (!packs_->sync_now(&error)) {
		decline(request, response, 409, "lang.busy", error);
		return;
	}
	ok(response);
}

void WebApi::mqtt_discovery(const WebRequest& request, WebResponse& response) {
	if (!method_is(request, response, HttpMethod::post)) {
		return;
	}
	if (mqtt_ == nullptr) {
		decline(request, response, 501, "system.unavailable", "this build has no MQTT client");
		return;
	}
	const char* error = nullptr;
	if (!mqtt_->publish_discovery(&error)) {
		decline(request, response, 409, "mqtt.refused", error);
		return;
	}
	Log::info(kTag, "MQTT discovery publish requested over the API");
	ok(response);
}

// ---- The status members of the console's status commands ----------------------------------------

void WebApi::write_status_details(JsonWriter& json) {
	uint64_t now = controller_.clock().monotonic_ms();
	uint64_t total = 0;
	uint64_t used = 0;
	bool usage = host_.storage_usage(total, used);
	if (usage || blobs_ != nullptr) {
		json.key("storage").begin_object();
		if (usage) {
			json.member("total", total).member("used", used).member("free", total > used ? total - used : 0);
		}
		if (blobs_ != nullptr) {
			BlobCounter counter;
			blobs_->visit(counter);
			json.member("files", counter.files).member("bytes", counter.bytes);
		}
		json.end_object();
	}
	if (zigbee_ != nullptr) {
		// The coordinator's summary; a changed generation says the device list changed.
		json.key("zigbee").begin_object()
			.member("up", zigbee_->network().up)
			.member("permit_join_s", zigbee_->permit_join_s())
			.member("devices", static_cast<uint32_t>(zigbee_->devices().size()))
			.member("generation", zigbee_->generation())
			.end_object();
	}
	if (knx_ != nullptr) {
		char text[knx::GatewayAddress::kMaxText];
		json.key("knx").begin_object()
			.member("state", knx::tunnel_state_name(knx_->state()))
			.member("reason", knx_->reason())
			.member("gateway", knx_->gateway().empty() ? "" : knx_->gateway().format(text, sizeof(text)))
			.member("nat", knx_->nat());
		if (knx_->is_up()) {
			char address[knx::IndividualAddress::kMaxText];
			json.member("channel", static_cast<uint32_t>(knx_->channel()))
				.member("address", knx_->address().format(address, sizeof(address)));
		}
		json.member("queued", static_cast<uint32_t>(knx_->queued()));
		if (telegrams_ != nullptr) {
			json.member("telegrams", telegrams_->total());
		}
		json.end_object();
	}
	if (external_ != nullptr) {
		json.key("external").begin_object().key("sources").begin_array();
		for (size_t i = 0; i < external_->size(); ++i) {
			ExternalSourceStatus s;
			if (!external_->status(i, s)) {
				continue;
			}
			json.begin_object()
				.member("id", s.id)
				.member("preset", s.preset)
				.member("interval_s", s.interval_s)
				.member("fetches", s.fetches)
				.member("failures", s.failures)
				.member("fetching", s.fetching);
			if (s.last_ok_ms != 0) {
				json.member("last_ok_ago_s", (now - s.last_ok_ms) / 1000);
			}
			json.member("error", s.last_error).member("next_s", s.next_ms > now ? (s.next_ms - now) / 1000 : 0);
			json.end_object();
		}
		json.end_array().end_object();
	}
	if (packs_ != nullptr) {
		json.key("lang").begin_object();
		packs_->write_json(json);
		json.end_object();
	}
	if (mqtt_ != nullptr) {
		MqttStatus s;
		mqtt_->status(s);
		json.key("mqtt").begin_object()
			.member("state", mqtt_state_name(s.state))
			.member("configured", s.broker[0] != '\0');
		if (s.broker[0] != '\0') {
			json.member("broker", s.broker)
				.member("base_topic", s.base_topic)
				.member("discovery", s.discovery)
				.member("functions", static_cast<uint32_t>(s.functions))
				.member("published", s.published)
				.member("commands", s.commands)
				.member("refusals", s.refusals)
				.member("connects", s.connects)
				.member("failures", s.failures)
				.member("error", s.last_error);
			if (s.next_attempt_ms != 0) {
				json.member("next_s", s.next_attempt_ms > now ? (s.next_attempt_ms - now) / 1000 : 0);
			}
		}
		json.end_object();
	}
}

} // namespace hc
