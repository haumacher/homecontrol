/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// "Stay logged in" (homecontrol #53): remembered sessions in web.sess.0 .. web.sess.3 that survive
// restarts and have no idle expiry, written only when created or ended; "log out everywhere".
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "hc/auth.h"
#include "hc/live_feed.h"
#include "hc/network_settings.h"
#include "hc/sha.h"
#include "hc/websocket.h"
#include "web_test_support.h"

using hc::Auth;
using hc::AuthStatus;
using hc::HttpMethod;
using hc_test::WebFixture;

namespace {

const char* const kLogin = "{\"password\":\"correct horse\"}";
const char* const kRemember = "{\"password\":\"correct horse\",\"remember\":true}";

/// The API on the same settings and clock after a simulated restart: a fresh Auth (RAM empty) and
/// a fresh WebApi, the fixture's requests go there.
struct Restart {
	hc::Auth auth;
	hc::WebApi api;

	explicit Restart(WebFixture& f) : auth(f.fake.settings, f.fake.clock, f.crypto), api(f.controller, f.model, auth, f.host) {
		auth.start();
		f.api_target = &api;
	}
};

std::string remembered_login(WebFixture& f) {
	auto r = f.post("/api/auth/login", kRemember);
	REQUIRE(r.status == 200);
	REQUIRE(r.cookie.rfind("hc_session=", 0) == 0);
	return r.cookie;
}

std::string plain_login(WebFixture& f) {
	auto r = f.post("/api/auth/login", kLogin);
	REQUIRE(r.status == 200);
	return r.cookie;
}

int status_of(WebFixture& f, const std::string& cookie) { return f.get("/api/status", cookie).status; }

/// All stored text of the settings, keys and values.
std::string settings_text(const WebFixture& f) {
	std::string all;
	for (const auto& e : f.fake.settings.entries) {
		all += e.first + "=" + e.second.text + "\n";
	}
	return all;
}

} // namespace

TEST_CASE("AuthRemember: the cookie of a remembered login carries a Max-Age, a plain one none") {
	WebFixture f;
	auto onboard = f.post("/api/auth/onboard", "{\"password\":\"correct horse\",\"remember\":true}");
	REQUIRE(onboard.status == 200);
	CHECK(onboard.set_cookie.find("; Max-Age=315360000;") != std::string::npos);
	CHECK(onboard.set_cookie.find("HttpOnly") != std::string::npos);
	CHECK(onboard.set_cookie.find("SameSite=Strict") != std::string::npos);
	CHECK(onboard.set_cookie.find("Path=/") != std::string::npos);
	CHECK(f.auth.remembered_count() == 1);

	auto remembered = f.post("/api/auth/login", kRemember);
	REQUIRE(remembered.status == 200);
	CHECK(remembered.set_cookie.find("Max-Age=315360000") != std::string::npos);
	auto plain = f.post("/api/auth/login", kLogin);
	REQUIRE(plain.status == 200);
	CHECK(plain.set_cookie.find("Max-Age") == std::string::npos);
	auto off = f.post("/api/auth/login", "{\"password\":\"correct horse\",\"remember\":false}");
	REQUIRE(off.status == 200);
	CHECK(off.set_cookie.find("Max-Age") == std::string::npos);
	CHECK(f.auth.remembered_count() == 2);
	CHECK(f.auth.session_count() == 4);

	// Not a boolean: refused with a reason, before any hashing; no session.
	auto bad = f.post("/api/auth/login", "{\"password\":\"correct horse\",\"remember\":\"yes\"}");
	CHECK(bad.status == 400);
	CHECK(bad.json["code"].text == "auth.remember_invalid");
	CHECK(bad.set_cookie.empty());
	CHECK(f.auth.session_count() == 4);

	auto status = f.get("/api/status", plain.cookie);
	CHECK(status.json["web"]["sessions"].number == 4);
	CHECK(status.json["web"]["remembered"].number == 2);
}

