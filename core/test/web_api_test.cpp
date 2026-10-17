/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <string>

#include "doctest.h"
#include "hc/network_settings.h"
#include "hc/web_api.h"
#include "web_test_support.h"

using hc::DatapointFlags;
using hc::DatapointSpec;
using hc::HttpMethod;
using hc::ValueType;
using hc_test::WebFixture;

namespace {

void load_model(WebFixture& f, size_t count = 3) {
	static std::vector<std::string> names;
	static std::vector<DatapointSpec> specs;
	names.clear();
	specs.clear();
	names.reserve(count);
	for (size_t i = 0; i < count; ++i) {
		names.push_back("Datapoint " + std::to_string(i));
	}
	for (size_t i = 0; i < count; ++i) {
		ValueType type = i == 0 ? ValueType::boolean : i == 1 ? ValueType::real : ValueType::unsigned_int;
		uint8_t flags = i == 2 ? DatapointFlags::kReadable : DatapointFlags::kWritable;
		specs.push_back({names[i].c_str(), type, flags});
	}
	REQUIRE(f.model.load(specs.data(), specs.size()) == hc::DatapointStatus::ok);
}

} // namespace

TEST_CASE("WebApi without a password serves only the onboarding") {
	WebFixture f;
	auto session = f.get("/api/auth/session");
	CHECK(session.status == 200);
	CHECK(session.json["onboarded"].boolean == false);
	CHECK(session.json["authenticated"].boolean == false);
	CHECK(session.json["device"].text == "homecontrol-a1b2c3");
	auto status = f.get("/api/status");
	CHECK(status.status == 403);
	CHECK(status.json["code"].text == "auth.onboarding_required");
	CHECK(status.json["error"].text.find("onboarding") != std::string::npos);
	auto login = f.post("/api/auth/login", "{\"password\":\"whatever1\"}");
	CHECK(login.status == 409);
	CHECK(login.json["code"].text == "auth.onboarding_required");
}

TEST_CASE("WebApi onboarding sets the password once and opens a session") {
	WebFixture f;
	auto shortpw = f.post("/api/auth/onboard", "{\"password\":\"short\"}");
	CHECK(shortpw.status == 400);
	CHECK(shortpw.json["code"].text == "auth.password_invalid");
	auto nocsrf = f.post("/api/auth/onboard", "{\"password\":\"correct horse\"}", "", "", false);
	CHECK(nocsrf.status == 403);
	CHECK(nocsrf.json["code"].text == "auth.csrf");
	auto badjson = f.post("/api/auth/onboard", "{password}");
	CHECK(badjson.status == 400);
	CHECK(badjson.json["code"].text == "api.bad_json");

	auto r = f.post("/api/auth/onboard", "{\"password\":\"correct horse\"}");
	REQUIRE(r.status == 200);
	CHECK(r.cookie.find("hc_session=") == 0);
	std::string cookie = r.cookie;
	auto again = f.post("/api/auth/onboard", "{\"password\":\"other password\"}");
	CHECK(again.status == 409);
	CHECK(again.json["code"].text == "auth.password_set");
	auto session = f.get("/api/auth/session", cookie);
	CHECK(session.json["authenticated"].boolean == true);
	CHECK(session.json["via"].text == "session");
	CHECK(f.get("/api/status", cookie).status == 200);
}

TEST_CASE("WebApi login, wrong password with a visible reason, rate limit and logout") {
	WebFixture f;
	f.onboard();
	CHECK(f.get("/api/status").status == 401);
	CHECK(f.get("/api/status").json["code"].text == "auth.required");
	auto wrong = f.post("/api/auth/login", "{\"password\":\"wrong password\"}");
	CHECK(wrong.status == 401);
	CHECK(wrong.json["code"].text == "auth.wrong_password");
	CHECK(wrong.json["error"].text == "wrong password");
	CHECK(wrong.json["failures"].number == 1);
	for (int i = 0; i < 4; ++i) {
		wrong = f.post("/api/auth/login", "{\"password\":\"wrong password\"}");
	}
	CHECK(wrong.status == 401);
	CHECK(wrong.retry_after == 30);
	auto blocked = f.post("/api/auth/login", "{\"password\":\"correct horse\"}");
	CHECK(blocked.status == 429);
	CHECK(blocked.json["code"].text == "auth.rate_limited");
	CHECK(blocked.retry_after == 30);
	f.fake.clock.advance(30000);
	auto ok = f.post("/api/auth/login", "{\"password\":\"correct horse\"}");
	REQUIRE(ok.status == 200);
	std::string cookie = ok.cookie;
	CHECK(f.get("/api/status", cookie).status == 200);
	auto out = f.post("/api/auth/logout", "{}", cookie);
	CHECK(out.status == 200);
	CHECK(out.cookie == "hc_session=");
	CHECK(f.get("/api/status", cookie).status == 401);
}

