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

// ---- The dashboard's view of the image (issue #14) ---------------------------------------------

#include "fake_blob_store.h"
#include "hc/image/image_service.h"
#include "hc/knx/adapter.h"
#include "hc/knx/tunnel.h"
#include "knx_test_support.h"
#include "rules_test_support.h"

namespace {

std::string image_fixture(const std::string& name) {
	std::string text;
	REQUIRE_MESSAGE(hc_test::read_text(HC_SPEC_DIR "/fixtures/images/" + name, text), "missing fixture " << name);
	return text;
}

/// The API with the image service, a KNX adapter without a gateway and an in-memory blob store.
struct DashboardFixture : WebFixture {
	hc_test::FakeUdpSocket socket{fake.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), fake.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{controller.loop(), fake.clock, model, tunnel};
	hc_test::FakeBlobStore blobs;
	hc::image::ImageService service{controller, model, blobs, adapter, &tunnel};
	std::string cookie;

	DashboardFixture() {
		fake.clock.wall_seconds = 1792231392; // 2026-10-17
		fake.clock.synchronized = true;
		controller.start();
		service.start();
		api.set_image(&service);
		cookie = onboard();
	}

	void load(const std::string& text) {
		REQUIRE(service.load(text.data(), text.size(), hc::image::Source::api, true));
	}

	Result put(const std::string& target, const std::string& body, bool csrf = true) {
		return request(HttpMethod::put, target, body, cookie, "", csrf);
	}
};

/// A small image: a virtual sensor with history, a fixed and an adjustable schedule entry, a rule
/// with a parameter of every kind the UI edits.
const char* kSmallImage = R"({
	"image": {"schemaVersion": "1.0", "projectId": "small", "revision": 3},
	"datapoints": [
		{"id": "temp", "label": "Temperature", "type": "real", "unit": "°C", "writable": true, "virtual": {},
		 "history": {"retention": 86400, "spacing": 60}},
		{"id": "lamp", "label": {"en": "Lamp", "de": "Lampe"}, "type": "boolean", "writable": true, "virtual": {}}
	],
	"functions": [
		{"id": "t", "label": "Temperature", "kind": "sensor", "datapoints": {"value": "temp"}},
		{"id": "l", "label": "Lamp", "kind": "light", "datapoints": {"command": "lamp", "unknownRole": "temp"}}
	],
	"schedules": [
		{"id": "fixed", "label": "Fixed", "kind": "weekly", "days": [1], "time": {"hour": 6, "minute": 0, "second": 0},
		 "enabledAdjustable": false},
		{"id": "free", "label": "Free", "kind": "weekly", "days": [2], "time": {"hour": 7, "minute": 0, "second": 0}}
	],
	"rules": [
		{"id": "r", "name": "Rule", "enabledAdjustable": false,
		 "parameters": [
			{"id": "level", "label": "Level", "type": "integer", "default": 5, "min": 0, "max": 10, "step": 1},
			{"id": "fixed", "label": "Fixed", "type": "boolean", "default": true, "adjustable": false},
			{"id": "mode", "label": "Mode", "type": "enumeration", "default": 1,
			 "choices": [{"value": 1, "label": "Eco"}, {"value": 2, "label": "Comfort"}]}
		 ],
		 "reactions": [{"triggers": [{"kind": "datapoint", "datapoint": "lamp"}],
			"actions": [{"kind": "write", "datapoint": "temp", "value": {"kind": "parameter", "id": "level"}}]}]}
	]
})";

} // namespace

TEST_CASE("WebApi.Dashboard without the image service or without an image") {
	WebFixture plain;
	std::string cookie = plain.onboard();
	for (const char* path : {"/api/model", "/api/history?ids=0", "/api/rules", "/api/schedules"}) {
		auto r = plain.get(path, cookie);
		CHECK(r.status == 404);
		CHECK(r.json["code"].text == "image.unavailable");
	}
	DashboardFixture f;
	CHECK(f.get("/api/model").status == 401);
	auto r = f.get("/api/model", f.cookie);
	REQUIRE(r.status == 200);
	CHECK_FALSE(r.json["loaded"].boolean);
	r = f.get("/api/rules", f.cookie);
	CHECK(r.status == 200);
	CHECK(r.json["count"].number == 0);
	r = f.get("/api/schedules", f.cookie);
	CHECK(r.json["count"].number == 0);
	CHECK(f.request(HttpMethod::post, "/api/model", "{}", f.cookie).status == 405);
	CHECK(f.get("/api/modelx", f.cookie).status == 404);
}

