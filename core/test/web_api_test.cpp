/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <string>

#include "doctest.h"
#include "hc/version.h"
#include "hc/network_settings.h"
#include "hc/blocks/room_controller.h"
#include "hc/ehome_link.h"
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
	CHECK(j["device"]["version"].text == hc::kVersion);
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

TEST_CASE("WebApi.Model passes a remote's presses on (image 1.4, haumacher/ehome #70)") {
	DashboardFixture f;
	f.load(R"({"image": {"schemaVersion": "1.4", "projectId": "remote", "revision": 1},
		"datapoints": [{"id": "hall.remote", "type": "enumeration",
		                "zigbee": {"ieee": "000b57fffe112233", "endpoint": 1, "cluster": 8, "client": true}},
		               {"id": "bell", "type": "boolean", "writable": true, "virtual": {}}],
		"functions": [{"id": "remote", "label": "Hall remote", "kind": "button",
		               "presses": {"on": [1, 2], "off": [0], "up": [3], "down": [4], "release": [5]},
		               "datapoints": {"press": "hall.remote"}},
		              {"id": "bell", "label": "Bell", "kind": "button", "datapoints": {"press": "bell"}}]})");
	auto r = f.get("/api/model", f.cookie);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	auto& functions = r.json["functions"].items;
	REQUIRE(functions.size() == 2);
	CHECK(functions[0]["kind"].text == "button");
	CHECK(functions[0]["datapoints"]["press"].number == 0);
	REQUIRE(functions[0]["presses"]["on"].items.size() == 2);
	CHECK(functions[0]["presses"]["on"].items[1].number == 2);
	CHECK(functions[0]["presses"]["release"].items[0].number == 5);
	CHECK(functions[0]["presses"]["press"].kind == hc_test::Json::Kind::null);
	// A boolean press has none, as before.
	CHECK(functions[1]["presses"].kind == hc_test::Json::Kind::null);
	auto& datapoints = r.json["datapoints"].items;
	CHECK(datapoints[0]["type"].text == "enumeration");
	CHECK_FALSE(datapoints[0]["writable"].boolean);
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
	// The rule parameters' datapoints (spec/rules.md 8.6): the parameter's label and unit, the enable
	// switch the rule's name, the technology `rules`.
	CHECK(datapoints[17]["name"].text == "rule/hall-presence-light/enabled");
	CHECK(datapoints[17]["technology"].text == "rules");
	CHECK(datapoints[17]["label"].text == "Hall light by presence");
	CHECK(datapoints[17]["parameter"]["rule"].text == "hall-presence-light");
	CHECK(datapoints[17]["parameter"]["parameter"].text == "enabled");
	CHECK(datapoints[17]["writable"].boolean);
	CHECK(datapoints[18]["label"].text == "Switch-off delay");
	CHECK(datapoints[18]["type"].text == "unsigned");
	CHECK(datapoints[18]["unit"].text == "ms");
	CHECK(datapoints[19]["unit"].text == "lx");
	CHECK(datapoints[19]["parameter"]["parameter"].text == "darkBelow");
	CHECK(datapoints[4]["parameter"].kind == hc_test::Json::Kind::null);
	// Their values and a command, which sets the parameter as PUT /api/rules/.../parameters/... does.
	auto value = f.get("/api/datapoints/19", f.cookie);
	REQUIRE(value.status == 200);
	CHECK(value.json["technology"].text == "rules");
	CHECK(value.json["value"].number == 80.0);
	CHECK(value.json["quality"].text == "good");
	auto command = f.request(HttpMethod::post, "/api/datapoints/19/command", "{\"value\": 120.5}", f.cookie);
	REQUIRE_MESSAGE(command.status == 200, command.body);
	auto rules = f.get("/api/rules", f.cookie);
	REQUIRE(rules.status == 200);
	CHECK(rules.json["rules"].items[0]["parameters"].items[1]["value"].number == 120.5);
	CHECK(rules.json["rules"].items[0]["parameters"].items[1]["overridden"].boolean);
	CHECK(f.request(HttpMethod::post, "/api/datapoints/19/command", "{\"value\": 5000}", f.cookie).status != 200);
	CHECK(f.get("/api/datapoints/19", f.cookie).json["value"].number == 120.5);
	auto put = f.request(HttpMethod::put, "/api/rules/hall-presence-light/parameters/darkBelow", "{\"value\": 60}", f.cookie);
	REQUIRE_MESSAGE(put.status == 200, put.body);
	f.controller.run_once();
	CHECK(f.get("/api/datapoints/19", f.cookie).json["value"].number == 60.0);
	r = f.get("/api/model", f.cookie);

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

/// Archives (issue #56): an hourly one for the temperature, a daily one for the boolean.
const char* kArchiveImage = R"({
	"image": {"schemaVersion": "1.2", "projectId": "archives", "revision": 1},
	"datapoints": [
		{"id": "temp", "type": "real", "writable": true, "virtual": {},
		 "history": {"retention": 7200, "spacing": 60, "archives": [{"resolution": 3600, "retention": 604800}]}},
		{"id": "heat", "type": "boolean", "writable": true, "virtual": {},
		 "history": {"retention": 7200, "spacing": 60, "archives": [{"resolution": 3600, "retention": 604800}]}}
	]
})";

