/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_WEB_TEST_SUPPORT_H
#define HC_WEB_TEST_SUPPORT_H

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "hc/auth.h"
#include "hc/controller.h"
#include "hc/datapoint.h"
#include "hc/json.h"
#include "hc/live_feed.h"
#include "hc/platform/crypto.h"
#include "hc/platform/web_host.h"
#include "hc/sha.h"
#include "hc/web_api.h"
#include "json_reader.h"
#include "test_support.h"

namespace hc_test {

/// Deterministic "random" bytes (a counter) and the core's software PBKDF2.
class FakeCrypto : public hc::Crypto {
public:
	bool random(uint8_t* out, size_t length) override {
		if (fail) {
			return false;
		}
		for (size_t i = 0; i < length; ++i) {
			out[i] = static_cast<uint8_t>(counter++ * 37 + 11);
		}
		return true;
	}
	void pbkdf2_sha256(const uint8_t* password, size_t password_length, const uint8_t* salt, size_t salt_length,
		uint32_t iterations, uint8_t* out, size_t out_length) override {
		hc::pbkdf2_sha256(password, password_length, salt, salt_length, iterations, out, out_length);
	}
	uint32_t counter = 1;
	bool fail = false;
};

class FakeWebHost : public hc::WebHost {
public:
	const char* device_name() const override { return "homecontrol-a1b2c3"; }
	bool mdns_announced() const override { return mdns; }
	const char* ntp_server() const override { return "pool.ntp.org"; }
	void ntp_server_changed() override { ++ntp_changes; }
	bool set_update_url(const char* url, const char** error) override {
		if (url[0] != '\0' && std::strncmp(url, "http", 4) != 0) {
			*error = "not an http:// or https:// URL";
			return false;
		}
		update_url = url;
		return true;
	}
	void write_update_state(hc::JsonWriter& json) const override {
		json.member("available", false).member("source", update_url.c_str());
	}
	const char* ui_version() const override { return "abc123"; }
	uint16_t websocket_port() const override { return 0; }

	// The system hooks (issue #38): the base's "unavailable" until a test enables them.
	hc::HostResult system_action_possible(hc::SystemAction action, const char** reason) const override {
		if (!system_actions) {
			return hc::WebHost::system_action_possible(action, reason);
		}
		return hc::HostResult::ok;
	}
	void system_action(hc::SystemAction action) override { actions.push_back(action); }
	hc::HostResult start_update(bool force, const char* url, const char** reason) override {
		if (!updates) {
			return hc::WebHost::start_update(force, url, reason);
		}
		if (!update_refusal.empty()) {
			*reason = update_refusal.c_str();
			return hc::HostResult::refused;
		}
		update_calls.push_back(std::string(force ? "now " : "check ") + (url != nullptr ? url : "-"));
		return hc::HostResult::ok;
	}
	hc::HostResult start_upload(const char* token, const char** reason) override {
		if (!uploads) {
			return hc::WebHost::start_upload(token, reason);
		}
		if (!upload_refusal.empty()) {
			*reason = upload_refusal.c_str();
			return hc::HostResult::refused;
		}
		upload_tokens.push_back(token);
		return hc::HostResult::ok;
	}
	hc::HostResult clear_coredump(const char** reason) override {
		if (!coredump_clearable) {
			return hc::WebHost::clear_coredump(reason);
		}
		++coredump_clears;
		return hc::HostResult::ok;
	}
	bool set_platform_log_level(const char* tag, hc::LogLevel level) override {
		if (!platform_log) {
			return false;
		}
		platform_levels.push_back(std::string(tag) + "=" + hc::log_level_name(level));
		return true;
	}
	bool has_platform_log() const override { return platform_log; }
	bool storage_usage(uint64_t& total, uint64_t& used) const override {
		if (!storage) {
			return false;
		}
		total = 1048576;
		used = 65536;
		return true;
	}

	bool mdns = true;
	int ntp_changes = 0;
	std::string update_url;
	bool system_actions = false;
	std::vector<hc::SystemAction> actions;
	bool updates = false;
	std::string update_refusal;
	std::vector<std::string> update_calls;
	bool uploads = false;
	std::string upload_refusal;
	std::vector<std::string> upload_tokens;
	bool coredump_clearable = false;
	int coredump_clears = 0;
	bool platform_log = false;
	std::vector<std::string> platform_levels;
	bool storage = false;
};

/// Records what the feed sends; can refuse sends.
class FakeTransport : public hc::WsTransport {
public:
	bool ws_send(uint32_t client, const char* text, size_t length) override {
		if (refuse) {
			++refused;
			return false;
		}
		sent.push_back({client, std::string(text, length)});
		return true;
	}
	void ws_close(uint32_t client, uint16_t code, const char* reason) override {
		closes.push_back({client, code, reason});
	}