TEST_CASE("WebApi state-changing requests need the CSRF header unless a bearer token is used") {
	WebFixture f;
	std::string cookie = f.onboard();
	load_model(f);
	auto r = f.request(HttpMethod::post, "/api/datapoints/0/command", "{\"value\":true}", cookie, "", false);
	CHECK(r.status == 403);
	CHECK(r.json["code"].text == "auth.csrf");
	char token[hc::Auth::kTokenText];
	REQUIRE(f.auth.create_token("ehome", token) == hc::AuthStatus::ok);
	std::string bearer = std::string("Bearer ") + token;
	r = f.request(HttpMethod::post, "/api/datapoints/0/command", "{\"value\":true}", "", bearer, false);
	CHECK(r.status == 200);
	CHECK(f.get("/api/auth/session", "", bearer).json["via"].text == "token");
	r = f.request(HttpMethod::get, "/api/status", "", "", "Bearer hct_00000000000000000000000000000000");
	CHECK(r.status == 401);
	CHECK(r.json["code"].text == "auth.token_invalid");
}

TEST_CASE("WebApi status carries the device, network, time, memory, Improv, update and LED state") {
	WebFixture f;
	std::string cookie = f.onboard();
	f.controller.start();
	auto r = f.get("/api/status", cookie);
	REQUIRE(r.status == 200);
	const hc_test::Json& j = r.json;
	CHECK(j["device"]["name"].text == "homecontrol-a1b2c3");
	CHECK(j["device"]["version"].text == "0.1.0-dev");
	CHECK(j["device"]["ui"].text == "abc123");
	CHECK(j["network"].has("state"));
	CHECK(j["network"]["mdns"].text == "homecontrol-a1b2c3.local");
	CHECK(j["time"].has("synchronized"));
	CHECK(j["time"]["tz"].text == hc::setting::kDefaultTimeZone);
	CHECK(j["memory"]["free"].number == 300000);
	CHECK(j["memory"]["min_free"].number == 250000);
	CHECK(j["improv"].has("active"));
	CHECK(j["update"]["available"].boolean == false);
	CHECK(j["led"].kind == hc_test::Json::Kind::string);
	CHECK(j["web"]["sessions"].number == 1);
	CHECK(j.has("uptime_s"));
}

TEST_CASE("WebApi lists datapoints in pages and commands them with reasons") {
	WebFixture f;
	std::string cookie = f.onboard();
	load_model(f, 120);
	auto first = f.get("/api/datapoints", cookie);
	REQUIRE(first.status == 200);
	CHECK(first.json["count"].number == 120);
	CHECK(first.json["datapoints"].items.size() == hc::WebApi::kPageSize);
	CHECK(first.json["next"].number == hc::WebApi::kPageSize);
	const hc_test::Json& dp = first.json["datapoints"].items[0];
	CHECK(dp["id"].number == 0);
	CHECK(dp["name"].text == "Datapoint 0");
	CHECK(dp["type"].text == "boolean");
	CHECK(dp["writable"].boolean == true);
	CHECK(dp["quality"].text == "unknown");
	CHECK(dp["value"].kind == hc_test::Json::Kind::null);
	auto last = f.get("/api/datapoints?from=100", cookie);
	CHECK(last.json["datapoints"].items.size() == 20);
	CHECK(last.json["next"].kind == hc_test::Json::Kind::null);

	auto ok = f.post("/api/datapoints/1/command", "{\"value\": 21.5}", cookie);
	REQUIRE(ok.status == 200);
	CHECK(ok.json["datapoint"]["value"].number == 21.5);
	CHECK(ok.json["datapoint"]["quality"].text == "good");
	CHECK(f.get("/api/datapoints/1", cookie).json["value"].number == 21.5);

	auto wrong = f.post("/api/datapoints/0/command", "{\"value\": 1}", cookie);
	CHECK(wrong.status == 422);
	CHECK(wrong.json["code"].text == "dp.wrong_type");
	auto readonly = f.post("/api/datapoints/2/command", "{\"value\": 1}", cookie);
	CHECK(readonly.status == 409);
	CHECK(readonly.json["code"].text == "dp.not_writable");
	auto unknown = f.post("/api/datapoints/500/command", "{\"value\": 1}", cookie);
	CHECK(unknown.status == 404);
	auto missing = f.post("/api/datapoints/0/command", "{}", cookie);
	CHECK(missing.status == 400);
	CHECK(missing.json["code"].text == "dp.value_missing");
	CHECK(f.get("/api/datapoints/0/command", cookie).status == 405);
	CHECK(f.get("/api/datapoints/01", cookie).status == 404);
	CHECK(f.get("/api/nothing", cookie).status == 404);
}

