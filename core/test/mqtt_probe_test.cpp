/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe of the MQTT client's place in the controller (issue #18): the image service configures it
// from the image's `mqtt` member, refuses an image that carries the password, clears it on unload;
// the model-reload hook follows datapoints that join after the configuration.

#include <string>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/image/image_service.h"
#include "hc/knx/adapter.h"
#include "hc/knx/telegram_log.h"
#include "hc/knx/tunnel.h"
#include "hc/mqtt.h"
#include "knx_test_support.h"
#include "mqtt_test_support.h"
#include "web_test_support.h"

using hc::Value;

namespace {

const char* const kImage = R"({"image": {"schemaVersion": "1.0", "projectId": "mqtt-probe", "revision": 1, "name": "Probe house"},
 "datapoints": [
  {"id": "hall.light", "type": "boolean", "writable": true},
  {"id": "hall.light.state", "type": "boolean"}
 ],
 "functions": [{"id": "hall-light", "kind": "light", "label": "Hall light",
   "datapoints": {"command": "hall.light", "state": "hall.light.state"}}],
 "mqtt": {"broker": "mqtt://broker.example:1883", "baseTopic": "probe/house"}})";

struct ServiceFixture : hc_test::WebFixture {
	hc_test::FakeUdpSocket socket{fake.clock};
	hc::knx::TelegramLog telegrams{fake.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), fake.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{controller.loop(), fake.clock, model, tunnel, &telegrams};
	hc_test::FakeBlobStore blobs;
	hc::image::ImageService image{controller, model, blobs, adapter, &tunnel};
	hc_test::FakeMqttClient client;
	hc::MqttIdentity identity;
	hc::Mqtt mqtt;

	ServiceFixture()
		: identity{make_identity()},
		  mqtt(controller.loop(), fake.clock, fake.settings, controller.connectivity(), model, client, identity,
			  &image.history()) {
		controller.start();
		controller.connectivity().link_up("192.0.2.10");
		adapter.start();
		image.set_mqtt(&mqtt);
		image.start();
		run();
	}

	static hc::MqttIdentity make_identity() {
		hc::MqttIdentity id;
		id.node_id = "homecontrol-probe1";
		id.sw_version = "test";
		return id;
	}

	void run() {
		for (int i = 0; i < 6; ++i) {
			controller.run_once();
		}
	}

	std::string refusals() const {
		std::string why;
		for (size_t i = 0; i < image.report().count; ++i) {
			why += std::string(image.report().refusals[i].path) + ": " + image.report().refusals[i].reason + "\n";
		}
		return why;
	}
};

} // namespace

TEST_CASE("MQTT probe: the image's mqtt member configures the client, unload takes it offline") {
	ServiceFixture f;
	std::string text = kImage;
	REQUIRE_MESSAGE(f.image.load(text.data(), text.size(), hc::image::Source::api, true), f.refusals());
	f.run();
	REQUIRE(f.client.connects == 1);
	CHECK(f.client.client_id == "homecontrol-probe1");
	CHECK(f.client.will_topic == "probe/house/status");
	f.client.accept();
	f.run();
	CHECK(f.mqtt.state() == hc::MqttState::connected);
	REQUIRE_FALSE(f.client.to("probe/house/status").empty());
	CHECK(f.client.to("probe/house/status").back().payload == "online");
	auto configs = f.client.under("homeassistant/light/homecontrol-probe1/hall-light/config");
	REQUIRE(configs.size() == 1);
	CHECK(configs[0].retain);
	CHECK(configs[0].payload.find("\"name\":\"Hall light\"") != std::string::npos);
	CHECK(configs[0].payload.find("\"~\":\"probe/house\"") != std::string::npos);

	// A command from the model shows up as a state.
	REQUIRE(f.model.command(f.model.find("hall.light")->id(), Value::of_bool(true)) == hc::DatapointStatus::ok);
	f.run();
	auto states = f.client.to("probe/house/dp/hall.light/state");
	REQUIRE_FALSE(states.empty());
	CHECK(states.back().payload.find("\"value\":true") != std::string::npos);

	// Unload: offline, then disconnected and unconfigured.
	const char* error = nullptr;
	REQUIRE(f.image.unload(&error));
	f.run();
	CHECK(f.client.to("probe/house/status").back().payload == "offline");
	f.fake.clock.now_ms += hc::Mqtt::kRetireMs + 1000;
	f.run();
	CHECK(f.mqtt.state() == hc::MqttState::disabled);
	CHECK(f.client.disconnects >= 1);
}

