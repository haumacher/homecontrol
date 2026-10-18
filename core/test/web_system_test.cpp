/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The console's commands over the network (issue #38, spec/api.md "System", "Tokens", "Zigbee"):
// restart, factory reset, update, log levels, WiFi, tokens, the Zigbee coordinator, the external
// sources, the language pack sync, the MQTT discovery, the KNX settings and the status members.

#include <algorithm>
#include <memory>
#include <string>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/external_values.h"
#include "hc/image/image_service.h"
#include "hc/knx/adapter.h"
#include "hc/knx/knx_settings.h"
#include "hc/knx/tunnel.h"
#include "hc/language_packs.h"
#include "hc/live_feed.h"
#include "hc/mqtt.h"
#include "hc/network_settings.h"
#include "knx_test_support.h"
#include "mqtt_test_support.h"
#include "web_test_support.h"
#include "zigbee_test_support.h"

using hc::HttpMethod;
using hc_test::WebFixture;

namespace {

struct SystemFixture : WebFixture {
	std::string cookie;

	SystemFixture() {
		controller.start();
		cookie = onboard();
	}

	Result call(HttpMethod method, const std::string& target, const std::string& body = "", bool csrf = true) {
		return request(method, target, body, cookie, "", csrf);
	}

	/// Runs the loop until `ms` later.
	void advance(uint64_t ms) {
		uint64_t end = fake.clock.now_ms + ms;
		while (fake.clock.now_ms < end) {
			fake.clock.advance(50);
			controller.loop().run_once(fake.clock.now_ms);
		}
	}
};

/// Records the fetches; the test completes them.
class FakeFetcher : public hc::HttpFetcher {
public:
	bool fetch(const char* url, size_t, hc::FetchReceiver& receiver, const char** error) override {
		if (receiver_ != nullptr) {
			*error = "busy: a fetch is in flight";
			return false;
		}
		urls.push_back(url);
		receiver_ = &receiver;
		return true;
	}
	bool busy() const override { return receiver_ != nullptr; }

	std::vector<std::string> urls;

private:
	hc::FetchReceiver* receiver_ = nullptr;
};

} // namespace

TEST_CASE("WebSystem: every new route needs the principal and the CSRF header") {
	SystemFixture f;
	struct Route {
		HttpMethod method;
		const char* path;
	};
	const Route routes[] = {
		{HttpMethod::post, "/api/system/restart"},
		{HttpMethod::post, "/api/system/factory-reset"},
		{HttpMethod::post, "/api/system/update/check"},
		{HttpMethod::post, "/api/system/update/now"},
		{HttpMethod::put, "/api/system/log-level"},
		{HttpMethod::put, "/api/system/wifi"},
		{HttpMethod::del, "/api/coredump"},
		{HttpMethod::post, "/api/tokens"},
		{HttpMethod::del, "/api/tokens/ehome"},
		{HttpMethod::post, "/api/zigbee/permit"},
		{HttpMethod::del, "/api/zigbee/devices/00124b0001020304"},
		{HttpMethod::post, "/api/zigbee/devices/00124b0001020304/identify"},
		{HttpMethod::put, "/api/zigbee/devices/00124b0001020304/label"},
		{HttpMethod::post, "/api/zigbee/devices/00124b0001020304/interview"},
		{HttpMethod::post, "/api/external/pv/fetch"},
		{HttpMethod::post, "/api/lang/sync"},
		{HttpMethod::post, "/api/mqtt/discovery"},
	};
	for (const Route& route : routes) {
		CAPTURE(route.path);
		auto anonymous = f.request(route.method, route.path, "{}", "", "", true);
		CHECK(anonymous.status == 401);
		CHECK(anonymous.json["code"].text == "auth.required");
		auto forged = f.call(route.method, route.path, "{}", false);
		CHECK(forged.status == 403);
		CHECK(forged.json["code"].text == "auth.csrf");
	}
	for (const char* path : {"/api/tokens", "/api/zigbee", "/api/system/log-level"}) {
		CAPTURE(path);
		CHECK(f.request(HttpMethod::get, path).status == 401);
	}
	CHECK(f.call(HttpMethod::get, "/api/system/nothing").status == 404);
	CHECK(f.call(HttpMethod::get, "/api/system/restart").status == 405);
}