TEST_CASE("WebApi log returns the mirrored lines and pages with after") {
	WebFixture f;
	std::string cookie = f.onboard();
	for (int i = 0; i < 5; ++i) {
		hc::Log::info("hc.test", "line %d", i);
	}
	auto r = f.get("/api/log", cookie);
	REQUIRE(r.status == 200);
	const auto& lines = r.json["lines"].items;
	REQUIRE(lines.size() >= 5);
	CHECK(lines.back()["message"].text == "line 4");
	CHECK(lines.back()["tag"].text == "hc.test");
	CHECK(lines.back()["level"].text == "I");
	uint32_t after = static_cast<uint32_t>(lines[lines.size() - 2]["seq"].number);
	auto tail = f.get("/api/log?after=" + std::to_string(after), cookie);
	REQUIRE(tail.json["lines"].items.size() == 1);
	CHECK(tail.json["lines"].items[0]["message"].text == "line 4");
	CHECK(tail.json["more"].boolean == false);
}

TEST_CASE("WebApi settings show the non-secret values and validate changes like the console") {
	WebFixture f;
	std::string cookie = f.onboard();
	f.fake.settings.set_string(hc::setting::kWifiSsid, "Example Net");
	f.fake.settings.set_string(hc::setting::kWifiPsk, "secret-key-1");
	auto r = f.get("/api/settings", cookie);
	REQUIRE(r.status == 200);
	CHECK(r.body.find("secret-key-1") == std::string::npos);
	CHECK(r.body.find("pbkdf2") == std::string::npos);
	bool ssid = false;
	for (const auto& s : r.json["settings"].items) {
		if (s["key"].text == "wifi.ssid") {
			ssid = true;
			CHECK(s["value"].text == "Example Net");
			CHECK(s["editable"].boolean == false);
		}
		CHECK(s["key"].text != "wifi.psk");
	}
	CHECK(ssid);

	auto bad = f.request(HttpMethod::put, "/api/settings/tz", "{\"value\":\"CET-1CEST\"}", cookie);
	CHECK(bad.status == 422);
	CHECK(bad.json["code"].text == "settings.invalid");
	auto good = f.request(HttpMethod::put, "/api/settings/tz", "{\"value\":\"EST5EDT,M3.2.0,M11.1.0\"}", cookie);
	CHECK(good.status == 200);
	CHECK(std::string(f.controller.time_zone()) == "EST5EDT,M3.2.0,M11.1.0");
	auto ntp = f.request(HttpMethod::put, "/api/settings/ntp.server", "{\"value\":\"ntp.example.org\"}", cookie);
	CHECK(ntp.status == 200);
	CHECK(f.host.ntp_changes == 1);
	CHECK(f.fake.settings.entries["ntp.server"].text == "ntp.example.org");
	auto badntp = f.request(HttpMethod::put, "/api/settings/ntp.server", "{\"value\":\"bad host!\"}", cookie);
	CHECK(badntp.status == 422);
	auto url = f.request(HttpMethod::put, "/api/settings/update.url", "{\"value\":\"ftp://x\"}", cookie);
	CHECK(url.status == 422);
	CHECK(url.json["error"].text == "not an http:// or https:// URL");
	auto ro = f.request(HttpMethod::put, "/api/settings/wifi.ssid", "{\"value\":\"x\"}", cookie);
	CHECK(ro.status == 403);
	CHECK(ro.json["code"].text == "settings.not_editable");
	CHECK(f.request(HttpMethod::put, "/api/settings/web.pw", "{\"value\":\"x\"}", cookie).status == 404);
	CHECK(f.request(HttpMethod::put, "/api/settings/tz", "{\"value\":1}", cookie).status == 400);
}

TEST_CASE("WebApi query parameters parse strictly") {
	uint32_t v = 0;
	CHECK(hc::query_uint("from=12&x=1", "from", v));
	CHECK(v == 12);
	CHECK(hc::query_uint("x=1&from=7", "from", v));
	CHECK(v == 7);
	CHECK_FALSE(hc::query_uint("from=-1", "from", v));
	CHECK_FALSE(hc::query_uint("from=1x", "from", v));
	CHECK_FALSE(hc::query_uint("fromage=1", "from", v));
	CHECK_FALSE(hc::query_uint("", "from", v));
}