TEST_CASE("WebApi.History: an archive answers with its resolution; a boolean has on_s; a full page is truncated") {
	DashboardFixture f;
	f.load(kArchiveImage);
	f.controller.run_once();
	int64_t begin = f.fake.clock.wall_seconds;
	// Six hours: the temperature alternates 20 / 22 every 10 minutes, the heating is on half of it.
	for (int i = 0; i < 36; ++i) {
		auto t = f.post("/api/datapoints/0/command", i % 2 == 0 ? "{\"value\":20}" : "{\"value\":22}", f.cookie);
		REQUIRE_MESSAGE(t.status == 200, t.body);
		auto h = f.post("/api/datapoints/1/command", i % 2 == 0 ? "{\"value\":true}" : "{\"value\":false}", f.cookie);
		REQUIRE_MESSAGE(h.status == 200, h.body);
		for (int s = 0; s < 600; s += 10) {
			f.fake.clock.wall_seconds += 10;
			f.fake.clock.advance(10000);
			f.controller.run_once();
		}
	}
	int64_t now = f.fake.clock.wall_seconds;
	// Six hourly buckets over the six hours: wider than the raw spacing and beyond the raw ring.
	auto r = f.get("/api/history?ids=0,1&from=" + std::to_string(begin) + "&to=" + std::to_string(now) + "&points=6",
		f.cookie);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	auto& series = r.json["series"].items;
	REQUIRE(series.size() == 2);
	CHECK(series[0]["store"].number == 3600);
	CHECK(series[0]["retention_s"].number == 604800);
	auto& temp = series[0]["points"].items;
	REQUIRE(temp.size() >= 5);
	auto& middle = temp[2].items;
	REQUIRE(middle.size() == 6); // [t, last, min, max, count, mean]
	CHECK(middle[2].number == 20.0);
	CHECK(middle[3].number == 22.0);
	CHECK(middle[5].number == doctest::Approx(21.0).epsilon(0.01)); // time-weighted
	auto& heat = series[1]["points"].items;
	REQUIRE(heat.size() >= 5);
	REQUIRE(heat[2].items.size() == 7); // and on_s
	CHECK(heat[2].items[6].number == doctest::Approx(1800).epsilon(0.02));
	CHECK(heat[2].items[5].number == doctest::Approx(0.5).epsilon(0.02));
	CHECK_FALSE(series[0]["truncated"].boolean);

	// Finer buckets than the archive: the raw ring answers what it still has.
	r = f.get("/api/history?ids=0&from=" + std::to_string(now - 3600) + "&to=" + std::to_string(now) + "&points=12",
		f.cookie);
	REQUIRE(r.status == 200);
	CHECK(r.json["series"].items[0]["store"].text == "raw");

	// Two hours of a value of the longest notation every minute: 160 full points of the raw ring exceed
	// the response buffer of 8 KB, the series ends early, still JSON. (Two hours of values like 20.33333
	// take about 7 KB: 44 bytes a point.)
	for (int i = 0; i < 120; ++i) {
		const char* body = i % 2 == 0 ? "{\"value\":-1.2345678e25}" : "{\"value\":-1.2345679e25}";
		auto t = f.post("/api/datapoints/0/command", body, f.cookie);
		REQUIRE_MESSAGE(t.status == 200, t.body);
		for (int s = 0; s < 60; s += 10) {
			f.fake.clock.wall_seconds += 10;
			f.fake.clock.advance(10000);
			f.controller.run_once();
		}
	}
	now = f.fake.clock.wall_seconds;
	r = f.get("/api/history?ids=0,0&from=" + std::to_string(now - 7200) + "&to=" + std::to_string(now) + "&points=80",
		f.cookie);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	bool truncated = false;
	for (auto& s : r.json["series"].items) {
		truncated = truncated || s["truncated"].boolean;
	}
	CHECK(truncated);
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
	// A bucket of several samples: [t, last, min, max, count, mean] (the mean since issue #54).
	auto& last = points.back();
	REQUIRE(last.items.size() == 6);
	CHECK(last.items[1].number == doctest::Approx(31.5));
	CHECK(last.items[2].number <= last.items[1].number);
	CHECK(last.items[5].number >= last.items[2].number);
	CHECK(last.items[5].number <= last.items[3].number);
	CHECK(last.items[5].number < last.items[3].number); // rising samples: the mean is below the maximum
	// The store that answered: the raw ring, with its reach (the seam of the archives, issue #56).
	CHECK(series[0]["store"].text == "raw");
	CHECK(series[0]["retention_s"].number > 0);
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
	CHECK_FALSE(entries[0]["enabledAdjustable"].boolean);
	CHECK(entries[1]["enabledAdjustable"].boolean);
	CHECK(entries[1]["adjustable"].items.empty());
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

namespace {

/// An image with an adjustable weekly entry (issue #58) and a rule that reads its time datapoint.
const char* kAdjustableImage = R"({
	"image": {"schemaVersion": "1.2", "projectId": "adjust", "revision": 1},
	"datapoints": [{"id": "early", "type": "boolean", "writable": true, "virtual": {}}],
	"schedules": [
		{"id": "morning", "label": "Morning", "kind": "weekly", "days": [1, 2, 3, 4, 5],
		 "time": {"hour": 6, "minute": 30, "second": 0}, "adjustable": ["time", "days"],
		 "bounds": {"time": {"min": {"hour": 5, "minute": 0, "second": 0}, "max": {"hour": 9, "minute": 0, "second": 0},
		                     "step": 300}}},
		{"id": "blinds", "label": "Blinds", "kind": "astro", "sun": "sunset", "offset": 0, "adjustable": ["offset"],
		 "bounds": {"offset": {"min": -3600000, "max": 3600000, "step": 300000}}}
	],
	"rules": [
		{"id": "watch", "name": "Watch",
		 "reactions": [{"triggers": [{"kind": "datapoint", "datapoint": "schedule/morning/time"}],
			"actions": [{"kind": "write", "datapoint": "early", "value": {"kind": "binary", "op": "<",
				"left": {"kind": "datapoint", "name": "schedule/morning/time"},
				"right": {"kind": "literal", "type": "timeOfDay", "value": {"hour": 6, "minute": 15, "second": 0}}}}]}]}
	]
})";

} // namespace

TEST_CASE("WebApi.Schedules: an output takes its value at the load quietly (issue #67)") {
	DashboardFixture f;
	f.load(R"({
		"image": {"schemaVersion": "1.2", "projectId": "output", "revision": 1},
		"datapoints": [{"id": "fired", "type": "boolean", "virtual": {}},
		               {"id": "seen", "type": "boolean", "writable": true, "virtual": {}}],
		"schedules": [{"id": "evening", "label": "Evening", "kind": "weekly", "days": [1, 2, 3, 4, 5, 6, 7],
		               "time": {"hour": 23, "minute": 59, "second": 59}, "output": "fired"}],
		"rules": [{"id": "watch", "name": "Watch",
			"reactions": [{"triggers": [{"kind": "datapoint", "datapoint": "fired"}],
				"actions": [{"kind": "write", "datapoint": "seen", "value": {"kind": "literal", "type": "boolean", "value": true}}]}]}]
	})");
	f.controller.run_once();
	f.controller.run_once();
	const hc::Datapoint* fired = f.model.find("fired");
	REQUIRE(fired != nullptr);
	CHECK(fired->quality() == hc::Quality::good);
	CHECK(f.model.find("seen")->quality() == hc::Quality::unknown);
}