TEST_CASE("AuthRemember: a remembered session survives a restart, a plain one does not") {
	WebFixture f;
	f.onboard();
	std::string remembered = remembered_login(f);
	std::string plain = plain_login(f);
	Restart restart(f);
	CHECK(status_of(f, remembered) == 200);
	auto refused = f.get("/api/status", plain);
	CHECK(refused.status == 401);
	CHECK(refused.json["code"].text == "auth.required");
	CHECK(f.get("/api/auth/session", remembered).json["via"].text == "session");
	// And a second restart, by start() again on the same object.
	restart.auth.start();
	CHECK(status_of(f, remembered) == 200);
}

TEST_CASE("AuthRemember: a remembered session has no idle expiry, a plain one has") {
	WebFixture f;
	f.onboard();
	std::string remembered = remembered_login(f);
	std::string plain = plain_login(f);
	f.fake.clock.advance(Auth::kSessionIdleMs + 60 * 60 * 1000); // 25 h without use
	CHECK(status_of(f, remembered) == 200);
	CHECK(status_of(f, plain) == 401);
	f.fake.clock.advance(10ull * Auth::kSessionIdleMs);
	CHECK(status_of(f, remembered) == 200);
	CHECK(f.auth.session_count() == 1);
}

TEST_CASE("AuthRemember: a fifth remembered login replaces the oldest, also after a restart") {
	WebFixture f;
	f.onboard();
	std::vector<std::string> cookies;
	for (size_t i = 0; i < Auth::kRememberedSlots + 1; ++i) {
		cookies.push_back(remembered_login(f));
		// Use the first one heavily: creation order decides, not use.
		CHECK(status_of(f, cookies[0]) == (i < Auth::kRememberedSlots ? 200 : 401));
	}
	auto check = [&]() {
		CHECK(status_of(f, cookies[0]) == 401);
		for (size_t i = 1; i < cookies.size(); ++i) {
			CHECK(status_of(f, cookies[i]) == 200);
		}
	};
	check();
	CHECK(f.auth.remembered_count() == Auth::kRememberedSlots);
	Restart restart(f);
	check();
	// The order is persisted: after the restart a sixth replaces the second, not a later one.
	cookies.push_back(remembered_login(f));
	CHECK(status_of(f, cookies[1]) == 401);
	for (size_t i = 2; i < cookies.size(); ++i) {
		CHECK(status_of(f, cookies[i]) == 200);
	}
	Restart again(f);
	CHECK(status_of(f, cookies[1]) == 401);
	for (size_t i = 2; i < cookies.size(); ++i) {
		CHECK(status_of(f, cookies[i]) == 200);
	}
	CHECK(f.fake.settings.entries.size() >= Auth::kRememberedSlots);
	for (size_t i = 0; i < Auth::kRememberedSlots; ++i) {
		CHECK(f.fake.settings.entries.count("web.sess." + std::to_string(i)) == 1);
	}
	CHECK(f.fake.settings.entries.count("web.sess.4") == 0);
}

TEST_CASE("AuthRemember: the settings are written at creation and end only, never on use") {
	WebFixture f;
	hc_test::FakeTransport transport;
	hc::LiveFeed feed{f.api, f.controller.loop(), transport};
	REQUIRE(feed.start());
	f.onboard();
	size_t before = f.fake.settings.writes;
	std::string cookie = remembered_login(f);
	CHECK(f.fake.settings.writes == before + 1); // the creation
	before = f.fake.settings.writes;
	for (int i = 0; i < 50; ++i) {
		CHECK(status_of(f, cookie) == 200);
		CHECK(f.get("/api/auth/session", cookie).json["authenticated"].boolean);
		f.fake.clock.advance(60 * 60 * 1000);
	}
	// A WebSocket on the session, kept alive by the feed's checks every second.
	REQUIRE(feed.open(1, cookie.c_str(), nullptr, nullptr));
	for (int i = 0; i < 100; ++i) {
		f.fake.clock.advance(100);
		f.controller.loop().run_once(f.fake.clock.now_ms);
	}
	CHECK(feed.client_count() == 1);
	Restart restart(f);
	CHECK(status_of(f, cookie) == 200);
	CHECK(f.fake.settings.writes == before); // no write on use, nor at the restart
	auto out = f.post("/api/auth/logout", "{}", cookie);
	CHECK(out.status == 200);
	CHECK(f.fake.settings.writes == before + 1); // the end

}