TEST_CASE("WebSystem: restart and factory reset are deferred, the reset behind the device's name") {
	SystemFixture f;
	hc_test::ScopedCapture capture;
	// A platform without the hook: 501 with the reason, in the log too.
	auto r = f.call(HttpMethod::post, "/api/system/restart");
	CHECK(r.status == 501);
	CHECK(r.json["code"].text == "system.unavailable");
	CHECK(r.json["error"].text == "this platform cannot restart or reset itself");
	CHECK(capture.sink.contains("refused (system.unavailable): this platform cannot restart or reset itself"));

	f.host.system_actions = true;
	r = f.call(HttpMethod::post, "/api/system/restart");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["action"].text == "restart");
	CHECK(r.json["in_ms"].number == hc::WebApi::kActionDelayMs);
	CHECK(f.host.actions.empty()); // the answer goes out first
	CHECK(f.call(HttpMethod::post, "/api/system/restart").json["code"].text == "system.busy");
	f.advance(hc::WebApi::kActionDelayMs + 100);
	REQUIRE(f.host.actions.size() == 1);
	CHECK(f.host.actions[0] == hc::SystemAction::restart);

	// The factory reset: the typed name must match.
	r = f.call(HttpMethod::post, "/api/system/factory-reset", "{}");
	CHECK(r.status == 400);
	CHECK(r.json["code"].text == "system.confirm_missing");
	r = f.call(HttpMethod::post, "/api/system/factory-reset", "{\"confirm\": \"homecontrol\"}");
	CHECK(r.status == 422);
	CHECK(r.json["code"].text == "system.confirm_mismatch");
	f.advance(2000);
	CHECK(f.host.actions.size() == 1);
	r = f.call(HttpMethod::post, "/api/system/factory-reset", "{\"confirm\": \"homecontrol-a1b2c3\"}");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["action"].text == "factory-reset");
	f.advance(hc::WebApi::kActionDelayMs + 100);
	REQUIRE(f.host.actions.size() == 2);
	CHECK(f.host.actions[1] == hc::SystemAction::factory_reset);
	CHECK(capture.sink.contains("factory reset requested over the API from 192.0.2.7"));
}

TEST_CASE("WebSystem: update check and update now through the platform") {
	SystemFixture f;
	auto r = f.call(HttpMethod::post, "/api/system/update/check");
	CHECK(r.status == 501);
	CHECK(r.json["code"].text == "system.unavailable");
	CHECK(r.json["error"].text.find("does not update itself") != std::string::npos);

	f.host.updates = true;
	CHECK(f.call(HttpMethod::post, "/api/system/update/check").status == 200);
	r = f.call(HttpMethod::post, "/api/system/update/now", "{\"url\": \"http://updates.example/hc\"}");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["force"].boolean);
	CHECK(f.call(HttpMethod::post, "/api/system/update/now").status == 200);
	REQUIRE(f.host.update_calls.size() == 3);
	CHECK(f.host.update_calls[0] == "check -");
	CHECK(f.host.update_calls[1] == "now http://updates.example/hc");
	CHECK(f.host.update_calls[2] == "now -");
	r = f.call(HttpMethod::post, "/api/system/update/check", "{\"url\": \"http://updates.example/hc\"}");
	CHECK(r.status == 400);
	CHECK(r.json["code"].text == "update.bad_request");
	r = f.call(HttpMethod::post, "/api/system/update/now", "{\"url\": 7}");
	CHECK(r.status == 422);
	f.host.update_refusal = "a check is running";
	r = f.call(HttpMethod::post, "/api/system/update/check");
	CHECK(r.status == 409);
	CHECK(r.json["code"].text == "update.refused");
	CHECK(r.json["error"].text == "a check is running");
}