TEST_CASE("WebApi.Schedules: adjustable members, bounds, overrides and their refusals") {
	DashboardFixture f;
	f.load(kAdjustableImage);
	f.controller.run_once();
	// The time datapoint took its value at the load quietly: the rule following it did not react.
	const hc::Datapoint* time_dp = f.model.find("schedule/morning/time");
	REQUIRE(time_dp != nullptr);
	CHECK(time_dp->quality() == hc::Quality::good);
	CHECK(f.model.find("early")->quality() == hc::Quality::unknown);
	auto r = f.get("/api/schedules", f.cookie);
	REQUIRE(r.status == 200);
	auto& entry = r.json["schedules"].items[0];
	REQUIRE(entry["adjustable"].items.size() == 2);
	CHECK(entry["adjustable"].items[0].text == "time");
	CHECK(entry["adjustable"].items[1].text == "days");
	CHECK(entry["overridden"].items.empty());
	CHECK(entry["values"]["time"]["hour"].number == 6);
	CHECK(entry["values"]["time"]["minute"].number == 30);
	CHECK(entry["values"]["days"].items.size() == 5);
	CHECK(entry["image"]["time"]["minute"].number == 30);
	CHECK(entry["bounds"]["time"]["min"]["hour"].number == 5);
	CHECK(entry["bounds"]["time"]["max"]["hour"].number == 9);
	CHECK(entry["bounds"]["time"]["step"].number == 300);
	CHECK_FALSE(entry["bounds"].has("days"));
	auto& blinds = r.json["schedules"].items[1];
	CHECK(blinds["values"]["offset"].number == 0);
	CHECK(blinds["bounds"]["offset"]["min"].number == -3600000);
	CHECK(blinds["bounds"]["offset"]["step"].number == 300000);

	r = f.put("/api/schedules/morning", R"({"time": {"hour": 6, "minute": 0, "second": 0}})");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["schedule"]["overridden"].items.size() == 1);
	CHECK(r.json["schedule"]["values"]["time"]["hour"].number == 6);
	CHECK(r.json["schedule"]["values"]["time"]["minute"].number == 0);
	CHECK(r.json["schedule"]["image"]["time"]["minute"].number == 30);

	// The datapoint followed, and the rule reading it saw the new time.
	r = f.get("/api/model", f.cookie);
	REQUIRE(r.status == 200);
	bool found = false;
	for (auto& dp : r.json["datapoints"].items) {
		if (dp["name"].text == "schedule/morning/time") {
			found = true;
			CHECK(dp["technology"].text == "scheduler");
			CHECK(dp["writable"].boolean);
		}
	}
	CHECK(found);
	f.controller.run_once();
	const hc::Datapoint* early = f.model.find("early");
	REQUIRE(early != nullptr);
	CHECK(early->value() == hc::Value::of_bool(true));

	auto refusal = [&](const char* body, int status, const char* code, const char* member) {
		auto result = f.put("/api/schedules/morning", body);
		CHECK_MESSAGE(result.status == status, body << " " << result.body);
		CHECK_MESSAGE(result.json["code"].text == code, body << " " << result.body);
		if (member != nullptr) {
			CHECK_MESSAGE(result.json["member"].text == member, body << " " << result.body);
		}
	};
	hc_test::ScopedCapture capture;
	refusal(R"({"time": {"hour": 4, "minute": 55, "second": 0}})", 400, "schedule.out_of_bounds", "time");
	refusal(R"({"time": {"hour": 6, "minute": 2, "second": 0}})", 400, "schedule.out_of_bounds", "time");
	refusal(R"({"time": {"hour": 25, "minute": 0, "second": 0}})", 400, "schedule.malformed", "time");
	refusal(R"({"days": []})", 400, "schedule.malformed", "days");
	refusal(R"({"days": [1, 1]})", 400, "schedule.malformed", "days");
	refusal(R"({"offset": 0})", 403, "schedule.not_adjustable", "offset");
	refusal(R"({"cron": "*"})", 400, "schedule.malformed", nullptr);
	refusal(R"({"reset": true, "days": [1]})", 400, "schedule.malformed", nullptr);
	refusal(R"({})", 400, "schedule.value_missing", nullptr);
	refusal(R"({"days": [1], "time": {"hour": 4, "minute": 0, "second": 0}})", 400, "schedule.out_of_bounds", "time");
	CHECK(f.get("/api/schedules", f.cookie).json["schedules"].items[0]["values"]["days"].items.size() == 5);

	r = f.put("/api/schedules/morning", R"({"days": [1, 2, 3, 4], "enabled": false})");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["schedule"]["overridden"].items.size() == 2);
	CHECK(r.json["schedule"]["enabledOverridden"].boolean);
	r = f.put("/api/schedules/blinds", R"({"offset": -1800000})");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["schedule"]["values"]["offset"].number == -1800000);

	// DELETE .../override and PUT {"reset": true} return to the image's values.
	r = f.request(HttpMethod::del, "/api/schedules/morning/override", "", f.cookie);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["schedule"]["overridden"].items.empty());
	CHECK_FALSE(r.json["schedule"]["enabledOverridden"].boolean);
	CHECK(r.json["schedule"]["values"]["time"]["minute"].number == 30);
	r = f.put("/api/schedules/blinds", R"({"reset": true})");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["schedule"]["values"]["offset"].number == 0);
	CHECK(f.request(HttpMethod::del, "/api/schedules/nothing/override", "", f.cookie).status == 404);
	CHECK(f.request(HttpMethod::put, "/api/schedules/morning/override", "{}", f.cookie).status == 405);
	CHECK(f.request(HttpMethod::del, "/api/schedules/morning/other", "", f.cookie).status == 404);
}

TEST_CASE("WebApi.Schedules: a command to schedule/<id>/time is a PUT") {
	DashboardFixture f;
	f.load(kAdjustableImage);
	f.controller.run_once();
	const hc::Datapoint* dp = f.model.find("schedule/morning/time");
	REQUIRE(dp != nullptr);
	char path[64];
	std::snprintf(path, sizeof(path), "/api/datapoints/%u/command", static_cast<unsigned>(dp->id()));
	auto r = f.request(HttpMethod::post, path, R"({"value": {"day": 0, "hour": 7, "minute": 15, "second": 0}})", f.cookie);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	f.controller.run_once();
	r = f.get("/api/schedules", f.cookie);
	CHECK(r.json["schedules"].items[0]["values"]["time"]["hour"].number == 7);
	CHECK(r.json["schedules"].items[0]["overridden"].items.size() == 1);
	hc_test::ScopedCapture capture;
	r = f.request(HttpMethod::post, path, R"({"value": {"day": 0, "hour": 9, "minute": 30, "second": 0}})", f.cookie);
	CHECK(r.status != 200);
	CHECK(f.get("/api/schedules", f.cookie).json["schedules"].items[0]["values"]["time"]["hour"].number == 7);
}

// ---- Blocks (issue #60, spec/blocks.md, spec/api.md "Blocks") ------------------------------------

namespace {

/// A room controller with a KNX-like virtual temperature, a valve, a window pair and a rule that
/// follows the controller's state.
const char* kBlocksImage = R"({
	"image": {"schemaVersion": "1.3", "projectId": "blocks", "revision": 1},
	"datapoints": [{"id": "temp", "type": "real", "writable": true, "virtual": {}},
	               {"id": "valve", "type": "real", "writable": true, "virtual": {}},
	               {"id": "win1", "type": "boolean", "writable": true, "virtual": {}},
	               {"id": "win2", "type": "boolean", "writable": true, "virtual": {}},
	               {"id": "heating", "type": "boolean", "writable": true, "virtual": {}}],
	"blocks": [{"id": "wz", "type": "org.homecontrol.room-controller", "version": "1.0", "label": {"en": "Living", "de": "Wohnen"},
	            "inputs": {"temperature": "temp", "window": ["win1", "win2"]}, "outputs": {"valve": "valve"},
	            "parameters": {"system": "convector", "comfort": 21.5}}],
	"rules": [{"id": "follow", "name": "Follow",
		"reactions": [{"triggers": [{"kind": "datapoint", "datapoint": "block/wz/demand"}],
			"actions": [{"kind": "write", "datapoint": "heating", "value": {"kind": "datapoint", "name": "block/wz/demand"}}]}]}]
})";