TEST_CASE("MQTT probe: an image that carries the broker password is refused under the mqtt path") {
	ServiceFixture f;
	std::string text = kImage;
	size_t at = text.find("\"baseTopic\"");
	REQUIRE(at != std::string::npos);
	text.insert(at, "\"password\": \"secret\", ");
	CHECK_FALSE(f.image.load(text.data(), text.size(), hc::image::Source::api, true));
	REQUIRE(f.image.report().count >= 1);
	CHECK(std::string(f.image.report().refusals[0].path).rfind("mqtt", 0) == 0);
	CHECK(std::string(f.image.report().refusals[0].reason).find("password") != std::string::npos);
	f.run();
	CHECK(f.client.connects == 0);
	CHECK(f.mqtt.state() == hc::MqttState::disabled);
}

TEST_CASE("MQTT probe: a datapoint that joins the model after the configuration is published after model_reloaded") {
	ServiceFixture f;
	std::string text = kImage;
	REQUIRE_MESSAGE(f.image.load(text.data(), text.size(), hc::image::Source::api, true), f.refusals());
	f.run();
	f.client.accept();
	f.run();
	// A third datapoint appears, as a provider's would (the image's keep their ids).
	hc::DatapointSpec specs[] = {
		{"hall.light", hc::ValueType::boolean, hc::DatapointFlags::kWritable},
		{"hall.light.state", hc::ValueType::boolean, 0},
		{"zigbee.lamp.on", hc::ValueType::boolean, hc::DatapointFlags::kWritable},
	};
	REQUIRE(f.model.load(specs, 3, true) == hc::DatapointStatus::ok);
	f.client.published.clear();
	REQUIRE(f.model.command(f.model.find("zigbee.lamp.on")->id(), Value::of_bool(true)) == hc::DatapointStatus::ok);
	f.run();
	CHECK(f.client.to("probe/house/dp/zigbee.lamp.on/state").empty()); // not in the table yet
	const char* error = nullptr;
	REQUIRE_MESSAGE(f.mqtt.model_reloaded(&error), (error ? error : ""));
	f.run();
	auto states = f.client.to("probe/house/dp/zigbee.lamp.on/state");
	REQUIRE_FALSE(states.empty());
	CHECK(states.back().payload.find("\"value\":true") != std::string::npos);
	// The function survived the reload.
	CHECK(f.client.under("homeassistant/light/homecontrol-probe1/hall-light/config").size() >= 1);
	CHECK(f.mqtt.state() == hc::MqttState::connected);
}

TEST_CASE("MQTT probe: mqtt.password is a write-only setting of the web API, mqtt.user a plain one") {
	hc_test::WebFixture f;
	std::string cookie = f.onboard();
	auto list = f.get("/api/settings", cookie);
	bool user = false;
	bool password = false;
	for (const auto& s : list.json["settings"].items) {
		if (s["key"].text == "mqtt.user") {
			user = true;
			CHECK(s["editable"].boolean);
			CHECK_FALSE(s.has("secret"));
		}
		if (s["key"].text == "mqtt.password") {
			password = true;
			CHECK(s["editable"].boolean);
			CHECK(s["secret"].boolean);
			CHECK(s["stored"].boolean == false);
		}
	}
	CHECK(user);
	CHECK(password);

	auto set = f.request(hc::HttpMethod::put, "/api/settings/mqtt.password", "{\"value\":\"s3cret-pw\"}", cookie);
	REQUIRE(set.status == 200);
	CHECK(f.fake.settings.entries["mqtt.password"].text == "s3cret-pw");
	for (const auto& s : set.json["settings"].items) {
		if (s["key"].text == "mqtt.password") {
			CHECK(s["stored"].boolean == true);
			CHECK(s["value"].kind == hc_test::Json::Kind::null); // never echoed
		}
	}
	CHECK(set.body.find("s3cret-pw") == std::string::npos);

	auto long_user = f.request(hc::HttpMethod::put, "/api/settings/mqtt.user", "{\"value\":\"" + std::string(64, 'u') + "\"}", cookie);
	CHECK(long_user.status == 422);
	auto ok_user = f.request(hc::HttpMethod::put, "/api/settings/mqtt.user", "{\"value\":\"hc\"}", cookie);
	CHECK(ok_user.status == 200);
	CHECK(f.fake.settings.entries["mqtt.user"].text == "hc");

	auto removed = f.request(hc::HttpMethod::put, "/api/settings/mqtt.password", "{\"value\":\"\"}", cookie);
	CHECK(removed.status == 200);
	CHECK(f.fake.settings.entries.count("mqtt.password") == 0);
	for (const auto& s : removed.json["settings"].items) {
		if (s["key"].text == "mqtt.password") {
			CHECK(s["stored"].boolean == false);
		}
	}
}