TEST_CASE("WebSystem: log levels per tag, the platform's tags only where it has a log") {
	SystemFixture f;
	auto r = f.call(HttpMethod::get, "/api/system/log-level");
	REQUIRE(r.status == 200);
	CHECK(r.json["default"].text == "info");
	CHECK(r.json["tags"].items.empty());
	CHECK_FALSE(r.json["platform_tags"].boolean);

	r = f.call(HttpMethod::put, "/api/system/log-level", "{\"tag\": \"hc.zigbee\", \"level\": \"debug\"}");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	REQUIRE(r.json["tags"].items.size() == 1);
	CHECK(r.json["tags"].items[0]["tag"].text == "hc.zigbee");
	CHECK(r.json["tags"].items[0]["level"].text == "debug");
	CHECK(hc::Log::level("hc.zigbee") == hc::LogLevel::debug);

	r = f.call(HttpMethod::put, "/api/system/log-level", "{\"tag\": \"wifi\", \"level\": \"debug\"}");
	CHECK(r.status == 422);
	CHECK(r.json["code"].text == "log.platform_tag");
	r = f.call(HttpMethod::put, "/api/system/log-level", "{\"tag\": \"hc.web\", \"level\": \"loud\"}");
	CHECK(r.status == 422);
	CHECK(r.json["code"].text == "log.invalid");
	r = f.call(HttpMethod::put, "/api/system/log-level", "{\"tag\": \"two words\", \"level\": \"warn\"}");
	CHECK(r.json["code"].text == "log.invalid");

	f.host.platform_log = true;
	r = f.call(HttpMethod::put, "/api/system/log-level", "{\"tag\": \"wifi\", \"level\": \"warn\"}");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["platform_tags"].boolean);
	REQUIRE(f.host.platform_levels.size() == 1);
	CHECK(f.host.platform_levels[0] == "wifi=warn");

	// "*" sets the default and drops the per-tag levels (as on the console); back to info for the
	// other tests.
	r = f.call(HttpMethod::put, "/api/system/log-level", "{\"tag\": \"*\", \"level\": \"info\"}");
	REQUIRE(r.status == 200);
	CHECK(r.json["tags"].items.empty());
	CHECK(hc::Log::level("hc.zigbee") == hc::LogLevel::info);
}

TEST_CASE("WebSystem: WiFi credentials are stored, the reconnect follows the answer") {
	SystemFixture f;
	hc_test::ScopedCapture capture;
	auto r = f.call(HttpMethod::put, "/api/system/wifi", "{\"psk\": \"secret key 1\"}");
	CHECK(r.status == 422);
	CHECK(r.json["code"].text == "wifi.invalid");
	r = f.call(HttpMethod::put, "/api/system/wifi", "{\"ssid\": \"example\", \"psk\": \"short\"}");
	CHECK(r.status == 422);
	CHECK(r.json["code"].text == "wifi.invalid");
	char stored[64] = {};
	CHECK(f.fake.settings.get_string(hc::setting::kWifiSsid, stored, sizeof(stored)) != hc::SettingsStatus::ok);

	r = f.call(HttpMethod::put, "/api/system/wifi", "{\"ssid\": \"example\", \"psk\": \"secret key 1\"}");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["ssid"].text == "example");
	CHECK(r.json["mdns"].text == "homecontrol-a1b2c3.local");
	CHECK(r.json["reconnect_in_ms"].number == hc::WebApi::kActionDelayMs);
	CHECK(f.fake.settings.get_string(hc::setting::kWifiSsid, stored, sizeof(stored)) == hc::SettingsStatus::ok);
	CHECK(std::string(stored) == "example");
	CHECK(f.fake.settings.get_string(hc::setting::kWifiPsk, stored, sizeof(stored)) == hc::SettingsStatus::ok);
	CHECK(std::string(stored) == "secret key 1");
	CHECK(r.body.find("secret") == std::string::npos);
	CHECK_FALSE(capture.sink.contains("secret key 1"));
	f.advance(hc::WebApi::kActionDelayMs + 100);
	CHECK(capture.sink.contains("new WiFi credentials from the API"));
}