std::string with_block(const std::string& block) {
	return R"({"image": {"schemaVersion": "1.3", "projectId": "refuse", "revision": 1},
		"datapoints": [{"id": "temp", "type": "real", "writable": true, "virtual": {}},
		               {"id": "flag", "type": "boolean", "writable": true, "virtual": {}},
		               {"id": "block/x", "type": "real", "virtual": {}}],
		"blocks": [)" + block + "]}";
}

} // namespace

TEST_CASE("Image.Blocks: schema 1.3; mirrors behind the image's datapoints; refusals with their paths") {
	DashboardFixture f;
	CHECK(std::string(hc::image::schema_version_text()) == "1.4");
	f.load(kBlocksImage);
	f.controller.run_once();
	f.controller.run_once();
	CHECK(f.service.info().blocks == 1);
	// spec/blocks.md 6: enabled, the adjustable setpoints, the outputs, the aggregated window.
	const char* mirrors[] = {"block/wz/enabled", "block/wz/comfort", "block/wz/standby", "block/wz/night", "block/wz/frost",
		"block/wz/valve", "block/wz/switch", "block/wz/demand", "block/wz/activeSetpoint", "block/wz/state", "block/wz/p",
		"block/wz/i", "block/wz/d", "block/wz/error", "block/wz/filtered", "block/wz/window"};
	for (const char* name : mirrors) {
		CAPTURE(name);
		const hc::Datapoint* dp = f.model.find(name);
		REQUIRE(dp != nullptr);
		CHECK(std::string(dp->technology()->name()) == "blocks");
	}
	CHECK(f.model.find("block/wz/temperature") == nullptr); // a single datapoint: no mirror
	CHECK(f.model.find("block/wz/comfort")->value().as_real() == 21.5);
	CHECK(f.model.find("block/wz/valve")->value().as_real() == 30); // no temperature yet: the fallback
	CHECK(f.model.find("heating")->value().as_bool() == true);       // the rule followed the demand

	// Refusals, each with its path (spec/blocks.md 11).
	struct Refusal {
		std::string block;
		const char* path;
		const char* reason;
	} refusals[] = {
		{R"({"id": "a", "type": "org.homecontrol.room-controller", "version": "1.1", "label": "A", "inputs": {"temperature": "temp"}})",
			"blocks[0].type", "unknownType"},
		{R"({"id": "a", "type": "org.example.nothing", "version": "1.0", "label": "A"})", "blocks[0].type", "unknownType"},
		{R"({"id": "a", "type": "org.homecontrol.room-controller", "version": "1.0", "label": "A"})",
			"blocks[0].inputs.temperature", "missingInput"},
		{R"({"id": "a", "type": "org.homecontrol.room-controller", "version": "1.0", "label": "A", "inputs": {"temperature": "flag"}})",
			"blocks[0].inputs.temperature", "typeError"},
		{R"({"id": "a", "type": "org.homecontrol.room-controller", "version": "1.0", "label": "A", "inputs": {"temperature": ["temp", "nope"]}})",
			"blocks[0].inputs.temperature[1]", "unknownReference"},
		{R"({"id": "a", "type": "org.homecontrol.room-controller", "version": "1.0", "label": "A", "inputs": {"temperature": "temp"}, "parameters": {"comfort": 40}})",
			"blocks[0].parameters.comfort", "invalidParameter"},
		{R"({"id": "a", "type": "org.homecontrol.room-controller", "version": "1.0", "label": "A", "inputs": {"temperature": "temp"}, "parameters": {"outputMin": 60, "outputMax": 50}})",
			"blocks[0].parameters.outputMin", "invalidParameter"},
		{R"({"id": "a", "type": "org.homecontrol.room-controller", "version": "1.0", "label": "A", "inputs": {"temperature": "temp"}, "outputs": {"valve": "block/a/p"}})",
			"blocks[0].outputs.valve", "outputConflict"},
		{R"({"id": "a", "type": "org.homecontrol.room-controller", "version": "1.0", "label": "A", "inputs": {"temperature": "block/a/valve"}})",
			"blocks[0].inputs.temperature", "forbidden"},
	};
	for (const Refusal& r : refusals) {
		CAPTURE(r.block);
		std::string text = with_block(r.block);
		hc::image::Report report;
		hc::image::Plan plan;
		hc::image::Loader loader(f.fake.clock, hc::HistoryLimits().cap_bytes);
		std::string without = text;
		without.replace(without.find(R"(,
		               {"id": "block/x", "type": "real", "virtual": {}})"), std::strlen(R"(,
		               {"id": "block/x", "type": "real", "virtual": {}})"), "");
		CHECK_FALSE(loader.parse(without.data(), without.size(), plan, report));
		REQUIRE(report.count > 0);
		CHECK(std::string(report.refusals[0].path) == r.path);
		CHECK(std::string(report.refusals[0].reason).find(r.reason) != std::string::npos);
	}
	// The prefix block/ is reserved for the mirrors.
	{
		std::string text = with_block(R"({"id": "a", "type": "org.homecontrol.room-controller", "version": "1.0", "label": "A", "inputs": {"temperature": "temp"}})");
		hc::image::Report report;
		hc::image::Plan plan;
		hc::image::Loader loader(f.fake.clock, hc::HistoryLimits().cap_bytes);
		CHECK_FALSE(loader.parse(text.data(), text.size(), plan, report));
		CHECK(std::string(report.refusals[0].path) == "datapoints[2].id");
	}
	// Strict inside (B1): an unknown member of a newer minor schema names the firmware's version (I4).
	{
		std::string text = with_block(R"({"id": "a", "type": "org.homecontrol.room-controller", "version": "1.0", "label": "A", "inputs": {"temperature": "temp"}, "wasm": "abc"})");
		text.replace(text.find("\"1.3\""), 5, "\"1.5\"");
		text.replace(text.find(R"(,
		               {"id": "block/x", "type": "real", "virtual": {}})"), std::strlen(R"(,
		               {"id": "block/x", "type": "real", "virtual": {}})"), "");
		hc::image::Report report;
		hc::image::Plan plan;
		hc::image::Loader loader(f.fake.clock, hc::HistoryLimits().cap_bytes);
		CHECK_FALSE(loader.parse(text.data(), text.size(), plan, report));
		CHECK(std::string(report.refusals[0].path) == "blocks[0]");
		CHECK(std::string(report.refusals[0].reason).find("unknown member 'wasm'") != std::string::npos);
		CHECK(std::string(report.refusals[0].reason).find("update the firmware") != std::string::npos);
	}
}

TEST_CASE("WebApi.Blocks: list, types, changes applied whole or refused whole, restart only after a fault") {
	DashboardFixture f;
	f.load(kBlocksImage);
	f.controller.run_once();
	f.controller.run_once();
	auto status = f.get("/api/status", f.cookie);
	REQUIRE(status.status == 200);
	CHECK(status.json["device"]["schema"].text == "1.4");
	CHECK(status.json["image"]["blocks"].number == 1);
	auto r = f.get("/api/blocks", f.cookie);
	REQUIRE(r.status == 200);
	REQUIRE(r.json["blocks"].items.size() == 1);
	// One room controller fits the E-Home connection's 4 KB answers (EhomeLink::kMaxResponseBody).
	MESSAGE("GET /api/blocks with one room controller: " << r.body.size() << " bytes");
	CHECK(r.body.size() < hc::EhomeLink::kMaxResponseBody);
	const auto& b = r.json["blocks"].items[0];
	CHECK(b["id"].text == "wz");
	CHECK(b["version"].text == "1.0");
	CHECK(b["label"]["de"].text == "Wohnen");
	CHECK(b["status"].text == "running");
	CHECK(b["inputs"].items[5]["id"].text == "window");
	CHECK(b["inputs"].items[5]["aggregation"].text == "any");
	CHECK(b["outputs"].items[0]["datapoint"].text == "valve");
	CHECK(b["outputs"].items[0]["value"].number == 30);
	CHECK(b["state"].has("integral"));
	auto types = f.get("/api/blocks/types", f.cookie);
	REQUIRE(types.status == 200);
	MESSAGE("GET /api/blocks/types: " << types.body.size() << " bytes");
	CHECK(b["values"]["system"].number == 2);
	CHECK(b["parameters"].items.size() == 4); // the adjustable setpoints
	CHECK(types.json["types"].items[0]["type"].text == "org.homecontrol.room-controller");
	CHECK(types.json["types"].items[0]["ports"].items[0]["aggregations"].items[0].text == "mean");

	// A change is checked whole: the valid comfort is not applied with the invalid night.
	auto refused = f.put("/api/blocks/wz", R"({"parameters": {"comfort": 22, "night": 99}})");
	CHECK(refused.status == 422);
	CHECK(refused.json["code"].text == "block.invalid_value");
	CHECK(refused.json["member"].text == "night");
	CHECK(f.model.find("block/wz/comfort")->value().as_real() == 21.5);
	CHECK(f.put("/api/blocks/wz", R"({"parameters": {"xp": 4}})").json["code"].text == "block.not_adjustable");
	CHECK(f.put("/api/blocks/nope", R"({"enabled": false})").status == 404);
	CHECK(f.put("/api/blocks/wz", R"({"restart": true})").json["code"].text == "block.not_faulted");
	CHECK(f.put("/api/blocks/wz", R"({"bogus": 1})").json["code"].text == "block.malformed");
	auto ok = f.put("/api/blocks/wz", R"({"parameters": {"comfort": 22.5}, "enabled": false})");
	REQUIRE(ok.status == 200);
	CHECK(ok.json["block"]["status"].text == "disabled");
	CHECK(ok.json["block"]["enabledOverridden"].boolean);
	CHECK(f.model.find("block/wz/comfort")->value().as_real() == 22.5);
	CHECK(f.model.find("valve")->value().as_real() == 30); // the failsafe value
	auto back = f.put("/api/blocks/wz", R"({"enabled": true, "reset": ["comfort"]})");
	REQUIRE(back.status == 200);
	CHECK(back.json["block"]["status"].text == "running");
	CHECK(f.model.find("block/wz/comfort")->value().as_real() == 21.5);

	// The image survives a reload from the store with its blocks' overrides (the boot path).
	CHECK(f.put("/api/blocks/wz", R"({"parameters": {"night": 16}})").status == 200);
	REQUIRE(f.service.load_stored(hc::image::Source::boot));
	f.controller.run_once();
	f.controller.run_once();
	CHECK(f.model.find("block/wz/night")->value().as_real() == 16);
}

TEST_CASE("Blocks probe: a rule sets a block's parameter in its cascade; a block reads another block's output") {
	DashboardFixture f;
	f.load(R"({
		"image": {"schemaVersion": "1.3", "projectId": "chain", "revision": 1},
		"datapoints": [{"id": "temp", "type": "real", "writable": true, "virtual": {}},
		               {"id": "party", "type": "boolean", "writable": true, "virtual": {}}],
		"blocks": [{"id": "wz", "type": "org.homecontrol.room-controller", "version": "1.0", "label": "Living",
		            "inputs": {"temperature": "temp"}, "parameters": {"system": "custom", "filter": 0}},
		           {"id": "echo", "type": "org.homecontrol.room-controller", "version": "1.0", "label": "Echo",
		            "inputs": {"temperature": "block/wz/filtered"}, "parameters": {"system": "custom", "filter": 0}}],
		"rules": [{"id": "party", "name": "Party",
			"reactions": [{"triggers": [{"kind": "datapoint", "datapoint": "party"}],
				"actions": [{"kind": "write", "datapoint": "block/wz/comfort", "value": {"kind": "literal", "type": "real", "value": 23}}]}]}]
	})");
	f.controller.run_once();
	f.controller.run_once();
	REQUIRE(f.model.command(f.model.find("temp")->id(), hc::Value::of_real(20.25)) == hc::DatapointStatus::ok);
	// The temperature input calls no step (trigger none): the next period takes it.
	f.fake.clock.advance(61000);
	for (int i = 0; i < 4; ++i) {
		f.controller.run_once();
	}
	CHECK(f.model.find("block/wz/filtered")->value().as_real() == 20.25);
	CHECK(f.model.find("block/echo/state")->value().as_enum() == hc::blocks::RoomController::heating);
	// Two room controllers do not fit one page of the test's buffer: the list pages (next).
	auto r = f.get("/api/blocks", f.cookie);
	REQUIRE(r.status == 200);
	CHECK(r.json["count"].number == 2);
	uint32_t next = r.json["next"].kind == hc_test::Json::Kind::number ? static_cast<uint32_t>(r.json["next"].number) : 1;
	auto second = f.get("/api/blocks?from=" + std::to_string(next), f.cookie);
	REQUIRE(second.status == 200);
	REQUIRE(!second.json["blocks"].items.empty());
	CHECK(second.json["blocks"].items.back()["id"].text == "echo");
	CHECK(second.json["blocks"].items.back()["inputs"].items[0]["value"].number == 20.25);
	// The rule's write to the parameter's datapoint is a parameter change in its cascade: the
	// controller steps at once with the new setpoint, and the override is persisted.
	REQUIRE(f.model.command(f.model.find("party")->id(), hc::Value::of_bool(true)) == hc::DatapointStatus::ok);
	f.controller.run_once();
	f.controller.run_once();
	CHECK(f.model.find("block/wz/comfort")->value().as_real() == 23);
	CHECK(f.model.find("block/wz/activeSetpoint")->value().as_real() == 23);
	CHECK(f.service.blocks().overridden(0, hc::blocks::RoomController::kComfort));
}

TEST_CASE("Blocks review probe: a schedule's output drives the setpoint; a diagnostic charted through a declared datapoint") {
	DashboardFixture f; // 2026-10-17 12:03:12 local time (the fixture's zone has a fixed offset of +2 h), synchronized
	f.load(R"({
		"image": {"schemaVersion": "1.3", "projectId": "sched", "revision": 1},
		"datapoints": [{"id": "temp", "type": "real", "writable": true, "virtual": {}},
		               {"id": "day", "type": "boolean", "virtual": {}},
		               {"id": "heat", "type": "real", "writable": true, "virtual": {}},
		               {"id": "wz.filtered", "type": "real", "writable": true, "virtual": {},
		                "history": {"retention": 86400, "spacing": 60}},
		               {"id": "seen", "type": "signed", "writable": true, "virtual": {}}],
		"schedules": [{"id": "morning", "label": "Morning", "kind": "weekly", "days": [1, 2, 3, 4, 5, 6, 7],
		               "time": {"hour": 12, "minute": 30, "second": 0}, "output": "day"}],
		"blocks": [{"id": "wz", "type": "org.homecontrol.room-controller", "version": "1.0", "label": "Living",
		            "inputs": {"temperature": "temp", "heatingPeriod": "day"},
		            "outputs": {"valve": "heat", "filtered": "wz.filtered"},
		            "parameters": {"system": "custom", "filter": 0, "td": 0}}],
		"rules": [{"id": "count", "name": "Count",
			"reactions": [{"triggers": [{"kind": "datapoint", "datapoint": "block/wz/activeSetpoint"}],
				"actions": [{"kind": "write", "datapoint": "seen", "value": {"kind": "datapoint", "name": "block/wz/activeSetpoint"}}]}]}]
	})");
	REQUIRE(f.model.command(f.model.find("temp")->id(), hc::Value::of_real(19.0)) == hc::DatapointStatus::ok);
	for (int s = 0; s < 120; s += 10) {
		f.fake.clock.wall_seconds += 10;
		f.fake.clock.advance(10000);
		f.controller.run_once();
	}
	// Not fired yet today: the schedule's output is false, heatingPeriod false is night.
	CHECK(f.model.find("day")->value().as_bool() == false);
	CHECK(f.model.find("block/wz/activeSetpoint")->value().as_real() == 17);
	CHECK(f.model.find("block/wz/state")->value().as_enum() == hc::blocks::RoomController::heating);
	CHECK(f.model.find("wz.filtered")->value().as_real() == 19.0);
	CHECK(f.model.find("heat")->value().as_real() == 0); // 2 K above the setpoint
	// The firing at 12:30 is an event: the controller steps at once to comfort.
	while (f.fake.clock.wall_seconds < 1792231392 + 27 * 60 + 10) {
		f.fake.clock.wall_seconds += 10;
		f.fake.clock.advance(10000);
		f.controller.run_once();
	}
	CHECK(f.model.find("day")->value().as_bool() == true);
	CHECK(f.model.find("block/wz/activeSetpoint")->value().as_real() == 21);
	CHECK(f.model.find("seen")->value().as_signed() == 21);
	double valve = f.model.find("heat")->value().as_real();
	CHECK(valve > 60); // 2 K error, xp 3: the proportional term alone is 66 %
	CHECK(valve == f.model.find("block/wz/valve")->value().as_real());
	// The declared datapoint behind the diagnostic carries its history.
	REQUIRE(f.model.command(f.model.find("temp")->id(), hc::Value::of_real(19.5)) == hc::DatapointStatus::ok);
	for (int s = 0; s < 600; s += 10) {
		f.fake.clock.wall_seconds += 10;
		f.fake.clock.advance(10000);
		f.controller.run_once();
	}
	int64_t now = f.fake.clock.wall_seconds;
	auto id = f.model.find("wz.filtered")->id();
	auto r = f.get("/api/history?ids=" + std::to_string(id) + "&from=" + std::to_string(now - 1800) + "&to=" +
		std::to_string(now) + "&points=6", f.cookie);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	auto& points = r.json["series"].items[0]["points"].items;
	REQUIRE_FALSE(points.empty());
	CHECK(points.back().items[1].number == 19.5);
	bool saw19 = false;
	for (auto& p : points) {
		saw19 = saw19 || p.items[2].number == 19.0;
	}
	CHECK(saw19);
}

