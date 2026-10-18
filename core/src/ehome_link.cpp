/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/ehome_link.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

#include "hc/log.h"
#include "hc/sha.h"
#include "hc/version.h"
#include "hc/websocket.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.ehome";

bool refuse(const char** error, const char* text) {
	if (error != nullptr) {
		*error = text;
	}
	return false;
}

bool is_hex(char c) {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool starts_with(const char* text, const char* prefix) {
	return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

/// Printable ASCII without space, quotes, backslash, angle brackets, braces, '|', '^', '`', and
/// without '?' and '#' (no query or fragment).
bool url_char(char c) {
	if (c <= 0x20 || c >= 0x7f) {
		return false;
	}
	return std::strchr("\"\\<>{}|^`?#", c) == nullptr;
}

/// Checks "<scheme>host[:port][/path]" after the scheme: a host, URL characters, no user info.
bool url_rest_valid(const char* rest, const char** error) {
	if (*rest == '\0' || *rest == '/' || *rest == ':') {
		return refuse(error, "the address has no host");
	}
	for (const char* p = rest; *p != '\0'; ++p) {
		if (!url_char(*p)) {
			return refuse(error, "the address contains a space, a quote, a query or another character a URL may not");
		}
		if (*p == '@') {
			return refuse(error, "the address may not carry a user name");
		}
	}
	return true;
}

void copy(char* out, size_t size, const char* text) {
	std::snprintf(out, size, "%s", text != nullptr ? text : "");
}

/// A string member of `object` into `out`; false if absent, not a string or too long.
bool string_member(const JsonSpan& object, const char* name, char* out, size_t size) {
	JsonSpan span;
	return json_member(object, name, span) && json_string(span, out, size);
}

} // namespace

// ---- Validators -------------------------------------------------------------------------------

bool ehome_url_valid(const char* url, const char** error) {
	if (url == nullptr || url[0] == '\0') {
		return refuse(error, "the E-Home address is empty");
	}
	if (std::strlen(url) > Settings::kMaxStringLength) {
		return refuse(error, "the E-Home address is longer than 127 bytes");
	}
	if (starts_with(url, "https://")) {
		return url_rest_valid(url + 8, error);
	}
	if (starts_with(url, "http://")) {
		return url_rest_valid(url + 7, error);
	}
	return refuse(error, "the E-Home address starts with https:// (http:// only for development)");
}

bool ehome_socket_valid(const char* socket, const char* url, const char** error) {
	if (socket == nullptr || socket[0] == '\0') {
		return refuse(error, "the socket address is empty");
	}
	if (std::strlen(socket) > Settings::kMaxStringLength) {
		return refuse(error, "the socket address is longer than 127 bytes");
	}
	if (starts_with(socket, "wss://")) {
		return url_rest_valid(socket + 6, error);
	}
	if (starts_with(socket, "ws://")) {
		if (url == nullptr || !starts_with(url, "http://")) {
			return refuse(error, "a plain ws:// socket goes only with an http:// E-Home address (development)");
		}
		return url_rest_valid(socket + 5, error);
	}
	return refuse(error, "the socket address is not a wss:// URL");
}

bool ehome_id_valid(const char* id, const char** error) {
	size_t length = id != nullptr ? std::strlen(id) : 0;
	if (length == 0 || length > 64) {
		return refuse(error, "the controller id has 1 to 64 characters");
	}
	for (size_t i = 0; i < length; ++i) {
		char c = id[i];
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
			c == '-';
		if (!ok) {
			return refuse(error, "the controller id may contain only letters, digits, '.', '_' and '-'");
		}
	}
	return true;
}

bool ehome_secret_valid(const char* secret, const char** error) {
	if (secret == nullptr || std::strlen(secret) != 64) {
		return refuse(error, "the secret is not 64 hex digits");
	}
	for (size_t i = 0; i < 64; ++i) {
		if (!is_hex(secret[i])) {
			return refuse(error, "the secret is not 64 hex digits");
		}
	}
	return true;
}

bool ehome_code_valid(const char* code, const char** error) {
	size_t length = code != nullptr ? std::strlen(code) : 0;
	if (length == 0 || length > 32) {
		return refuse(error, "the enrollment code has 1 to 32 characters");
	}
	for (size_t i = 0; i < length; ++i) {
		char c = code[i];
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
		if (!ok) {
			return refuse(error, "the enrollment code consists of letters, digits and '-'");
		}
	}
	return true;
}

bool ehome_mac(const char* secret, const char* nonce, char out[65]) {
	out[0] = '\0';
	if (!ehome_secret_valid(secret)) {
		return false;
	}
	size_t digits = nonce != nullptr ? std::strlen(nonce) : 0;
	if (digits < 32 || digits > 128 || digits % 2 != 0) {
		return false;
	}
	uint8_t key[32];
	uint8_t data[64];
	if (!from_hex(secret, key, sizeof(key)) || !from_hex(nonce, data, digits / 2)) {
		std::memset(key, 0, sizeof(key));
		return false;
	}
	uint8_t mac[Sha256::kDigestSize];
	hmac_sha256(key, sizeof(key), data, digits / 2, mac);
	std::memset(key, 0, sizeof(key));
	to_hex(mac, sizeof(mac), out);
	return true;
}

bool ehome_enrolled(const Settings& settings) {
	char text[Settings::kMaxStringLength + 1];
	char url[Settings::kMaxStringLength + 1];
	if (settings.get_string(setting::kEhomeUrl, url, sizeof(url)) != SettingsStatus::ok || !ehome_url_valid(url)) {
		return false;
	}
	if (settings.get_string(setting::kEhomeId, text, sizeof(text)) != SettingsStatus::ok || !ehome_id_valid(text)) {
		return false;
	}
	if (settings.get_string(setting::kEhomeSocket, text, sizeof(text)) != SettingsStatus::ok ||
		!ehome_socket_valid(text, url)) {
		return false;
	}
	bool secret = settings.get_string(setting::kEhomeSecret, text, sizeof(text)) == SettingsStatus::ok &&
		ehome_secret_valid(text);
	std::memset(text, 0, sizeof(text));
	return secret;
}

const char* ehome_state_name(EhomeState state) {
	switch (state) {
		case EhomeState::off: return "off";
		case EhomeState::connecting: return "connecting";
		case EhomeState::connected: return "connected";
		case EhomeState::failed: return "failed";
	}
	return "off";
}

// ---- The link ---------------------------------------------------------------------------------

EhomeLink::EhomeLink(WebApi& api, LiveFeed& feed, WebSocketClient& client, HttpFetcher& fetcher)
	: api_(api),
	  feed_(feed),
	  client_(client),
	  fetcher_(fetcher),
	  loop_(api.controller().loop()),
	  backoff_(kBackoffInitialMs, kBackoffCapMs) {}

EhomeLink::~EhomeLink() {
	loop_.cancel(tick_timer_);
	loop_.cancel(retry_timer_);
	loop_.cancel(enroll_timer_);
	loop_.cancel(stream_timer_);
	if (api_.ehome() == this) {
		api_.set_ehome(nullptr);
	}
	free_answer();
	delete[] body_;
	delete[] out_;
}

void EhomeLink::reload() {
	Settings& settings = api_.controller().settings();
	enrolled_ = ehome_enrolled(settings);
	bool on = false;
	enabled_ = settings.get_bool(setting::kEhomeEnabled, on) == SettingsStatus::ok && on;
	if (settings.get_string(setting::kEhomeUrl, url_, sizeof(url_)) != SettingsStatus::ok) {
		url_[0] = '\0';
	}
	if (settings.get_string(setting::kEhomeName, installation_, sizeof(installation_)) != SettingsStatus::ok) {
		installation_[0] = '\0';
	}
}

void EhomeLink::start() {
	api_.set_ehome(this);
	since_ms_ = api_.controller().clock().monotonic_ms();
	tick_timer_ = loop_.schedule_periodic(since_ms_ + kTickMs, kTickMs, Callback::member<EhomeLink, &EhomeLink::tick>(this));
	if (!tick_timer_.valid()) {
		Log::error(kTag, "no timer for the E-Home connection: it will not connect");
	}
	reload();
	if (!enrolled_) {
		Log::info(kTag, "not enrolled with E-Home (Settings page: Connect to E-Home)");
		return;
	}
	Log::info(kTag, "enrolled with %s (installation \"%s\"); the connection is switched %s", url_, installation_,
		enabled_ ? "on" : "off");
	if (enabled_) {
		attempt();
	}
}

void EhomeLink::set_state(EhomeState state) {
	if (state != state_) {
		state_ = state;
		since_ms_ = api_.controller().clock().monotonic_ms();
	}
}

void EhomeLink::go_idle(const char* why) {
	loop_.cancel(retry_timer_);
	retry_timer_ = TimerHandle();
	if (phase_ != Phase::idle) {
		stop_socket(websocket::kCloseNormal, why);
		phase_ = Phase::idle;
		Log::info(kTag, "connection closed: %s", why);
	}
	error_[0] = '\0';
	last_logged_[0] = '\0';
	backoff_.reset();
	set_state(EhomeState::off);
}

void EhomeLink::enabled_changed() {
	reload();
	if (enabled_ && enrolled_) {
		if (phase_ == Phase::idle || phase_ == Phase::backoff) {
			backoff_.reset();
			error_[0] = '\0';
			last_logged_[0] = '\0';
			attempt();
		}
		return;
	}
	go_idle(enrolled_ ? "switched off by the owner" : "not enrolled");
}

void EhomeLink::attempt() {
	if (!enabled_ || !enrolled_) {
		go_idle(!enrolled_ ? "not enrolled" : "switched off by the owner");
		return;
	}
	uint64_t now = api_.controller().clock().monotonic_ms();
	if (!api_.controller().connectivity().is_up()) {
		if (phase_ != Phase::waiting) {
			Log::info(kTag, "waiting for the network");
		}
		phase_ = Phase::waiting;
		set_state(EhomeState::connecting);
		return;
	}
	char socket[Settings::kMaxStringLength + 1];
	if (api_.controller().settings().get_string(setting::kEhomeSocket, socket, sizeof(socket)) != SettingsStatus::ok) {
		fail("the socket address (setting %s) is unreadable", setting::kEhomeSocket);
		return;
	}
	const char* error = nullptr;
	phase_ = Phase::opening;
	phase_ms_ = now;
	set_state(EhomeState::connecting);
	if (out_ == nullptr) {
		out_ = new (std::nothrow) char[kOutBytes];
		if (out_ == nullptr) {
			fail("cannot connect: no memory for the response buffer (%u bytes)", static_cast<unsigned>(kOutBytes));
			return;
		}
	}
	if (!client_.connect(socket, *this, &error)) {
		fail("cannot connect: %s", error != nullptr ? error : "the client refused");
		return;
	}
	socket_ = true;
	Log::info(kTag, "connecting to %s", socket);
}

void EhomeLink::stop_socket(uint16_t code, const char* reason) {
	drop_request();
	drop_pending_key("the connection ended");
	feed_.detach_ehome();
	if (socket_) {
		socket_ = false;
		client_.close(code, reason);
	}
	free_out();
}

void EhomeLink::free_out() {
	delete[] out_;
	out_ = nullptr;
}

void EhomeLink::fail(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(error_, sizeof(error_), fmt, args);
	va_end(args);
	stop_socket(websocket::kCloseGoingAway, error_);
	uint32_t delay = backoff_.next();
	phase_ = Phase::backoff;
	retry_at_ms_ = api_.controller().clock().monotonic_ms() + delay;
	loop_.cancel(retry_timer_);
	retry_timer_ = loop_.schedule_at(retry_at_ms_, Callback::member<EhomeLink, &EhomeLink::retry>(this));
	set_state(EhomeState::failed);
	if (std::strcmp(last_logged_, error_) == 0) {
		Log::info(kTag, "%s; retry in %u s (failure %u)", error_, static_cast<unsigned>(delay / 1000),
			static_cast<unsigned>(backoff_.failures()));
	} else {
		Log::warn(kTag, "%s; retry in %u s (failure %u)", error_, static_cast<unsigned>(delay / 1000),
			static_cast<unsigned>(backoff_.failures()));
		copy(last_logged_, sizeof(last_logged_), error_);
	}
}

void EhomeLink::retry() {
	retry_timer_ = TimerHandle();
	if (phase_ == Phase::backoff) {
		attempt();
	}
}

void EhomeLink::tick() {
	uint64_t now = api_.controller().clock().monotonic_ms();
	switch (phase_) {
		case Phase::idle:
			break;
		case Phase::waiting:
			if (api_.controller().connectivity().is_up()) {
				attempt();
			}
			break;
		case Phase::backoff:
			if (!retry_timer_.valid() && now >= retry_at_ms_) {
				attempt(); // the retry timer could not be scheduled
			}
			break;
		case Phase::opening:
		case Phase::challenge:
		case Phase::authenticating:
			if (now - phase_ms_ >= kHandshakeTimeoutMs) {
				fail("no %s from E-Home within %u s",
					phase_ == Phase::opening ? "connection" : phase_ == Phase::challenge ? "challenge" : "ready",
					static_cast<unsigned>(kHandshakeTimeoutMs / 1000));
			}
			break;
		case Phase::connected:
			if (pong_due_ms_ != 0 && now >= pong_due_ms_) {
				fail("no pong from E-Home within %u s", static_cast<unsigned>(kPongTimeoutMs / 1000));
			} else if (now >= ping_at_ms_) {
				ping_at_ms_ = now + kPingMs;
				if (!client_.send_ping()) {
					fail("cannot send the ping");
				} else if (pong_due_ms_ == 0) {
					pong_due_ms_ = now + kPongTimeoutMs;
				}
			}
			break;
	}
}

// ---- The socket -------------------------------------------------------------------------------

void EhomeLink::ws_client_opened() {
	if (phase_ != Phase::opening) {
		return;
	}
	phase_ = Phase::challenge;
	Log::info(kTag, "socket open, awaiting E-Home's challenge");
}

void EhomeLink::ws_client_closed(const char* reason) {
	socket_ = false;
	switch (phase_) {
		case Phase::opening:
			fail("cannot connect: %s", reason);
			break;
		case Phase::challenge:
		case Phase::authenticating:
			fail("closed during the authentication: %s", reason);
			break;
		case Phase::connected:
			fail("connection lost: %s", reason);
			break;
		default:
			break;
	}
}

void EhomeLink::ws_client_pong() {
	pong_due_ms_ = 0;
}

bool EhomeLink::send_text(const char* text, size_t length) {
	return socket_ && client_.send_text(text, length);
}

void EhomeLink::ws_client_text(const char* text, size_t length) {
	if (phase_ != Phase::challenge && phase_ != Phase::authenticating && phase_ != Phase::connected) {
		return;
	}
	JsonSpan message;
	JsonSpan type;
	if (!json_parse(text, length, message) || message.kind != JsonSpan::Kind::object ||
		!json_member(message, "type", type) || type.kind != JsonSpan::Kind::string) {
		if (phase_ != Phase::connected) {
			fail("E-Home sent a malformed message during the authentication");
		} else {
			feed_.ehome_message(text, length); // the feed answers ws.bad_message
		}
		return;
	}
	if (json_string_equals(type, "refused")) {
		char code[48] = "?";
		char reason[96] = "no reason given";
		string_member(message, "code", code, sizeof(code));
		string_member(message, "error", reason, sizeof(reason));
		fail("E-Home refused the connection: %s (%s)", reason, code);
		return;
	}
	if (phase_ != Phase::connected) {
		handshake_message(message, type);
		return;
	}
	if (json_string_equals(type, "request")) {
		request(message);
		return;
	}
	if (json_string_equals(type, "key")) {
		key_message(message);
		return;
	}
	feed_.ehome_message(text, length);
}

void EhomeLink::handshake_message(const JsonSpan& message, const JsonSpan& type) {
	Settings& settings = api_.controller().settings();
	if (phase_ == Phase::challenge && json_string_equals(type, "challenge")) {
		char nonce[132];
		char id[Settings::kMaxStringLength + 1];
		char secret[Settings::kMaxStringLength + 1];
		char mac[65];
		if (!string_member(message, "nonce", nonce, sizeof(nonce))) {
			fail("E-Home's challenge carries no nonce");
			return;
		}
		bool ok = settings.get_string(setting::kEhomeId, id, sizeof(id)) == SettingsStatus::ok &&
			settings.get_string(setting::kEhomeSecret, secret, sizeof(secret)) == SettingsStatus::ok &&
			ehome_mac(secret, nonce, mac);
		std::memset(secret, 0, sizeof(secret));
		if (!ok) {
			fail("cannot answer E-Home's challenge (a nonce of 32 to 128 hex digits, and the stored secret)");
			return;
		}
		char auth[320];
		char fingerprint[kFingerprintSize];
		uint8_t key[kEd25519KeySize];
		bool has_key = load_installation_key(settings, key) == KeyState::ok;
		if (has_key) {
			key_fingerprint(key, fingerprint);
		}
		JsonWriter json(auth, sizeof(auth));
		json.begin_object()
			.member("type", "auth")
			.member("controller", id)
			.member("mac", mac)
			.member("version", kVersion)
			.member("build", build_id());
		if (has_key) {
			json.member("key", fingerprint); // the installation key the device holds
		}
		json.end_object();
		bool sent = !json.overflowed() && send_text(auth, json.length());
		std::memset(mac, 0, sizeof(mac));
		std::memset(auth, 0, sizeof(auth));
		if (!sent) {
			fail("cannot send the authentication");
			return;
		}
		phase_ = Phase::authenticating;
		Log::info(kTag, "challenge answered as controller %s", id);
		return;
	}
	if (phase_ == Phase::authenticating && json_string_equals(type, "ready")) {
		char name[Settings::kMaxStringLength + 1] = {};
		if (string_member(message, "installation", name, sizeof(name)) && name[0] != '\0' &&
			Settings::check_string(name) == SettingsStatus::ok && std::strcmp(name, installation_) != 0) {
			if (settings.set_string(setting::kEhomeName, name) == SettingsStatus::ok) {
				Log::info(kTag, "the installation is called \"%s\" at E-Home now (was \"%s\")", name, installation_);
				copy(installation_, sizeof(installation_), name);
			}
		}
		uint64_t now = api_.controller().clock().monotonic_ms();
		phase_ = Phase::connected;
		set_state(EhomeState::connected);
		backoff_.reset();
		error_[0] = '\0';
		last_logged_[0] = '\0';
		ping_at_ms_ = now + kPingMs;
		pong_due_ms_ = 0;
		Log::info(kTag, "connected to E-Home (installation \"%s\")", installation_);
		feed_.attach_ehome(*this, kFeedHandle);
		return;
	}
	char name[24] = "?";
	json_string(type, name, sizeof(name));
	fail("E-Home sent \"%s\" while the controller awaited the %s", name,
		phase_ == Phase::challenge ? "challenge" : "ready");
}

// ---- The feed's transport ---------------------------------------------------------------------

bool EhomeLink::ws_send(uint32_t client, const char* text, size_t length) {
	(void)client;
	return phase_ == Phase::connected && send_text(text, length);
}

void EhomeLink::ws_close(uint32_t client, uint16_t code, const char* reason) {
	(void)client;
	(void)code;
	if (phase_ == Phase::connected) {
		fail("the live feed closed the E-Home client: %s", reason);
	}
}

// ---- Requests ---------------------------------------------------------------------------------

void EhomeLink::drop_request() {
	has_signature_ = false;
	delete[] body_;
	body_ = nullptr;
	body_length_ = 0;
	body_have_ = 0;
	discard_ = 0;
	busy_ = false;
	streaming_ = false;
	loop_.cancel(stream_timer_);
	stream_timer_ = TimerHandle();
}

void EhomeLink::answer(const char* id, int status, const char* code, const char* text) {
	char buffer[512];
	int n = std::snprintf(buffer, sizeof(buffer), "{\"type\":\"response\",\"id\":%s,\"status\":%d,\"body\":", id, status);
	if (n <= 0 || static_cast<size_t>(n) >= sizeof(buffer)) {
		return;
	}
	JsonWriter json(buffer + n, sizeof(buffer) - static_cast<size_t>(n) - 1);
	json.begin_object().member("error", text).member("code", code).end_object();
	if (json.overflowed()) {
		return;
	}
	size_t length = static_cast<size_t>(n) + json.length();
	buffer[length++] = '}';
	if (!send_text(buffer, length)) {
		Log::warn(kTag, "the answer to request %s was not sent (%s)", id, code);
	}
}

void EhomeLink::request(const JsonSpan& message) {
	JsonSpan span;
	char id[sizeof(request_id_)];
	if (!json_member(message, "id", span) ||
		(span.kind != JsonSpan::Kind::number && span.kind != JsonSpan::Kind::string) || span.length >= sizeof(id)) {
		static const char kError[] = "{\"type\":\"error\",\"error\":\"a request needs an \\\"id\\\" (a number or a "
			"string of at most 32 bytes)\",\"code\":\"ehome.bad_request\"}";
		Log::warn(kTag, "E-Home sent a request without a usable id: refused");
		send_text(kError, sizeof(kError) - 1);
		return;
	}
	std::memcpy(id, span.data, span.length);
	id[span.length] = '\0';
	JsonSpan length_span;
	bool has_length = json_member(message, "length", length_span);
	if (busy_) {
		Log::warn(kTag, "request %s refused: request %s is still running", id, request_id_);
		answer(id, 429, "ehome.busy", "one request at a time: wait for the response to the running one");
		if (has_length) {
			fail("E-Home announced a body while request %s was running (its frames cannot be told apart)",
				request_id_);
		}
		return;
	}
	char method[8] = {};
	JsonSpan member;
	HttpMethod m = HttpMethod::other;
	if (json_member(message, "method", member) && json_string(member, method, sizeof(method))) {
		m = std::strcmp(method, "GET") == 0 ? HttpMethod::get
			: std::strcmp(method, "POST") == 0 ? HttpMethod::post
			: std::strcmp(method, "PUT") == 0 ? HttpMethod::put
			: std::strcmp(method, "DELETE") == 0 ? HttpMethod::del : HttpMethod::other;
	}
	double length = 0;
	const char* refusal = nullptr;
	if (m == HttpMethod::other) {
		refusal = "\"method\" is GET, POST, PUT or DELETE";
	} else if (!json_member(message, "path", member) || !json_string(member, path_, sizeof(path_) - 1) ||
		!starts_with(path_, "/api/")) {
		refusal = "\"path\" is an API path (/api/..., at most 254 bytes)";
	} else if (has_length && (!json_number(length_span, length) || length < 0 || length > 4294967295.0 ||
		length != static_cast<double>(static_cast<uint64_t>(length)))) {
		refusal = "\"length\" is the body's size in bytes";
	}
	if (refusal != nullptr) {
		Log::warn(kTag, "request %s refused: %s", id, refusal);
		answer(id, 400, "ehome.bad_request", refusal);
		if (has_length && json_number(length_span, length) && length > 0 && length <= 4294967295.0) {
			discard_ = static_cast<size_t>(length);
		}
		return;
	}
	std::memcpy(request_id_, id, sizeof(request_id_));
	method_ = m;
	has_signature_ = json_member(message, "signature", member);
	if (has_signature_ && !json_string(member, signature_, sizeof(signature_))) {
		copy(signature_, sizeof(signature_), "invalid"); // not a string, or too long: the API refuses it
	}
	char* q = std::strchr(path_, '?');
	if (q != nullptr) {
		*q = '\0';
		query_ = q + 1;
	} else {
		query_ = path_ + std::strlen(path_); // ""
	}
	if (has_length && length > 0) {
		size_t bytes = static_cast<size_t>(length);
		size_t limit = WebApi::body_limit(method_, path_);
		if (bytes > limit) {
			char text[64];
			std::snprintf(text, sizeof(text), "the request body is longer than %u bytes", static_cast<unsigned>(limit));
			Log::warn(kTag, "request %s %s refused: %s", method, path_, text);
			answer(id, 413, "api.body_too_large", text);
			discard_ = bytes;
			return;
		}
		body_ = new (std::nothrow) char[bytes + 1];
		if (body_ == nullptr) {
			Log::warn(kTag, "request %s %s refused: no heap block of %u bytes for its body", method, path_,
				static_cast<unsigned>(bytes));
			answer(id, 507, "api.no_memory", "the device has not enough free memory for this body");
			discard_ = bytes;
			return;
		}
		body_length_ = bytes;
		body_have_ = 0;
		busy_ = true;
		Log::info(kTag, "request %s: %s %s, receiving %u bytes", id, method, path_, static_cast<unsigned>(bytes));
		return; // dispatched when the last byte arrived (ws_client_binary)
	}
	JsonSpan body;
	bool inline_body = json_member(message, "body", body);
	if (inline_body && has_signature_ && method_ == HttpMethod::put && std::strcmp(path_, "/api/image") == 0) {
		// The inline body is a span of parsed JSON, not the exported file's bytes the signature covers.
		static const char kText[] = "a signed image is sent with length and binary frames: the signature covers its "
			"exact bytes";
		Log::warn(kTag, "request %s PUT /api/image refused: %s", id, kText);
		answer(id, 400, "image.bad_request", kText);
		has_signature_ = false;
		return;
	}
	busy_ = true;
	if (inline_body) {
		// The body is a span of the message, valid during this call: dispatch now.
		WebRequest req;
		req.method = method_;
		req.path = path_;
		req.query = query_;
		req.requested_with = WebApi::kCsrfHeaderValue;
		req.source = "E-Home";
		req.ehome = true;
		req.image_signature = has_signature_ ? signature_ : nullptr;
		req.body = body.data;
		req.body_length = body.length;
		WebResponse response(out_ + kPrefix, kMaxResponseBody);
		api_.handle(req, response);
		send_response(response);
		return;
	}
	dispatch();
}

void EhomeLink::ws_client_binary(const uint8_t* data, size_t length, bool last) {
	(void)last; // a body may span several binary messages: only the count matters
	if (phase_ != Phase::connected) {
		return;
	}
	if (discard_ > 0) {
		size_t take = length < discard_ ? length : discard_;
		discard_ -= take;
		data += take;
		length -= take;
		if (length == 0) {
			return;
		}
	}
	if (body_ == nullptr) {
		fail("E-Home sent a binary frame no request announced");
		return;
	}
	if (length > body_length_ - body_have_) {
		fail("E-Home sent more body bytes than request %s announced (%u)", request_id_,
			static_cast<unsigned>(body_length_));
		return;
	}
	std::memcpy(body_ + body_have_, data, length);
	body_have_ += length;
	if (body_have_ == body_length_) {
		body_[body_length_] = '\0';
		dispatch();
	}
}

void EhomeLink::dispatch() {
	WebRequest req;
	req.method = method_;
	req.path = path_;
	req.query = query_;
	req.requested_with = WebApi::kCsrfHeaderValue;
	req.source = "E-Home";
	req.ehome = true;
	req.image_signature = has_signature_ ? signature_ : nullptr;
	req.body = body_;
	req.body_length = body_length_;
	WebResponse response(out_ + kPrefix, kMaxResponseBody);
	api_.handle(req, response);
	delete[] body_; // the routes are done with it when handle() returns (as with the HTTP servers' buffers)
	body_ = nullptr;
	body_length_ = 0;
	body_have_ = 0;
	send_response(response);
}

void EhomeLink::send_response(WebResponse& response) {
	if (response.deferred || response.needs_hash) {
		// Only the enrollment defers and only a login hashes: both are refused to E-Home before.
		WebApi::refuse(response, 500, "ehome.unsupported", "this route cannot answer over the E-Home connection");
	}
	if (method_ != HttpMethod::get) {
		Log::info(kTag, "request %s: %s → %d", request_id_, path_, response.status);
	} else {
		Log::debug(kTag, "request %s: GET %s → %d", request_id_, path_, response.status);
	}
	if (response.stream) {
		char header[256];
		JsonWriter json(header, sizeof(header));
		json.begin_object().member("type", "response").key("id").raw(request_id_, std::strlen(request_id_))
			.member("status", static_cast<int32_t>(response.status))
			.member("length", static_cast<uint64_t>(response.stream_length))
			.member("content_type", response.content_type)
			.end_object();
		if (json.overflowed() || !send_text(header, json.length())) {
			Log::warn(kTag, "the answer to request %s was not sent", request_id_);
			busy_ = false;
			return;
		}
		streaming_ = true;
		stream_token_ = response.stream_token;
		stream_length_ = response.stream_length;
		stream_sent_ = 0;
		schedule_stream();
		return;
	}
	char prefix[kPrefix];
	int n = std::snprintf(prefix, sizeof(prefix), "{\"type\":\"response\",\"id\":%s,\"status\":%d,", request_id_,
		response.status);
	if (n > 0 && static_cast<size_t>(n) < sizeof(prefix) && response.retry_after_s > 0) {
		n += std::snprintf(prefix + n, sizeof(prefix) - static_cast<size_t>(n), "\"retry_after_s\":%u,",
			static_cast<unsigned>(response.retry_after_s));
	}
	if (n > 0 && static_cast<size_t>(n) < sizeof(prefix)) {
		n += std::snprintf(prefix + n, sizeof(prefix) - static_cast<size_t>(n), "\"body\":");
	}
	if (n <= 0 || static_cast<size_t>(n) >= sizeof(prefix)) {
		busy_ = false;
		return;
	}
	char* body = out_ + kPrefix;
	size_t body_length = response.length;
	if (body_length == 0) {
		std::memcpy(body, "null", 4);
		body_length = 4;
	}
	char* start = body - n;
	std::memcpy(start, prefix, static_cast<size_t>(n));
	body[body_length] = '}';
	busy_ = false;
	if (!send_text(start, static_cast<size_t>(n) + body_length + 1)) {
		Log::warn(kTag, "the answer to request %s (%d) was not sent", request_id_, response.status);
	}
}

void EhomeLink::schedule_stream() {
	if (!stream_posted_) {
		stream_posted_ = loop_.post(Callback::member<EhomeLink, &EhomeLink::stream_next>(this));
		if (!stream_posted_) {
			// The loop's queue is full: try again in a moment.
			stream_timer_ = loop_.schedule_at(api_.controller().clock().monotonic_ms() + 10,
				Callback::member<EhomeLink, &EhomeLink::stream_next>(this));
		}
	}
}

void EhomeLink::stream_next() {
	stream_posted_ = false;
	stream_timer_ = TimerHandle();
	if (!streaming_ || phase_ != Phase::connected) {
		return;
	}
	size_t want = stream_length_ - stream_sent_;
	if (want > kStreamPiece) {
		want = kStreamPiece;
	}
	size_t got = 0;
	if (!api_.read_stream(stream_token_, stream_sent_, out_, want, got) || got == 0) {
		fail("the document of request %s changed while it was sent (%u of %u bytes): the socket is closed",
			request_id_, static_cast<unsigned>(stream_sent_), static_cast<unsigned>(stream_length_));
		return;
	}
	if (got > want) {
		got = want;
	}
	if (!socket_ || !client_.send_binary(reinterpret_cast<const uint8_t*>(out_), got)) {
		fail("cannot send the answer to request %s (%u of %u bytes)", request_id_,
			static_cast<unsigned>(stream_sent_), static_cast<unsigned>(stream_length_));
		return;
	}
	stream_sent_ += got;
	if (stream_sent_ >= stream_length_) {
		streaming_ = false;
		busy_ = false;
		Log::info(kTag, "request %s: %u bytes sent", request_id_, static_cast<unsigned>(stream_length_));
		return;
	}
	schedule_stream();
}

// ---- The enrollment ---------------------------------------------------------------------------

void EhomeLink::handle_route(const char* rest, const WebRequest& request, WebResponse& response) {
	if (rest[0] == '\0') {
		if (request.method != HttpMethod::del) {
			WebApi::refuse(response, 405, "api.method_not_allowed", "use DELETE");
			return;
		}
		disconnect(response);
		return;
	}
	if (std::strcmp(rest, "/enroll") == 0) {
		if (request.method != HttpMethod::post) {
			WebApi::refuse(response, 405, "api.method_not_allowed", "use POST");
			return;
		}
		enroll(request, response);
		return;
	}
	if (std::strcmp(rest, "/key/accept") == 0) {
		if (request.method != HttpMethod::post) {
			WebApi::refuse(response, 405, "api.method_not_allowed", "use POST");
			return;
		}
		key_route(request, response);
		return;
	}
	WebApi::refuse(response, 404, "api.not_found", "no such API endpoint");
}

void EhomeLink::enroll(const WebRequest& request, WebResponse& response) {
	if (enroll_ != Enroll::none) {
		WebApi::refuse(response, 409, "ehome.enrolling", "an enrollment is under way");
		return;
	}
	JsonSpan body;
	const char* error = nullptr;
	if (!json_parse(request.body, request.body_length, body, &error) || body.kind != JsonSpan::Kind::object) {
		WebApi::refuse(response, 400, "api.bad_json", error != nullptr ? error : "the body must be a JSON object");
		return;
	}
	char url[Settings::kMaxStringLength + 2];
	char code[40];
	if (!string_member(body, "url", url, sizeof(url))) {
		WebApi::refuse(response, 400, "ehome.url_invalid", "the body needs {\"url\": \"https://...\", \"code\": \"...\"}");
		return;
	}
	size_t url_length = std::strlen(url);
	while (url_length > 0 && url[url_length - 1] == '/') {
		url[--url_length] = '\0';
	}
	if (!ehome_url_valid(url, &error)) {
		WebApi::refuse(response, 400, "ehome.url_invalid", error);
		return;
	}
	if (!string_member(body, "code", code, sizeof(code)) || !ehome_code_valid(code, &error)) {
		WebApi::refuse(response, 400, "ehome.code_invalid",
			error != nullptr ? error : "the body needs the enrollment code E-Home shows (\"code\")");
		return;
	}
	reload();
	if (enrolled_) {
		char text[200];
		std::snprintf(text, sizeof(text), "the controller is enrolled with %s already: disconnect first", url_);
		Log::warn(kTag, "enrollment refused: %s", text);
		WebApi::refuse(response, 409, "ehome.enrolled", text);
		return;
	}
	char target[HttpFetcher::kMaxUrl + 1];
	std::snprintf(target, sizeof(target), "%s/api/controllers/enroll", url);
	char payload[384];
	JsonWriter json(payload, sizeof(payload));
	json.begin_object()
		.member("code", code)
		.member("name", api_.host().device_name())
		.member("version", kVersion)
		.member("build", build_id())
		.end_object();
	if (answer_ == nullptr) {
		answer_ = new (std::nothrow) char[kMaxEnrollAnswer];
	}
	if (answer_ != nullptr) {
		answer_body_ = FetchBody(answer_, kMaxEnrollAnswer, "E-Home's answer");
	}
	if (json.overflowed() || answer_ == nullptr ||
		!fetcher_.post(target, payload, json.length(), *this, &error)) {
		free_answer();
		char text[160];
		std::snprintf(text, sizeof(text), "cannot reach E-Home: %s",
			json.overflowed()     ? "the request does not fit"
			: error != nullptr    ? error
			: answer_ == nullptr  ? "no memory for the answer (2048 bytes)"
								  : "the HTTP client refused");
		Log::warn(kTag, "enrollment with %s failed: %s", url, text);
		WebApi::refuse(response, 502, "ehome.unreachable", text);
		return;
	}
	copy(enroll_url_, sizeof(enroll_url_), url);
	enroll_ = Enroll::running;
	enroll_timer_ = loop_.schedule_at(api_.controller().clock().monotonic_ms() + kEnrollTimeoutMs,
		Callback::member<EhomeLink, &EhomeLink::enroll_timeout>(this));
	Log::info(kTag, "enrolling with %s (the code from E-Home, entered by %s)", url, request.source);
	response.deferred = true;
	response.length = 0;
}

void EhomeLink::finish_enroll(int status, const char* code, const char* text) {
	loop_.cancel(enroll_timer_);
	enroll_timer_ = TimerHandle();
	enroll_ = Enroll::done;
	enroll_status_ = status;
	copy(enroll_code_, sizeof(enroll_code_), code);
	copy(enroll_text_, sizeof(enroll_text_), text);
}

void EhomeLink::enroll_timeout() {
	enroll_timer_ = TimerHandle();
	if (enroll_ != Enroll::running) {
		return;
	}
	char text[96];
	std::snprintf(text, sizeof(text), "no answer from E-Home within %u s", static_cast<unsigned>(kEnrollTimeoutMs / 1000));
	free_answer();
	Log::warn(kTag, "enrollment with %s failed: %s", enroll_url_, text);
	finish_enroll(502, "ehome.unreachable", text);
}

bool EhomeLink::poll_enroll(WebResponse& response) {
	if (enroll_ == Enroll::running) {
		return false;
	}
	if (enroll_ == Enroll::none) {
		WebApi::refuse(response, 500, "api.not_deferred", "no answer is pending");
		return true;
	}
	enroll_ = Enroll::none;
	if (enroll_status_ != 200) {
		WebApi::refuse(response, enroll_status_, enroll_code_, enroll_text_);
		return true;
	}
	response.status = 200;
	response.content_type = "application/json";
	JsonWriter json(response.body, response.capacity);
	json.begin_object().member("ok", true).member("installation", installation_).end_object();
	response.length = json.overflowed() ? 0 : json.length();
	return true;
}

void EhomeLink::free_answer() {
	delete[] answer_;
	answer_ = nullptr;
	answer_body_ = FetchBody(nullptr, 0, "E-Home's answer");
}

bool EhomeLink::fetch_begin(int, size_t content_length) {
	// The body of every status is read: E-Home's refusal document names the reason.
	return enroll_ == Enroll::running && answer_ != nullptr && answer_body_.begin(content_length);
}

bool EhomeLink::fetch_chunk(const char* data, size_t size) {
	return enroll_ == Enroll::running && answer_ != nullptr && answer_body_.append(data, size);
}

void EhomeLink::fetch_end(const FetchResult& result) {
	if (enroll_ != Enroll::running) {
		Log::info(kTag, "E-Home's late answer to an enrollment ignored (HTTP %d)", result.http_status);
		free_answer();
		return;
	}
	if (result.http_status == 200 && answer_body_.refusal()[0] != '\0') {
		char text[160];
		std::snprintf(text, sizeof(text), "E-Home's answer is not usable: %s", answer_body_.refusal());
		free_answer();
		Log::warn(kTag, "enrollment with %s failed: %s", enroll_url_, text);
		finish_enroll(502, "ehome.bad_answer", text);
		return;
	}
	// The answer's buffer goes when this call returns (the spans below point into it).
	struct Release {
		EhomeLink* self;
		~Release() { self->free_answer(); }
	} release{this};
	// A body that overflowed or was cut is not parsed; its status still speaks.
	JsonSpan answer;
	bool parsed = result.complete && answer_ != nullptr && answer_body_.length() > 0 && json_parse(answer_body_.data(), answer_body_.length(), answer) &&
		answer.kind == JsonSpan::Kind::object;
	char error_text[128] = {};
	char error_code[32] = {};
	if (parsed) {
		string_member(answer, "error", error_text, sizeof(error_text));
		string_member(answer, "code", error_code, sizeof(error_code));
	}
	int status = result.http_status;
	if (status == 200 && result.ok) {
		char id[Settings::kMaxStringLength + 1] = {};
		char name[Settings::kMaxStringLength + 1] = {};
		char secret[Settings::kMaxStringLength + 1] = {};
		char socket[Settings::kMaxStringLength + 1] = {};
		char key_text[2 * kEd25519KeySize + 2] = {};
		uint8_t key[kEd25519KeySize];
		bool has_key = false;
		const char* what = nullptr;
		const char* why = nullptr;
		if (!parsed) {
			what = "the answer";
			why = "not a JSON object";
		} else if (!string_member(answer, "controller", id, sizeof(id)) || !ehome_id_valid(id, &why)) {
			what = "controller";
		} else if (!string_member(answer, "installation", name, sizeof(name)) ||
			Settings::check_string(name) != SettingsStatus::ok) {
			what = "installation";
			why = "not a name of at most 127 bytes";
		} else if (!string_member(answer, "secret", secret, sizeof(secret)) || !ehome_secret_valid(secret, &why)) {
			what = "secret";
		} else if (!string_member(answer, "socket", socket, sizeof(socket)) ||
			!ehome_socket_valid(socket, enroll_url_, &why)) {
			what = "socket";
		} else {
			JsonSpan member;
			has_key = json_member(answer, "public_key", member) && member.kind != JsonSpan::Kind::null;
			if (has_key && (!json_string(member, key_text, sizeof(key_text)) || !parse_public_key(key_text, key))) {
				what = "public_key";
				why = "not an Ed25519 public key of 64 hex digits";
			}
		}
		if (what != nullptr) {
			std::memset(secret, 0, sizeof(secret));
			char text[160];
			std::snprintf(text, sizeof(text), "E-Home's answer is not usable: %s (%s)", what,
				why != nullptr ? why : "missing");
			Log::warn(kTag, "enrollment with %s failed: %s", enroll_url_, text);
			finish_enroll(502, "ehome.bad_answer", text);
			return;
		}
		Settings& settings = api_.controller().settings();
		SettingsStatus stored = settings.set_string(setting::kEhomeUrl, enroll_url_);
		if (stored == SettingsStatus::ok) {
			stored = settings.set_string(setting::kEhomeId, id);
		}
		if (stored == SettingsStatus::ok) {
			stored = settings.set_string(setting::kEhomeName, name);
		}
		if (stored == SettingsStatus::ok) {
			stored = settings.set_string(setting::kEhomeSocket, socket);
		}
		if (stored == SettingsStatus::ok) {
			stored = settings.set_string(setting::kEhomeSecret, secret);
		}
		if (stored == SettingsStatus::ok) {
			stored = settings.set_bool(setting::kEhomeEnabled, true);
		}
		if (stored == SettingsStatus::ok) {
			// The installation key comes with the enrollment, trusted by the one-time code and TLS
			// (spec/api.md "Signed runtime images"): it replaces a key kept from an earlier pairing; an
			// answer without one leaves the device without. Written last, so that a failed enrollment
			// keeps the old key in force.
			if (has_key) {
				char hex[2 * kEd25519KeySize + 1];
				to_hex(key, sizeof(key), hex);
				stored = settings.set_string(setting::kEhomeKey, hex);
			} else {
				SettingsStatus erased = settings.erase(setting::kEhomeKey);
				stored = erased == SettingsStatus::not_found ? SettingsStatus::ok : erased;
			}
		}
		std::memset(secret, 0, sizeof(secret));
		if (stored != SettingsStatus::ok) {
			// The enrollment goes; an installation key kept from before stays in force.
			const char* keys[] = {setting::kEhomeUrl, setting::kEhomeId, setting::kEhomeName, setting::kEhomeSocket,
				setting::kEhomeSecret, setting::kEhomeEnabled};
			for (const char* key : keys) {
				settings.erase(key);
			}
			char text[96];
			std::snprintf(text, sizeof(text), "the device could not store the enrollment (%s)",
				settings_status_name(stored));
			Log::error(kTag, "enrollment with %s failed: %s", enroll_url_, text);
			finish_enroll(500, "settings.storage_error", text);
			return;
		}
		reload();
		drop_pending_key("enrolled anew");
		Log::info(kTag, "enrolled with %s as controller %s, installation \"%s\"; the connection is switched on",
			enroll_url_, id, installation_);
		if (has_key) {
			char fingerprint[kFingerprintSize];
			key_fingerprint(key, fingerprint);
			Log::info(kTag, "installation key %s stored: runtime images need its signature from now on", fingerprint);
		} else {
			Log::info(kTag, "E-Home sent no installation key: runtime images are not signed");
		}
		finish_enroll(200, nullptr, nullptr);
		backoff_.reset();
		error_[0] = '\0';
		last_logged_[0] = '\0';
		attempt();
		return;
	}
	if (status == 404 || status == 409 || status == 410) {
		const char* code = status == 404 ? "ehome.code_unknown" : status == 410 ? "ehome.code_expired" : "ehome.enrolled";
		const char* text = status == 404 ? "E-Home does not know this code (wrong, or used already)"
			: status == 410 ? "the code has expired: ask E-Home for a new one"
			: "E-Home has this controller enrolled already: disconnect it there first";
		if (starts_with(error_code, "ehome.")) {
			code = error_code;
		}
		if (error_text[0] != '\0') {
			text = error_text;
		}
		Log::warn(kTag, "E-Home refused the enrollment: %s (%s)", text, code);
		finish_enroll(status, code, text);
		return;
	}
	char text[160];
	if (status != 0) {
		std::snprintf(text, sizeof(text), "E-Home answered HTTP %d%s%s", status, error_text[0] != '\0' ? ": " : "",
			error_text);
	} else {
		std::snprintf(text, sizeof(text), "cannot reach E-Home: %s", result.error != nullptr ? result.error : "no answer");
	}
	Log::warn(kTag, "enrollment with %s failed: %s", enroll_url_, text);
	finish_enroll(502, "ehome.unreachable", text);
}

void EhomeLink::disconnect(WebResponse& response) {
	if (enroll_ == Enroll::running) {
		WebApi::refuse(response, 409, "ehome.enrolling", "an enrollment is under way");
		return;
	}
	go_idle("disconnected by the owner"); // drops a pending key with the socket
	Settings& settings = api_.controller().settings();
	// The installation key (ehome.key) stays: a device once paired keeps refusing unsigned images until
	// a factory reset or a new enrollment's answer replaces it (spec/api.md "Signed runtime images").
	const char* keys[] = {setting::kEhomeEnabled, setting::kEhomeSecret, setting::kEhomeSocket, setting::kEhomeId,
		setting::kEhomeName, setting::kEhomeUrl};
	SettingsStatus failed = SettingsStatus::ok;
	for (const char* key : keys) {
		SettingsStatus status = settings.erase(key);
		if (status != SettingsStatus::ok && status != SettingsStatus::not_found) {
			failed = status;
		}
	}
	char was[sizeof(url_)];
	copy(was, sizeof(was), url_);
	reload();
	if (failed != SettingsStatus::ok) {
		char text[96];
		std::snprintf(text, sizeof(text), "the device could not erase the enrollment (%s)", settings_status_name(failed));
		Log::error(kTag, "disconnect: %s", text);
		WebApi::refuse(response, 500, "settings.storage_error", text);
		return;
	}
	if (was[0] != '\0') {
		Log::info(kTag, "disconnected from E-Home at %s: the enrollment is erased", was);
	}
	char fingerprint[kFingerprintSize];
	if (stored_key(fingerprint) == KeyState::ok) {
		Log::info(kTag, "the installation key %s stays in force: images still need its signature (until a factory "
			"reset or a new enrollment)", fingerprint);
	}
	JsonWriter json(response.body, response.capacity);
	json.begin_object().member("ok", true).end_object();
	response.length = json.overflowed() ? 0 : json.length();
}

// ---- The installation key ---------------------------------------------------------------------

KeyState EhomeLink::stored_key(char* out) const {
	uint8_t key[kEd25519KeySize];
	KeyState state = load_installation_key(api_.controller().settings(), key);
	if (state == KeyState::ok) {
		key_fingerprint(key, out);
	} else {
		out[0] = '\0';
	}
	return state;
}

bool EhomeLink::pending_key(char* out) const {
	if (!has_pending_key_) {
		out[0] = '\0';
		return false;
	}
	key_fingerprint(pending_key_, out);
	return true;
}

void EhomeLink::drop_pending_key(const char* why) {
	if (!has_pending_key_) {
		return;
	}
	char fingerprint[kFingerprintSize];
	key_fingerprint(pending_key_, fingerprint);
	has_pending_key_ = false;
	std::memset(pending_key_, 0, sizeof(pending_key_));
	Log::info(kTag, "the offered installation key %s was not accepted and is dropped: %s", fingerprint, why);
}

void EhomeLink::send_key_message(const char* type, const char* fingerprint) {
	char text[96];
	JsonWriter json(text, sizeof(text));
	json.begin_object().member("type", type).member("fingerprint", fingerprint).end_object();
	if (json.overflowed() || !send_text(text, json.length())) {
		Log::warn(kTag, "the message %s to E-Home was not sent", type);
	}
}

void EhomeLink::key_message(const JsonSpan& message) {
	char text[2 * kEd25519KeySize + 2] = {};
	char announced[kFingerprintSize + 4] = {};
	uint8_t key[kEd25519KeySize];
	const char* refusal = nullptr;
	if (!string_member(message, "public_key", text, sizeof(text)) || !parse_public_key(text, key)) {
		refusal = "\"public_key\" is an Ed25519 public key of 64 hex digits";
	}
	char fingerprint[kFingerprintSize];
	if (refusal == nullptr) {
		key_fingerprint(key, fingerprint);
		JsonSpan member;
		if (json_member(message, "fingerprint", member) &&
			(!json_string(member, announced, sizeof(announced)) || std::strcmp(announced, fingerprint) != 0)) {
			refusal = "\"fingerprint\" is not the key's (the first 8 bytes of its SHA-256, hex in groups of four)";
		}
	}
	if (refusal != nullptr) {
		Log::warn(kTag, "E-Home offered an unusable installation key: %s", refusal);
		char out[256];
		JsonWriter json(out, sizeof(out));
		json.begin_object().member("type", "error").member("error", refusal).member("code", "ehome.bad_key").end_object();
		if (!json.overflowed()) {
			send_text(out, json.length());
		}
		return;
	}
	uint8_t current[kEd25519KeySize];
	if (load_installation_key(api_.controller().settings(), current) == KeyState::ok &&
		constant_time_equal(current, key, sizeof(key))) {
		drop_pending_key("E-Home offered the key in force again");
		Log::info(kTag, "E-Home offered the installation key in force (%s): nothing to accept", fingerprint);
		send_key_message("key_accepted", fingerprint);
		return;
	}
	if (has_pending_key_ && constant_time_equal(pending_key_, key, sizeof(key))) {
		send_key_message("key_pending", fingerprint);
		return;
	}
	drop_pending_key("E-Home offered another one");
	std::memcpy(pending_key_, key, sizeof(key));
	has_pending_key_ = true;
	Log::warn(kTag, "E-Home offers a new installation key %s: it takes effect when the owner accepts it (Settings "
		"page, or `ehome key accept` on the console); until then the key in force stays", fingerprint);
	send_key_message("key_pending", fingerprint);
}

bool EhomeLink::accept_key(const char* fingerprint, const char** code, const char** error) {
	if (!has_pending_key_) {
		*code = "ehome.no_pending_key";
		*error = "E-Home has not offered a new installation key (or the connection ended since)";
		return false;
	}
	char pending[kFingerprintSize];
	key_fingerprint(pending_key_, pending);
	if (fingerprint != nullptr && std::strcmp(fingerprint, pending) != 0) {
		*code = "ehome.key_changed";
		*error = "the offered key is not the one shown any more: look at its fingerprint again";
		return false;
	}
	char hex[2 * kEd25519KeySize + 1];
	to_hex(pending_key_, sizeof(pending_key_), hex);
	SettingsStatus status = api_.controller().settings().set_string(setting::kEhomeKey, hex);
	if (status != SettingsStatus::ok) {
		Log::error(kTag, "the installation key %s was not stored (%s): the old key stays in force", pending,
			settings_status_name(status));
		*code = "settings.storage_error";
		*error = "the device could not store the key";
		return false;
	}
	has_pending_key_ = false;
	std::memset(pending_key_, 0, sizeof(pending_key_));
	Log::warn(kTag, "installation key %s accepted by the owner: runtime images need its signature from now on", pending);
	if (phase_ == Phase::connected) {
		send_key_message("key_accepted", pending);
	}
	return true;
}

void EhomeLink::key_route(const WebRequest& request, WebResponse& response) {
	JsonSpan body;
	const char* error = nullptr;
	char fingerprint[kFingerprintSize + 4] = {};
	if (!json_parse(request.body, request.body_length, body, &error) || body.kind != JsonSpan::Kind::object ||
		!string_member(body, "fingerprint", fingerprint, sizeof(fingerprint))) {
		WebApi::refuse(response, 400, "api.bad_json",
			"the body needs {\"fingerprint\": \"...\"}: the fingerprint of the key the owner accepts");
		return;
	}
	const char* code = nullptr;
	if (!accept_key(fingerprint, &code, &error)) {
		Log::warn(kTag, "accepting the installation key refused (%s): %s", request.source, error);
		WebApi::refuse(response, std::strcmp(code, "settings.storage_error") == 0 ? 500 : 409, code, error);
		return;
	}
	JsonWriter json(response.body, response.capacity);
	json.begin_object().member("ok", true).member("key", fingerprint).end_object();
	response.length = json.overflowed() ? 0 : json.length();
}

// ---- Status -----------------------------------------------------------------------------------

void EhomeLink::write_status(JsonWriter& json) const {
	uint64_t now = api_.controller().clock().monotonic_ms();
	json.begin_object()
		.member("enabled", enabled_)
		.member("enrolled", enrolled_)
		.member("state", ehome_state_name(state_))
		.member("since_s", (now - since_ms_) / 1000);
	if (enrolled_) {
		json.member("installation", installation_); // the address is the setting ehome.url
	}
	char fingerprint[kFingerprintSize];
	KeyState key = stored_key(fingerprint);
	if (key == KeyState::ok) {
		json.member("key", fingerprint);
	} else if (key == KeyState::damaged) {
		json.member("key", "damaged");
	}
	if (pending_key(fingerprint)) {
		json.member("pending_key", fingerprint);
	}
	if (error_[0] != '\0') {
		json.member("error", error_);
	}
	json.end_object();
}

} // namespace hc