TEST_CASE("WebSystem: a token created through the API deploys an image, a revoked one is refused") {
	SystemFixture f;
	hc_test::FakeUdpSocket socket{f.fake.clock};
	hc::knx::KnxTunnel tunnel{f.controller.loop(), f.fake.clock, f.controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{f.controller.loop(), f.fake.clock, f.model, tunnel};
	hc_test::FakeBlobStore blobs;
	hc::image::ImageService service{f.controller, f.model, blobs, adapter, &tunnel};
	service.start();
	f.api.set_image(&service);

	auto r = f.call(HttpMethod::get, "/api/tokens");
	REQUIRE(r.status == 200);
	CHECK(r.json["tokens"].items.empty());
	CHECK(r.json["max"].number == 8);
	r = f.call(HttpMethod::post, "/api/tokens", "{\"name\": \"bad name\"}");
	CHECK(r.status == 422);
	CHECK(r.json["code"].text == "auth.token_name_invalid");
	r = f.call(HttpMethod::post, "/api/tokens", "{\"name\": \"ehome\"}");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	std::string token = r.json["token"].text;
	CHECK(token.rfind("hct_", 0) == 0);
	CHECK(f.call(HttpMethod::post, "/api/tokens", "{\"name\": \"ehome\"}").json["code"].text == "auth.token_name_taken");
	r = f.call(HttpMethod::get, "/api/tokens");
	REQUIRE(r.json["tokens"].items.size() == 1);
	CHECK(r.json["tokens"].items[0]["name"].text == "ehome");
	CHECK(r.body.find(token) == std::string::npos); // shown once

	const std::string image = R"({"image": {"schemaVersion": "1.0", "projectId": "tok", "revision": 1},
		"datapoints": [{"id": "lamp", "type": "boolean", "writable": true, "virtual": {}}]})";
	std::string bearer = "Bearer " + token;
	r = f.request(HttpMethod::put, "/api/image", image, "", bearer, false);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["image"]["projectId"].text == "tok");

	CHECK(f.call(HttpMethod::del, "/api/tokens/nothing").status == 404);
	REQUIRE(f.call(HttpMethod::del, "/api/tokens/ehome").status == 200);
	r = f.request(HttpMethod::put, "/api/image", image, "", bearer, false);
	CHECK(r.status == 401);
	CHECK(r.json["code"].text == "auth.token_invalid");
}

TEST_CASE("WebSystem: clearing the core dump through the platform") {
	SystemFixture f;
	CHECK(f.call(HttpMethod::del, "/api/coredump").json["code"].text == "coredump.none");
}