TEST_CASE("Blocks review probe: a heating demand over a room controller that is disabled and a plain valve") {
	DashboardFixture f;
	f.load(R"({
		"image": {"schemaVersion": "1.3", "projectId": "demand", "revision": 1},
		"datapoints": [{"id": "temp", "type": "real", "writable": true, "virtual": {}},
		               {"id": "v2", "type": "real", "writable": true, "virtual": {}},
		               {"id": "req", "type": "boolean", "writable": true, "virtual": {}}],
		"blocks": [{"id": "wz", "type": "org.homecontrol.room-controller", "version": "1.0", "label": "Living",
		            "inputs": {"temperature": "temp"}, "parameters": {"system": "custom", "filter": 0, "td": 0}},
		           {"id": "hd", "type": "org.homecontrol.heating-demand", "version": "1.0", "label": "Demand",
		            "inputs": {"valves": ["block/wz/valve", "v2"]}, "outputs": {"request": "req"},
		            "parameters": {"minOn": 0, "minOff": 0}}]
	})");
	f.controller.run_once();
	f.controller.run_once();
	// No temperature yet: the room controller sends its fallback 30 %; v2 has no value (ignored, counted stale).
	CHECK(f.model.find("block/wz/valve")->value().as_real() == 30);
	CHECK(f.model.find("block/hd/level")->value().as_real() == 30);
	CHECK(f.model.find("block/hd/staleInputs")->value().as_signed() == 1);
	CHECK(f.model.find("req")->value().as_bool() == true);
	CHECK(f.model.find("block/hd/valves")->value().as_real() == 30); // the mirror: max over the good ones
	REQUIRE(f.model.command(f.model.find("temp")->id(), hc::Value::of_real(23.0)) == hc::DatapointStatus::ok);
	f.fake.clock.advance(61000);
	for (int i = 0; i < 4; ++i) {
		f.controller.run_once();
	}
	// 2 K above the setpoint: the valve closes, the level falls to 0, the request goes off.
	CHECK(f.model.find("block/wz/valve")->value().as_real() == 0);
	CHECK(f.model.find("block/hd/level")->value().as_real() == 0);
	CHECK(f.model.find("req")->value().as_bool() == false);
	// The room controller disabled: its failsafe 30 % reaches the demand through its mirror.
	auto r = f.put("/api/blocks/wz", R"({"enabled": false})");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	f.controller.run_once();
	CHECK(f.model.find("block/wz/valve")->value().as_real() == 30);
	CHECK(f.model.find("block/hd/level")->value().as_real() == 30);
	CHECK(f.model.find("req")->value().as_bool() == true);
	// The plain valve reports: the higher one wins, nothing stale any more.
	REQUIRE(f.model.command(f.model.find("v2")->id(), hc::Value::of_real(80.0)) == hc::DatapointStatus::ok);
	f.controller.run_once();
	f.controller.run_once();
	CHECK(f.model.find("block/hd/level")->value().as_real() == 80);
	CHECK(f.model.find("block/hd/staleInputs")->value().as_signed() == 0);
	CHECK(f.model.find("block/hd/valves")->value().as_real() == 80);
	// A reload from the store (the boot path): wz stays disabled, the demand starts over the same inputs.
	REQUIRE(f.service.load_stored(hc::image::Source::boot));
	f.controller.run_once();
	f.controller.run_once();
	CHECK(f.model.find("block/wz/enabled")->value().as_bool() == false);
	CHECK(f.model.find("block/wz/valve")->value().as_real() == 30);
	CHECK(f.model.find("req")->value().as_bool() == true);
}