TEST_CASE("WebApi.Model of the example house: rooms, functions with roles, labels, units, bindings") {
	DashboardFixture f;
	f.load(image_fixture("example.json"));
	auto r = f.get("/api/model", f.cookie);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["loaded"].boolean);
	CHECK(r.json["project"].text == "example-house");
	CHECK(r.json["revision"].number == 7);
	CHECK(r.json["source"].text == "api");
	CHECK(r.json["next"].kind == hc_test::Json::Kind::null);
	MESSAGE("model view of the example house: " << f.service.view().bytes() << " heap bytes");
	CHECK(f.service.view().bytes() <= 8192);
	auto& locations = r.json["locations"].items;
	REQUIRE(locations.size() == 4);
	CHECK(locations[2]["id"].text == "living");
	CHECK(locations[2]["label"]["de"].text == "Wohnzimmer");
	CHECK(locations[2]["parent"].text == "ground");
	CHECK(locations[2]["kind"].text == "room");
	auto& functions = r.json["functions"].items;
	REQUIRE(functions.size() == 9);
	CHECK(functions[0]["kind"].text == "light");
	CHECK(functions[0]["room"].text == "living");
	CHECK(functions[0]["datapoints"]["command"].number == 0);
	CHECK(functions[0]["datapoints"]["state"].number == 1);
	CHECK(functions[3]["unit"].text == "°C");
	CHECK(functions[7]["scene"].number == 1);
	CHECK(functions[7]["room"].kind == hc_test::Json::Kind::null);
	auto& datapoints = r.json["datapoints"].items;
	REQUIRE(datapoints.size() == f.model.size());
	CHECK(datapoints[4]["name"].text == "living.temperature");
	CHECK(datapoints[4]["label"].text == "Living room temperature");
	CHECK(datapoints[4]["unit"].text == "°C");
	CHECK(datapoints[4]["history"].boolean);
	CHECK(datapoints[4]["type"].text == "real");
	CHECK_FALSE(datapoints[4]["writable"].boolean);
	CHECK(datapoints[5]["writable"].boolean);
	CHECK(datapoints[4]["technology"].text == "knx");
	CHECK(datapoints[4]["binding"]["knx"]["dpt"].text == "9.001");
	CHECK(datapoints[1]["label"]["en"].text == "Living room light (state)");
	CHECK(datapoints[13]["technology"].text == "scheduler");

	// Paging: one index space over locations, functions, datapoints.
	r = f.get("/api/model?from=5", f.cookie);
	REQUIRE(r.status == 200);
	CHECK(r.json["locations"].items.empty());
	REQUIRE(r.json["functions"].items.size() == 8);
	CHECK(r.json["functions"].items[0]["id"].text == "living-dimmer");
	uint32_t generation = static_cast<uint32_t>(r.json["generation"].number);

	// A new image: a new generation and the new model; unloading empties it.
	f.load(kSmallImage);
	r = f.get("/api/model", f.cookie);
	CHECK(r.json["generation"].number != generation);
	REQUIRE(r.json["functions"].items.size() == 2);
	CHECK(r.json["functions"].items[1]["datapoints"]["unknownRole"].number == 0); // kept: the UI ignores it
	CHECK(r.json["datapoints"].items[1]["binding"]["virtual"].kind == hc_test::Json::Kind::object);
	REQUIRE(f.request(HttpMethod::del, "/api/image", "", f.cookie).status == 200);
	r = f.get("/api/model", f.cookie);
	CHECK_FALSE(r.json["loaded"].boolean);
}

TEST_CASE("WebApi.History: buckets, refusals, a datapoint without history") {
	DashboardFixture f;
	f.load(kSmallImage);
	f.controller.run_once();
	// Samples: a value every 10 minutes for two hours.
	for (int i = 0; i < 12; ++i) {
		f.fake.clock.wall_seconds += 600;
		f.fake.clock.advance(600000);
		auto c = f.post("/api/datapoints/0/command", "{\"value\":" + std::to_string(20 + i) + ".5}", f.cookie);
		REQUIRE_MESSAGE(c.status == 200, c.body);
		f.controller.run_once();
	}
	int64_t now = f.fake.clock.wall_seconds;
	auto r = f.get("/api/history?ids=0,1&from=" + std::to_string(now - 7200) + "&to=" + std::to_string(now + 1) +
		"&points=4", f.cookie);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	auto& series = r.json["series"].items;
	REQUIRE(series.size() == 2);
	auto& points = series[0]["points"].items;
	REQUIRE(points.size() >= 3);
	REQUIRE(points.size() <= 4);
	// A bucket of several samples: [t, last, min, max, count].
	auto& last = points.back();
	REQUIRE(last.items.size() == 5);
	CHECK(last.items[1].number == doctest::Approx(31.5));
	CHECK(last.items[2].number <= last.items[1].number);
	CHECK(series[1]["code"].text == "history.none");

	// Defaults: the last 24 h, 48 points.
	r = f.get("/api/history?ids=0", f.cookie);
	REQUIRE(r.status == 200);
	CHECK(r.json["to"].number - r.json["from"].number == 86400);
	CHECK(r.json["series"].items[0]["points"].items.size() >= 1);

	CHECK(f.get("/api/history", f.cookie).json["code"].text == "history.bad_query");
	CHECK(f.get("/api/history?ids=a", f.cookie).status == 400);
	CHECK(f.get("/api/history?ids=0,1,2,3,4,5,6,7,8", f.cookie).status == 400);
	CHECK(f.get("/api/history?ids=0&points=0", f.cookie).status == 400);
	CHECK(f.get("/api/history?ids=0,1&points=81", f.cookie).status == 400);
	CHECK(f.get("/api/history?ids=0&from=10&to=5", f.cookie).status == 400);
	f.fake.clock.synchronized = false;
	r = f.get("/api/history?ids=0", f.cookie);
	CHECK(r.status == 409);
	CHECK(r.json["code"].text == "history.no_time");
}