TEST_CASE("WebSystem: the KNX gateway and NAT mode are settings the UI edits, applied to the tunnel") {
	SystemFixture f;
	hc_test::FakeUdpSocket socket{f.fake.clock};
	hc::knx::KnxTunnel tunnel{f.controller.loop(), f.fake.clock, f.controller.connectivity(), socket};
	auto listing = f.call(HttpMethod::get, "/api/settings");
	REQUIRE(listing.status == 200);
	bool gateway = false;
	bool nat = false;
	for (const auto& s : listing.json["settings"].items) {
		if (s["key"].text == "knx.gateway") {
			gateway = true;
			CHECK(s["editable"].boolean);
			CHECK(s["value"].kind == hc_test::Json::Kind::null);
		}
		if (s["key"].text == "knx.nat") {
			nat = true;
			CHECK(s["editable"].boolean);
			CHECK(s["value"].text == "off");
			CHECK(s["default"].text == "off");
		}
	}
	CHECK(gateway);
	CHECK(nat);

	auto r = f.call(HttpMethod::put, "/api/settings/knx.gateway", "{\"value\": \"not a host\"}");
	CHECK(r.status == 422);
	CHECK(r.json["code"].text == "settings.invalid");
	r = f.call(HttpMethod::put, "/api/settings/knx.gateway", "{\"value\": \"192.0.2.10:3672\"}");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	char stored[64] = {};
	REQUIRE(f.fake.settings.get_string(hc::knx::setting::kGateway, stored, sizeof(stored)) == hc::SettingsStatus::ok);
	CHECK(std::string(stored) == "192.0.2.10:3672");
	CHECK(tunnel.gateway().empty()); // no tunnel handed to the API yet: only stored

	f.api.set_knx(&tunnel, nullptr);
	r = f.call(HttpMethod::put, "/api/settings/knx.gateway", "{\"value\": \"192.0.2.11\"}");
	REQUIRE(r.status == 200);
	char text[hc::knx::GatewayAddress::kMaxText];
	CHECK(std::string(tunnel.gateway().format(text, sizeof(text))) == "192.0.2.11:3671");
	r = f.call(HttpMethod::put, "/api/settings/knx.nat", "{\"value\": \"maybe\"}");
	CHECK(r.status == 422);
	CHECK(r.json["code"].text == "settings.invalid");
	r = f.call(HttpMethod::put, "/api/settings/knx.nat", "{\"value\": \"on\"}");
	REQUIRE(r.status == 200);
	CHECK(tunnel.nat());
	bool flag = false;
	CHECK(f.fake.settings.get_bool(hc::knx::setting::kNat, flag) == hc::SettingsStatus::ok);
	CHECK(flag);
	auto status = f.call(HttpMethod::get, "/api/status");
	CHECK(status.json["knx"]["gateway"].text == "192.0.2.11:3671");
	CHECK(status.json["knx"]["nat"].boolean);
	CHECK(status.json["knx"]["state"].text == "down");
	r = f.call(HttpMethod::put, "/api/settings/knx.gateway", "{\"value\": \"\"}");
	REQUIRE(r.status == 200);
	CHECK(tunnel.gateway().empty());
	CHECK(f.fake.settings.get_string(hc::knx::setting::kGateway, stored, sizeof(stored)) ==
		hc::SettingsStatus::not_found);
}

TEST_CASE("WebSystem: the status carries storage, external, lang and mqtt as the console prints them") {
	SystemFixture f;
	auto r = f.call(HttpMethod::get, "/api/status");
	CHECK_FALSE(r.json.has("storage")); // nothing known: left out
	hc_test::FakeBlobStore blobs;
	const char data[] = "0123456789";
	hc::BlobChunk chunk{data, 10};
	REQUIRE(blobs.write("values", &chunk, 1) == hc::BlobStatus::ok);
	f.api.set_blob_store(&blobs);
	f.host.storage = true;
	r = f.call(HttpMethod::get, "/api/status");
	CHECK(r.json["storage"]["total"].number == 1048576);
	CHECK(r.json["storage"]["used"].number == 65536);
	CHECK(r.json["storage"]["files"].number == 1);
	CHECK(r.json["storage"]["bytes"].number == 10);
}