TEST_CASE("AuthRemember: a logout ends the remembered session, also after a restart") {
	WebFixture f;
	f.onboard();
	std::string cookie = remembered_login(f);
	std::string other = remembered_login(f);
	auto out = f.post("/api/auth/logout", "{}", cookie);
	CHECK(out.status == 200);
	CHECK(out.set_cookie.find("Max-Age=0") != std::string::npos);
	CHECK(status_of(f, cookie) == 401);
	Restart restart(f);
	CHECK(status_of(f, cookie) == 401);
	CHECK(status_of(f, other) == 200);
	CHECK(f.auth.remembered_count() == 1);
}

TEST_CASE("AuthRemember: log out everywhere ends every session, closes the WebSockets, needs the CSRF header") {
	WebFixture f;
	hc_test::FakeTransport transport;
	hc::LiveFeed feed{f.api, f.controller.loop(), transport};
	REQUIRE(feed.start());
	std::string first = f.onboard();
	std::string plain = plain_login(f);
	std::string remembered = remembered_login(f);
	std::string remembered2 = remembered_login(f);
	REQUIRE(feed.open(1, plain.c_str(), nullptr, nullptr));
	REQUIRE(feed.open(2, remembered.c_str(), nullptr, nullptr));

	// Without the CSRF header: refused, nothing ends.
	auto csrf = f.request(HttpMethod::post, "/api/auth/sessions/clear", "{}", remembered, "", false);
	CHECK(csrf.status == 403);
	CHECK(csrf.json["code"].text == "auth.csrf");
	CHECK(status_of(f, remembered) == 200);
	// Without a principal: 401; with GET: 405.
	CHECK(f.post("/api/auth/sessions/clear", "{}").status == 401);
	CHECK(f.get("/api/auth/sessions/clear", remembered).status == 405);

	auto clear = f.post("/api/auth/sessions/clear", "{}", remembered);
	CHECK(clear.status == 200);
	CHECK(clear.json["ended"].number == 4);
	CHECK(clear.set_cookie.find("Max-Age=0") != std::string::npos);
	for (const std::string& c : {first, plain, remembered, remembered2}) {
		CHECK(status_of(f, c) == 401);
	}
	CHECK(f.auth.session_count() == 0);
	for (int i = 0; i < 20; ++i) {
		f.fake.clock.advance(100);
		f.controller.loop().run_once(f.fake.clock.now_ms);
	}
	CHECK(feed.client_count() == 0);
	int ended = 0;
	for (const auto& close : transport.closes) {
		CHECK(close.code == hc::websocket::kClosePolicy);
		++ended;
	}
	CHECK(ended == 2);
	CHECK(transport.messages(1, "error").back()["code"].text == "auth.session_ended");
	CHECK(transport.messages(2, "error").back()["code"].text == "auth.session_ended");
	Restart restart(f);
	for (const std::string& c : {first, plain, remembered, remembered2}) {
		CHECK(status_of(f, c) == 401);
	}
	CHECK(restart.auth.remembered_count() == 0);
	for (size_t i = 0; i < Auth::kRememberedSlots; ++i) {
		CHECK(f.fake.settings.entries.count("web.sess." + std::to_string(i)) == 0);
	}

}