TEST_CASE("WebApi.Rules: diagnostics, parameters with their ranges, set, reset, refusals") {
	DashboardFixture f;
	f.load(image_fixture("example.json"));
	auto r = f.get("/api/rules", f.cookie);
	REQUIRE(r.status == 200);
	REQUIRE(r.json["rules"].items.size() == 2);
	auto& rule = r.json["rules"].items[0];
	CHECK(rule["id"].text == "hall-presence-light");
	CHECK(rule["enabled"].boolean);
	CHECK(rule["enabledAdjustable"].boolean);
	REQUIRE(rule["parameters"].items.size() == 2);
	auto& timeout = rule["parameters"].items[0];
	CHECK(timeout["type"].text == "duration");
	CHECK(timeout["value"].number == 180000);
	CHECK(timeout["min"].number == 30000);
	CHECK(timeout["max"].number == 3600000);
	CHECK(timeout["step"].number == 30000);
	CHECK(timeout["unit"].text == "min");
	CHECK_FALSE(timeout["overridden"].boolean);

	// Set: the answer and the list show the override; it is persisted.
	r = f.put("/api/rules/hall-presence-light/parameters/timeout", "{\"value\":600000}");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["rule"]["parameters"].items[0]["value"].number == 600000);
	CHECK(r.json["rule"]["parameters"].items[0]["overridden"].boolean);
	r = f.get("/api/rules", f.cookie);
	CHECK(r.json["rules"].items[0]["parameters"].items[0]["value"].number == 600000);
	// The enable switch is parameter "enabled".
	r = f.put("/api/rules/hall-presence-light/parameters/enabled", "{\"value\":false}");
	REQUIRE(r.status == 200);
	CHECK_FALSE(r.json["rule"]["enabled"].boolean);
	CHECK(r.json["rule"]["enabledOverridden"].boolean);
	// Reset.
	r = f.put("/api/rules/hall-presence-light/parameters/timeout", "{\"reset\":true}");
	REQUIRE(r.status == 200);
	CHECK(r.json["rule"]["parameters"].items[0]["value"].number == 180000);
	CHECK_FALSE(r.json["rule"]["parameters"].items[0]["overridden"].boolean);

	// Refusals speak.
	r = f.put("/api/rules/hall-presence-light/parameters/timeout", "{\"value\":1}");
	CHECK(r.status == 422);
	CHECK(r.json["code"].text == "rule.invalid_value");
	CHECK(r.json["error"].text == "outside min..max");
	r = f.put("/api/rules/hall-presence-light/parameters/timeout", "{\"value\":\"long\"}");
	CHECK(r.status == 422);
	r = f.put("/api/rules/nothing/parameters/timeout", "{\"value\":1}");
	CHECK(r.status == 404);
	CHECK(r.json["code"].text == "rule.unknown");
	r = f.put("/api/rules/hall-presence-light/parameters/nothing", "{\"value\":1}");
	CHECK(r.status == 404);
	CHECK(r.json["code"].text == "rule.unknown_parameter");
	r = f.put("/api/rules/hall-presence-light/parameters/timeout", "{}");
	CHECK(r.status == 400);
	CHECK(r.json["code"].text == "rule.value_missing");
	r = f.put("/api/rules/hall-presence-light/parameters/timeout", "{\"value\":600000}", false);
	CHECK(r.status == 403);
	CHECK(r.json["code"].text == "auth.csrf");
	CHECK(f.request(HttpMethod::post, "/api/rules/hall-presence-light/parameters/timeout", "{}", f.cookie).status == 405);
	CHECK(f.put("/api/rules/hall-presence-light", "{}").status == 404);
	CHECK(f.put("/api/rules/hall-presence-light/parameters/", "{}").status == 404);
	CHECK(f.put("/api/rules/hall%2Dpresence-light/parameters/timeout", "{\"value\":60000}").status == 200);
	CHECK(f.put("/api/rules/hall%2/parameters/timeout", "{\"value\":60000}").status == 404);
}