TEST_CASE("WebSystem: a manual fetch of an external source") {
	SystemFixture f;
	auto r = f.call(HttpMethod::post, "/api/external/pv/fetch");
	CHECK(r.status == 501);
	CHECK(r.json["code"].text == "system.unavailable");

	f.fake.clock.wall_seconds = 1792270800;
	f.fake.clock.synchronized = true;
	f.controller.connectivity().link_up("192.0.2.100");
	hc::DatapointSpec specs[] = {{"pv.power", hc::ValueType::real, hc::DatapointFlags::kReadable}};
	REQUIRE(f.model.load(specs, 1) == hc::DatapointStatus::ok);
	FakeFetcher fetcher;
	hc::ExternalValues external(f.controller.loop(), f.controller.time(), f.controller.settings(),
		f.controller.connectivity(), f.model, fetcher);
	const char* config =
		R"({"sources": [{"id": "pv", "url": "http://pv.example/", "interval": 300, "bindings": [{"datapoint": "pv.power", "path": "a"}]}]})";
	const char* error = nullptr;
	REQUIRE_MESSAGE(external.configure_json(config, std::strlen(config), &error), (error ? error : ""));
	f.api.set_external(&external);

	r = f.call(HttpMethod::post, "/api/external/nope/fetch");
	CHECK(r.status == 404);
	CHECK(r.json["code"].text == "external.unknown");
	r = f.call(HttpMethod::post, "/api/external/pv/fetch");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	REQUIRE(fetcher.urls.size() == 1);
	CHECK(fetcher.urls[0] == "http://pv.example/");
	r = f.call(HttpMethod::post, "/api/external/pv/fetch");
	CHECK(r.status == 409);
	CHECK(r.json["code"].text == "external.refused");
	CHECK(r.json["error"].text == "its fetch is in flight");
	auto status = f.call(HttpMethod::get, "/api/status");
	REQUIRE(status.json["external"]["sources"].items.size() == 1);
	CHECK(status.json["external"]["sources"].items[0]["id"].text == "pv");
	CHECK(status.json["external"]["sources"].items[0]["fetching"].boolean);
	CHECK(f.call(HttpMethod::get, "/api/external/pv/fetch").status == 405);
	CHECK(f.call(HttpMethod::post, "/api/external/pv").status == 404);
}

TEST_CASE("WebSystem: the language pack sync") {
	SystemFixture f;
	auto r = f.call(HttpMethod::post, "/api/lang/sync");
	CHECK(r.status == 501);
	CHECK(r.json["code"].text == "system.unavailable");
	FakeFetcher fetcher;
	hc_test::FakeBlobStore blobs;
	hc::LanguagePacks packs(f.controller.loop(), f.controller.time(), f.controller.settings(),
		f.controller.connectivity(), fetcher, blobs, "0.2.0");
	f.api.set_language_packs(&packs);
	r = f.call(HttpMethod::post, "/api/lang/sync");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	auto status = f.call(HttpMethod::get, "/api/status");
	CHECK(status.json["lang"]["lang"].kind == hc_test::Json::Kind::string);
	CHECK(status.json["lang"]["state"].kind == hc_test::Json::Kind::string);
}

TEST_CASE("WebSystem: the MQTT discovery publish") {
	SystemFixture f;
	auto r = f.call(HttpMethod::post, "/api/mqtt/discovery");
	CHECK(r.status == 501);
	f.controller.connectivity().link_up("192.0.2.100");
	hc_test::FakeMqttClient client;
	hc::MqttIdentity identity;
	identity.node_id = "homecontrol-a1b2c3";
	identity.sw_version = "test";
	hc::Mqtt mqtt(f.controller.loop(), f.fake.clock, f.controller.settings(), f.controller.connectivity(), f.model,
		client, identity, nullptr);
	f.api.set_mqtt(&mqtt);
	r = f.call(HttpMethod::post, "/api/mqtt/discovery");
	CHECK(r.status == 409);
	CHECK(r.json["code"].text == "mqtt.refused");
	auto status = f.call(HttpMethod::get, "/api/status");
	CHECK(status.json["mqtt"]["configured"].boolean == false);

	const char* member = R"({"broker": "mqtt://broker.example"})";
	const char* error = nullptr;
	REQUIRE_MESSAGE(mqtt.configure_json(member, std::strlen(member), &error), (error ? error : ""));
	for (int i = 0; i < 50 && !client.session; ++i) {
		f.advance(100);
	}
	REQUIRE(client.session);
	client.accept();
	f.advance(200);
	r = f.call(HttpMethod::post, "/api/mqtt/discovery");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	status = f.call(HttpMethod::get, "/api/status");
	CHECK(status.json["mqtt"]["configured"].boolean);
	CHECK(status.json["mqtt"]["broker"].text == "mqtt://broker.example");
	CHECK(status.json["mqtt"]["state"].text == hc::mqtt_state_name(mqtt.state()));
}