TEST_CASE("Blocks review probe: sun shading without a wall clock; a wind alarm across an operation and an image swap") {
	DashboardFixture f;
	f.fake.clock.synchronized = false;
	auto image = [](int revision) {
		return std::string(R"({"image": {"schemaVersion": "1.3", "projectId": "shade", "revision": )") +
			std::to_string(revision) + R"(},
		"settings": {"location": {"lat": 48.137, "lon": 11.576}},
		"datapoints": [{"id": "wind", "type": "real", "writable": true, "virtual": {}},
		               {"id": "man", "type": "boolean", "writable": true, "virtual": {}},
		               {"id": "pos", "type": "real", "writable": true, "virtual": {}}],
		"blocks": [{"id": "ss", "type": "org.homecontrol.sun-shading", "version": "1.0", "label": "South",
		            "inputs": {"wind": "wind", "manual": "man"}, "outputs": {"position": "pos"},
		            "parameters": {"alarmRelease": 60000, "minMoveInterval": 0}}]})";
	};
	f.load(image(1));
	f.controller.run_once();
	f.controller.run_once();
	// No clock: no sun, idle, nothing written (a start in idle moves nothing).
	CHECK(f.model.find("block/ss/state")->value().as_enum() == 0);
	CHECK(f.model.find("pos")->quality() == hc::Quality::unknown);
	CHECK(f.model.find("block/ss/sunAzimuth")->quality() == hc::Quality::unknown);
	// The wind alarm works without a clock: retracted at once.
	REQUIRE(f.model.command(f.model.find("wind")->id(), hc::Value::of_real(15.0)) == hc::DatapointStatus::ok);
	f.controller.run_once();
	f.controller.run_once();
	CHECK(f.model.find("block/ss/state")->value().as_enum() == 2);
	CHECK(f.model.find("pos")->value().as_real() == 0);
	// An operation during the alarm does not lift it; the retracted position is sent again.
	REQUIRE(f.model.command(f.model.find("pos")->id(), hc::Value::of_real(50.0)) == hc::DatapointStatus::ok);
	REQUIRE(f.model.command(f.model.find("man")->id(), hc::Value::of_bool(true)) == hc::DatapointStatus::ok);
	f.controller.run_once();
	f.controller.run_once();
	CHECK(f.model.find("block/ss/state")->value().as_enum() == 2);
	CHECK(f.model.find("pos")->value().as_real() == 0);
	// An image swap during the alarm keeps it (the state record of the same major version).
	f.load(image(2));
	f.controller.run_once();
	f.controller.run_once();
	CHECK(f.model.find("block/ss/state")->value().as_enum() == 2);
	CHECK(f.model.find("pos")->value().as_real() == 0);
	// Calm: the alarm ends after alarmRelease, not before.
	REQUIRE(f.model.command(f.model.find("wind")->id(), hc::Value::of_real(3.0)) == hc::DatapointStatus::ok);
	f.controller.run_once();
	f.fake.clock.advance(30000);
	f.controller.run_once();
	f.controller.run_once();
	CHECK(f.model.find("block/ss/state")->value().as_enum() == 2);
	f.fake.clock.advance(61000);
	for (int i = 0; i < 4; ++i) {
		f.controller.run_once();
	}
	CHECK(f.model.find("block/ss/state")->value().as_enum() != 2);
}