TEST_CASE("WebApi.Rules: fixed parameters, choices, the enable switch not adjustable") {
	DashboardFixture f;
	f.load(kSmallImage);
	auto r = f.get("/api/rules", f.cookie);
	REQUIRE(r.status == 200);
	auto& rule = r.json["rules"].items[0];
	CHECK_FALSE(rule["enabledAdjustable"].boolean);
	auto& parameters = rule["parameters"].items;
	REQUIRE(parameters.size() == 3);
	CHECK(parameters[0]["step"].number == 1);
	CHECK_FALSE(parameters[1]["adjustable"].boolean);
	REQUIRE(parameters[2]["choices"].items.size() == 2);
	CHECK(parameters[2]["choices"].items[1]["label"].text == "Comfort");
	r = f.put("/api/rules/r/parameters/fixed", "{\"value\":false}");
	CHECK(r.status == 403);
	CHECK(r.json["code"].text == "rule.not_adjustable");
	r = f.put("/api/rules/r/parameters/enabled", "{\"value\":false}");
	CHECK(r.status == 403);
	r = f.put("/api/rules/r/parameters/mode", "{\"value\":3}");
	CHECK(r.status == 422);
	CHECK(r.json["error"].text == "not one of the choices");
	CHECK(f.put("/api/rules/r/parameters/mode", "{\"value\":2}").status == 200);
}

TEST_CASE("WebApi.Rules: labels pass through as the image gives them (spec/rules.md 2.2)") {
	DashboardFixture f;
	std::string text = image_fixture("example.json");
	auto replace = [&](const std::string& from, const std::string& to) {
		size_t at = text.find(from);
		REQUIRE(at != std::string::npos);
		text.replace(at, from.size(), to);
	};
	replace(R"("name": "Hall light by presence")", R"("name": {"en": "Hall light by presence", "de": "Flurlicht"})");
	replace(R"("label": "Switch-off delay")", R"("label": {"en": "Switch-off delay", "de": "Ausschaltverzögerung"})");
	f.load(text);
	auto r = f.get("/api/rules", f.cookie);
	REQUIRE(r.status == 200);
	auto& rule = r.json["rules"].items[0];
	CHECK(rule["name"]["en"].text == "Hall light by presence");
	CHECK(rule["name"]["de"].text == "Flurlicht");
	CHECK(rule["parameters"].items[0]["label"]["de"].text == "Ausschaltverzögerung");
	CHECK(rule["parameters"].items[1]["label"].text == "Dark below");
}

TEST_CASE("WebApi.Schedules: entries with their switches and next firings") {
	DashboardFixture f;
	f.load(kSmallImage);
	f.controller.run_once();
	auto r = f.get("/api/schedules", f.cookie);
	REQUIRE(r.status == 200);
	CHECK(r.json["synchronized"].boolean);
	auto& entries = r.json["schedules"].items;
	REQUIRE(entries.size() == 2);
	CHECK(entries[0]["id"].text == "fixed");
	CHECK_FALSE(entries[0]["adjustable"].boolean);
	CHECK(entries[1]["adjustable"].boolean);
	CHECK(entries[1]["kind"].text == "weekly");
	CHECK(entries[1]["enabled"].boolean);
	CHECK(entries[1]["nextFiring"].number > f.fake.clock.wall_seconds);

	r = f.put("/api/schedules/free", "{\"enabled\":false}");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK_FALSE(r.json["schedule"]["enabled"].boolean);
	CHECK(r.json["schedule"]["nextFiring"].kind == hc_test::Json::Kind::null);
	CHECK_FALSE(f.get("/api/schedules", f.cookie).json["schedules"].items[1]["enabled"].boolean);

	r = f.put("/api/schedules/fixed", "{\"enabled\":false}");
	CHECK(r.status == 403);
	CHECK(r.json["code"].text == "schedule.not_adjustable");
	CHECK(r.json["error"].text.size() > 0);
	r = f.put("/api/schedules/nothing", "{\"enabled\":false}");
	CHECK(r.status == 404);
	CHECK(r.json["code"].text == "schedule.unknown");
	r = f.put("/api/schedules/free", "{\"enabled\":1}");
	CHECK(r.status == 400);
	CHECK(r.json["code"].text == "schedule.value_missing");
	CHECK(f.put("/api/schedules/free", "{\"enabled\":true}", false).status == 403);
	CHECK(f.put("/api/schedules/free/x", "{\"enabled\":true}").status == 404);
	CHECK(f.request(HttpMethod::post, "/api/schedules/free", "{}", f.cookie).status == 405);
}