	/// Parsed messages to `client` of `type`.
	std::vector<Json> messages(uint32_t client, const std::string& type) const {
		std::vector<Json> out;
		for (const auto& s : sent) {
			if (s.client != client) {
				continue;
			}
			Json json;
			std::string error;
			if (JsonReader(s.text).parse(json, error) && json["type"].text == type) {
				out.push_back(json);
			}
		}
		return out;
	}

	struct Sent {
		uint32_t client;
		std::string text;
	};
	struct Close {
		uint32_t client;
		uint16_t code;
		std::string reason;
	};
	std::vector<Sent> sent;
	std::vector<Close> closes;
	bool refuse = false;
	int refused = 0;
};

/// A controller with a datapoint model, the authentication and the API, wired to fakes.
struct WebFixture {
	FakePlatform fake;
	hc::Controller controller{fake.platform()};
	hc::DatapointModel model{fake.clock};
	FakeCrypto crypto;
	FakeWebHost host;
	hc::Auth auth{fake.settings, fake.clock, crypto};
	hc::WebApi api{controller, model, auth, host};
	char buffer[8192];
	/// The X-Image-Signature of the requests that follow ("": none).
	std::string image_signature;

	struct Result {
		int status = 0;
		std::string body;
		std::string cookie;
		uint32_t retry_after = 0;
		Json json;
	};

	WebFixture() { auth.start(); }

	/// Runs a request through both passes, computing the hash in between as a platform does.
	Result request(hc::HttpMethod method, const std::string& target, const std::string& body = "",
		const std::string& cookie = "", const std::string& authorization = "", bool csrf = true,
		const char* source = "192.0.2.7") {
		std::string path = target;
		std::string query;
		size_t q = target.find('?');
		if (q != std::string::npos) {
			path = target.substr(0, q);
			query = target.substr(q + 1);
		}
		hc::WebRequest req;
		req.method = method;
		req.path = path.c_str();
		req.query = query.c_str();
		req.cookie = cookie.empty() ? nullptr : cookie.c_str();
		req.authorization = authorization.empty() ? nullptr : authorization.c_str();
		req.requested_with = csrf ? "HomeControl" : nullptr;
		req.image_signature = image_signature.empty() ? nullptr : image_signature.c_str();
		req.source = source;
		req.body = body.c_str();
		req.body_length = body.size();
		hc::WebResponse resp(buffer, sizeof(buffer));
		api.handle(req, resp);
		hc::PasswordHash hash;
		if (resp.needs_hash) {
			hash.params = resp.hash_params;
			crypto.pbkdf2_sha256(reinterpret_cast<const uint8_t*>(resp.password), resp.password_length,
				hash.params.salt, sizeof(hash.params.salt), hash.params.iterations, hash.key, sizeof(hash.key));
			req.hash = &hash;
			hc::WebResponse second(buffer, sizeof(buffer));
			api.handle(req, second);
			return result(second);
		}
		return result(resp);
	}

	Result get(const std::string& target, const std::string& cookie = "", const std::string& authorization = "") {
		return request(hc::HttpMethod::get, target, "", cookie, authorization);
	}

	Result post(const std::string& target, const std::string& body, const std::string& cookie = "",
		const std::string& authorization = "", bool csrf = true) {
		return request(hc::HttpMethod::post, target, body, cookie, authorization, csrf);
	}

	/// Onboards with `password` and returns the Cookie header for the session.
	std::string onboard(const std::string& password = "correct horse") {
		Result r = post("/api/auth/onboard", "{\"password\":\"" + password + "\"}");
		REQUIRE(r.status == 200);
		return r.cookie;
	}

	static Result result(const hc::WebResponse& resp) {
		Result r;
		r.status = resp.status;
		r.body.assign(resp.body, resp.length);
		r.retry_after = resp.retry_after_s;
		std::string cookie = resp.set_cookie;
		size_t semicolon = cookie.find(';');
		r.cookie = cookie.substr(0, semicolon);
		std::string error;
		if (!r.body.empty()) {
			JsonReader(r.body).parse(r.json, error);
		}
		return r;
	}
};

} // namespace hc_test

#endif // HC_WEB_TEST_SUPPORT_H