TEST_CASE("Blocks review probe: house mode drives a room controller; vacation ends when a new image unbinds it") {
	DashboardFixture f;
	auto image = [](int revision, bool vacation) {
		return std::string(R"({"image": {"schemaVersion": "1.3", "projectId": "house", "revision": )") +
			std::to_string(revision) + R"(},
		"datapoints": [{"id": "temp", "type": "real", "writable": true, "virtual": {}},
		               {"id": "pres", "type": "boolean", "writable": true, "virtual": {}},
		               {"id": "vac", "type": "boolean", "writable": true, "virtual": {}}],
		"blocks": [{"id": "hm", "type": "org.homecontrol.house-mode", "version": "1.0", "label": "House",
		            "inputs": {"presence": "pres")" + (vacation ? R"(, "vacation": "vac")" : "") + R"(}},
		           {"id": "wz", "type": "org.homecontrol.room-controller", "version": "1.0", "label": "Living",
		            "inputs": {"temperature": "temp", "mode": "block/hm/hvacMode"},
		            "parameters": {"system": "custom", "filter": 0}}]})";
	};
	f.load(image(1, true));
	REQUIRE(f.model.command(f.model.find("temp")->id(), hc::Value::of_real(20.0)) == hc::DatapointStatus::ok);
	REQUIRE(f.model.command(f.model.find("pres")->id(), hc::Value::of_bool(true)) == hc::DatapointStatus::ok);
	f.fake.clock.advance(61000);
	for (int i = 0; i < 4; ++i) {
		f.controller.run_once();
	}
	CHECK(f.model.find("block/hm/mode")->value().as_enum() == 0);
	CHECK(f.model.find("block/wz/activeSetpoint")->value().as_real() == 21);
	// Vacation: frost protection in the room, nobody occupied.
	REQUIRE(f.model.command(f.model.find("vac")->id(), hc::Value::of_bool(true)) == hc::DatapointStatus::ok);
	f.controller.run_once();
	f.controller.run_once();
	CHECK(f.model.find("block/hm/mode")->value().as_enum() == 3);
	CHECK(f.model.find("block/hm/occupied")->value().as_bool() == false);
	CHECK(f.model.find("block/wz/activeSetpoint")->value().as_real() == 7);
	// The persisted mode survives a reload from the store (the boot path).
	REQUIRE(f.service.load_stored(hc::image::Source::boot));
	f.controller.run_once();
	f.controller.run_once();
	CHECK(f.model.find("block/hm/mode")->value().as_enum() == 3);
	// A new image no longer binds vacation: it ends; presence makes it home again.
	f.load(image(2, false));
	f.fake.clock.advance(1000);
	for (int i = 0; i < 4; ++i) {
		f.controller.run_once();
	}
	CHECK(f.model.find("block/hm/mode")->value().as_enum() == 0);
	CHECK(f.model.find("block/wz/activeSetpoint")->value().as_real() == 21);
}