// ---- Zigbee ----------------------------------------------------------------------------------------

namespace {

struct ZigbeeWebFixture : SystemFixture {
	hc_test::FakeBlobStore store;
	hc_test::FakeZigbeeDriver driver;
	hc::zigbee::ZigbeeAdapter adapter{controller.loop(), fake.clock, model, driver, &store};
	hc_test::ZigbeeActions device{adapter};

	ZigbeeWebFixture() {
		REQUIRE(adapter.start());
		adapter.network_state(true, 0x1a62, 15, 0xddddddddddddddddull);
		api.set_zigbee(&adapter);
	}
};

const char* kBulbPath = "/api/zigbee/devices/00124b0001020304";

} // namespace

TEST_CASE("WebSystem.Zigbee: without a coordinator every route says why") {
	SystemFixture f;
	auto r = f.call(HttpMethod::get, "/api/zigbee");
	CHECK(r.status == 501);
	CHECK(r.json["code"].text == "system.unavailable");
	CHECK(r.json["error"].text == "this controller has no Zigbee coordinator");
	f.api.set_zigbee(nullptr, "this chip has no 802.15.4 radio");
	r = f.call(HttpMethod::post, "/api/zigbee/permit", "{\"seconds\": 60}");
	CHECK(r.status == 501);
	CHECK(r.json["error"].text == "this chip has no 802.15.4 radio");
	CHECK_FALSE(f.call(HttpMethod::get, "/api/status").json.has("zigbee"));
}

TEST_CASE("WebSystem.Zigbee: pairing, the device list, label, identify, interview, remove") {
	ZigbeeWebFixture f;
	auto r = f.call(HttpMethod::get, "/api/zigbee");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["count"].number == 0);
	CHECK(r.json["network"]["up"].boolean);
	CHECK(r.json["network"]["permit_join_s"].number == 0);

	// Open the network: the request reaches the driver, the stack's answer the state.
	r = f.call(HttpMethod::post, "/api/zigbee/permit", "{\"seconds\": 255}");
	CHECK(r.status == 422);
	CHECK(r.json["code"].text == "zigbee.invalid");
	CHECK(f.call(HttpMethod::post, "/api/zigbee/permit", "{\"seconds\": 1.5}").status == 422);
	CHECK(f.call(HttpMethod::post, "/api/zigbee/permit", "{}").status == 422);
	r = f.call(HttpMethod::post, "/api/zigbee/permit", "{\"seconds\": 120}");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(f.driver.calls.back() == "permit 120");
	f.adapter.permit_join_status(120);
	r = f.call(HttpMethod::get, "/api/zigbee");
	CHECK(r.json["network"]["permit_join_s"].number == 120);
	CHECK(f.call(HttpMethod::get, "/api/status").json["zigbee"]["permit_join_s"].number == 120);

	// The bulb joins and is interviewed: listed with its three datapoints.
	f.device.join_bulb();
	r = f.call(HttpMethod::get, "/api/zigbee");
	REQUIRE(r.json["count"].number == 1);
	const auto& bulb = r.json["devices"].items[0];
	CHECK(bulb["ieee"].text == "00124b0001020304");
	CHECK(bulb["manufacturer"].text == "IKEA of Sweden");
	CHECK(bulb["interviewed"].boolean);
	CHECK_FALSE(bulb["left"].boolean);
	REQUIRE(bulb["datapoints"].items.size() == 3);
	CHECK(bulb["datapoints"].items[0].text == "zigbee.00124b0001020304.11.on");
	CHECK(bulb["datapoints"].items[2].text == "zigbee.00124b0001020304.11.color_temperature");
	CHECK(r.json["next"].kind == hc_test::Json::Kind::null);

	// Label: shown by the API and the console's JSON view.
	r = f.call(HttpMethod::put, std::string(kBulbPath) + "/label", "{\"label\": \"Hall ceiling\"}");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["device"]["label"].text == "Hall ceiling");
	char buffer[2048];
	hc::JsonWriter json(buffer, sizeof(buffer));
	f.adapter.write_json(json);
	CHECK(std::string(json.data(), json.length()).find("\"label\":\"Hall ceiling\"") != std::string::npos);
	r = f.call(HttpMethod::put, std::string(kBulbPath) + "/label", "{\"label\": \"" + std::string(33, 'x') + "\"}");
	CHECK(r.status == 422);
	CHECK(r.json["code"].text == "zigbee.invalid");

	// Identify and interview again.
	REQUIRE(f.call(HttpMethod::post, std::string(kBulbPath) + "/identify").status == 200);
	CHECK(f.driver.calls.back() == "identify 0x1a2b 11 10");
	REQUIRE(f.call(HttpMethod::post, std::string(kBulbPath) + "/interview").status == 200);
	CHECK(f.adapter.interviewing() == hc_test::kBulb);
	CHECK(f.call(HttpMethod::get, "/api/zigbee").json["interviewing"].text == "00124b0001020304");
	f.driver.accept = false;
	r = f.call(HttpMethod::post, std::string(kBulbPath) + "/identify");
	CHECK(r.status == 409);
	CHECK(r.json["code"].text == "zigbee.refused");
	f.driver.accept = true;

	// Unknown and malformed addresses.
	r = f.call(HttpMethod::post, "/api/zigbee/devices/00124b00ffffffff/identify");
	CHECK(r.status == 404);
	CHECK(r.json["code"].text == "zigbee.unknown_device");
	CHECK(f.call(HttpMethod::del, "/api/zigbee/devices/xyz").status == 404);
	CHECK(f.call(HttpMethod::post, std::string(kBulbPath) + "/blink").status == 404);
	CHECK(f.call(HttpMethod::get, std::string(kBulbPath) + "/identify").status == 405);

	// Remove: the device is asked to leave and is gone.
	REQUIRE(f.call(HttpMethod::del, kBulbPath).status == 200);
	CHECK(f.driver.calls.back() == "leave 0x1a2b");
	CHECK(f.call(HttpMethod::get, "/api/zigbee").json["count"].number == 0);
	CHECK(f.call(HttpMethod::del, kBulbPath).status == 404);
}