TEST_CASE("AuthRemember: log out everywhere with a token, and the E-Home principal may not") {
	WebFixture f;
	f.onboard();
	std::string remembered = remembered_login(f);
	char token[Auth::kTokenText];
	REQUIRE(f.auth.create_token("script", token) == AuthStatus::ok);
	std::string bearer = std::string("Bearer ") + token;
	auto clear = f.request(HttpMethod::post, "/api/auth/sessions/clear", "{}", "", bearer, false);
	CHECK(clear.status == 200);
	CHECK(status_of(f, remembered) == 401);
	CHECK(f.get("/api/status", "", bearer).status == 200); // tokens stay

	hc::WebRequest req;
	req.method = HttpMethod::post;
	req.path = "/api/auth/sessions/clear";
	req.ehome = true;
	req.body = "{}";
	req.body_length = 2;
	hc::WebResponse resp(f.buffer, sizeof(f.buffer));
	f.api.handle(req, resp);
	CHECK(resp.status == 403);
}

TEST_CASE("AuthRemember: clear_password ends the remembered sessions too") {
	WebFixture f;
	f.onboard();
	std::string remembered = remembered_login(f);
	hc::SessionRef ref = f.auth.session_from_cookie(remembered.c_str());
	REQUIRE(ref.valid());
	REQUIRE(f.auth.clear_password() == AuthStatus::ok);
	CHECK_FALSE(f.auth.session_alive(ref));
	CHECK(f.auth.remembered_count() == 0);
	f.onboard("another one");
	CHECK(status_of(f, remembered) == 401);
	Restart restart(f);
	CHECK(status_of(f, remembered) == 401);
	CHECK(restart.auth.remembered_count() == 0);
}

TEST_CASE("AuthRemember: the settings hold the SHA-256 of the session id, never the cookie") {
	WebFixture f;
	f.onboard();
	std::string cookie = remembered_login(f);
	std::string id = cookie.substr(std::strlen("hc_session="));
	REQUIRE(id.size() == 32);
	std::string all = settings_text(f);
	CHECK(all.find(id) == std::string::npos);
	const auto& stored = f.fake.settings.entries.at("web.sess.0");
	uint8_t hash[hc::Sha256::kDigestSize];
	hc::Sha256::hash(id.data(), id.size(), hash);
	char hex[hc::Sha256::kDigestSize * 2 + 1];
	hc::to_hex(hash, sizeof(hash), hex);
	// The frozen format: the creation order, '$', 64 hex digits of the hash.
	CHECK(stored.text == std::string("1$") + hex);
	CHECK(hc::setting_is_secret("web.sess.0"));
	CHECK(std::strlen("web.sess.3") <= hc::Settings::kMaxKeyLength);
}

TEST_CASE("AuthRemember: unreadable records are ignored and reused, a failed write refuses the login") {
	WebFixture f;
	f.onboard();
	f.fake.settings.set_string("web.sess.0", "garbage");
	f.fake.settings.set_string("web.sess.1", "7$00");
	f.fake.settings.set_string("web.sess.2", "x7$0000000000000000000000000000000000000000000000000000000000000000");
	f.fake.settings.set_string("web.sess.3", "4294967295$0000000000000000000000000000000000000000000000000000000000000000");
	{
		hc_test::ScopedCapture capture;
		Restart restart(f);
		CHECK(restart.auth.remembered_count() == 0);
		CHECK(capture.sink.contains("unreadable"));
		std::string cookie = remembered_login(f);
		CHECK(status_of(f, cookie) == 200);
		CHECK(f.fake.settings.entries.at("web.sess.0").text.rfind("1$", 0) == 0);

		f.fake.settings.fail_writes = true;
		auto refused = f.post("/api/auth/login", kRemember);
		CHECK(refused.status == 500);
		CHECK(refused.json["code"].text == "auth.storage_error");
		CHECK(refused.set_cookie.empty());
		CHECK(restart.auth.remembered_count() == 1);
		// A logout whose erase fails says so and still ends the session until the restart.
		auto out = f.post("/api/auth/logout", "{}", cookie);
		CHECK(out.status == 500);
		CHECK(out.json["code"].text == "auth.storage_error");
		CHECK(status_of(f, cookie) == 401);
		f.fake.settings.fail_writes = false;
	}
}