TEST_CASE("Blocks review probe: a PV surplus chain through the controller; the second instance takes only what the "
		  "first leaves; a lost meter releases both") {
	DashboardFixture f;
	f.load(R"({"image": {"schemaVersion": "1.3", "projectId": "pv", "revision": 1},
		"datapoints": [{"id": "grid", "type": "real", "writable": true, "virtual": {}},
		               {"id": "hp", "type": "boolean", "writable": true, "virtual": {}},
		               {"id": "rod", "type": "real", "writable": true, "virtual": {}}],
		"blocks": [{"id": "pv1", "type": "org.homecontrol.pv-surplus", "version": "1.0", "label": "PV 1",
		            "inputs": {"gridPower": "grid", "lowerPower": "block/pv2/loadPower"},
		            "outputs": {"switch1": "hp"},
		            "parameters": {"surplusFilter": 0, "meterTimeout": 60000, "power1": 1000, "minOn1": 0}},
		           {"id": "pv2", "type": "org.homecontrol.pv-surplus", "version": "1.0", "label": "PV 2",
		            "inputs": {"gridPower": "block/pv1/surplus"}, "outputs": {"setpoint1": "rod"},
		            "parameters": {"gridSign": "exportPositive", "surplusFilter": 0, "meterTimeout": 60000, "power1": 3000,
		                           "minPower1": 1000, "step1": 1000, "minOn1": 0}}]})");
	auto meter = [&](double grid) {
		REQUIRE(f.model.command(f.model.find("grid")->id(), hc::Value::of_real(grid)) == hc::DatapointStatus::ok);
		f.controller.run_once();
	};
	auto period = [&]() {
		f.fake.clock.advance(30000);
		for (int i = 0; i < 6; ++i) {
			f.controller.run_once();
		}
	};
	meter(-2600); // 2600 W export: the blocks start on it and allocate at once
	CHECK(f.model.find("hp")->value().as_bool());
	CHECK(f.model.find("block/pv1/surplus")->value().as_real() == 1600);
	CHECK(f.model.find("rod")->value().as_real() == 1000); // 1600 left, a step of the rod: not 2000
	meter(-600); // the meter shows both loads
	period();
	CHECK(f.model.find("hp")->value().as_bool());
	CHECK(f.model.find("rod")->value().as_real() == 1000);
	CHECK(f.model.find("block/pv1/loadPower")->value().as_real() == 2000);
	// The meter falls silent: both instances release their loads within meterTimeout and a period.
	for (int i = 0; i < 3; ++i) {
		period();
	}
	CHECK(!f.model.find("hp")->value().as_bool());
	period(); // the second loses its meter, the first one's surplus, a period later
	CHECK(f.model.find("rod")->value().as_real() == 0);
}

TEST_CASE("Blocks review probe: a PV chain whose meter falls silent releases both instances' loads") {
	DashboardFixture f;
	f.load(R"({"image": {"schemaVersion": "1.3", "projectId": "pv", "revision": 1},
		"datapoints": [{"id": "grid", "type": "real", "writable": true, "virtual": {}},
		               {"id": "boiler", "type": "boolean", "writable": true, "virtual": {}},
		               {"id": "car", "type": "boolean", "writable": true, "virtual": {}}],
		"blocks": [{"id": "a", "type": "org.homecontrol.pv-surplus", "version": "1.0", "label": "A",
		            "inputs": {"gridPower": "grid", "lowerPower": "block/b/loadPower"}, "outputs": {"switch1": "boiler"},
		            "parameters": {"gridSign": "exportPositive", "surplusFilter": 0, "meterTimeout": 60000,
		                           "power1": 1000, "minOn1": 0, "minOff1": 0}},
		           {"id": "b", "type": "org.homecontrol.pv-surplus", "version": "1.0", "label": "B",
		            "inputs": {"gridPower": "block/a/surplus"}, "outputs": {"switch1": "car"},
		            "parameters": {"gridSign": "exportPositive", "surplusFilter": 0, "meterTimeout": 60000,
		                           "power1": 500, "minOn1": 0, "minOff1": 0}}]})");
	auto run = [&](int seconds, bool report) {
		for (int s = 0; s < seconds; s += 5) {
			if (report) {
				// A physical meter: the export is the generation's 3000 W minus what the loads draw.
				const hc::Datapoint* boiler = f.model.find("boiler");
				const hc::Datapoint* car = f.model.find("car");
				double drawn = (boiler->quality() == hc::Quality::good && boiler->value().as_bool() ? 1000.0 : 0.0) +
					(car->quality() == hc::Quality::good && car->value().as_bool() ? 500.0 : 0.0);
				REQUIRE(f.model.command(f.model.find("grid")->id(), hc::Value::of_real(3000.0 - drawn)) == hc::DatapointStatus::ok);
			}
			f.fake.clock.advance(5000);
			f.controller.run_once();
			f.controller.run_once();
		}
	};
	run(180, true);
	CHECK(f.model.find("boiler")->value().as_bool() == true);
	CHECK(f.model.find("car")->value().as_bool() == true);
	// The meter falls silent: after meterTimeout A has no measurement and releases its load; B, fed by
	// A's surplus, must not keep running or switch on from what A no longer measures.
	bool car_on_after_loss = false;
	for (int s = 0; s < 240; s += 5) {
		run(5, false);
		if (s > 150 && f.model.find("car")->value().as_bool()) {
			car_on_after_loss = true;
		}
	}
	CHECK(f.model.find("boiler")->value().as_bool() == false);
	CHECK(f.model.find("car")->value().as_bool() == false);
	CHECK_FALSE(car_on_after_loss);
}