TEST_CASE("WebSystem.Zigbee: the live feed carries joins, interviews and the permit state") {
	ZigbeeWebFixture f;
	hc_test::FakeTransport transport;
	hc::LiveFeed feed(f.api, f.controller.loop(), transport);
	REQUIRE(feed.start());
	REQUIRE(feed.open(1, f.cookie.c_str(), nullptr, nullptr));
	feed.message(1, "{\"type\":\"subscribe\"}", 20);
	f.adapter.permit_join_status(254);
	f.device.join_bulb();
	auto events = transport.messages(1, "zigbee");
	REQUIRE(events.size() >= 4);
	CHECK(events[0]["event"].text == "permit");
	CHECK(events[0]["permit_join_s"].number == 254);
	std::vector<std::string> kinds;
	for (const auto& e : events) {
		kinds.push_back(e["event"].text);
	}
	CHECK(std::find(kinds.begin(), kinds.end(), "joined") != kinds.end());
	CHECK(std::find(kinds.begin(), kinds.end(), "interviewing") != kinds.end());
	CHECK(kinds.back() == "interviewed");
	CHECK(events.back()["ieee"].text == "00124b0001020304");
	CHECK(events.back()["generation"].number == f.adapter.generation());
	// GET /api/status carries the summary with the generation.
	auto status = f.call(HttpMethod::get, "/api/status");
	CHECK(status.json["zigbee"]["devices"].number == 1);
	CHECK(status.json["zigbee"]["generation"].number == f.adapter.generation());
	CHECK(status.json["zigbee"]["permit_join_s"].number == 254);
	feed.closed(1);
}

