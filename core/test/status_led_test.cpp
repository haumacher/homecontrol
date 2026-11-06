/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <string>

#include "doctest.h"
#include "hc/controller.h"
#include "hc/status_led.h"
#include "test_support.h"
#include "web_test_support.h"

using hc::LedStatus;
using hc::StatusLed;

namespace {

/// The levels of `ticks` consecutive ticks as a string of '#' (lit) and '.' (dark).
std::string levels(StatusLed& led, int ticks) {
	std::string result;
	for (int i = 0; i < ticks; ++i) {
		result += led.tick() ? '#' : '.';
	}
	return result;
}

} // namespace

TEST_CASE("StatusLed.PatternsOverOneSecond") {
	StatusLed led;
	led.set_status(LedStatus::pairing);
	CHECK(levels(led, 20) == "##..##..##..##..##..");
	led.set_status(LedStatus::connecting);
	CHECK(levels(led, 20) == "##########..........");
	led.set_status(LedStatus::setup);
	CHECK(levels(led, 20) == "##..................");
	led.set_status(LedStatus::degraded);
	CHECK(levels(led, 20) == "##################..");
	led.set_status(LedStatus::ready);
	CHECK(levels(led, 20) == "####################");
}

TEST_CASE("StatusLed.AChangeRestartsThePatternButTheSameStatusDoesNot") {
	StatusLed led;
	led.set_status(LedStatus::setup);
	CHECK(levels(led, 3) == "##.");
	led.set_status(LedStatus::setup);
	CHECK(levels(led, 17) == ".................");
	CHECK(levels(led, 2) == "##"); // wrapped after 20 ticks
	led.set_status(LedStatus::pairing);
	CHECK(levels(led, 4) == "##..");
	CHECK(std::string(hc::led_status_name(LedStatus::degraded)) == "degraded");
}

TEST_CASE("StatusLed.QuietDarkensOnlyReady") {
	StatusLed led;
	led.set_quiet(true);
	led.set_status(LedStatus::ready);
	CHECK(levels(led, 40) == "........................................");
	// The other four states keep their patterns.
	led.set_status(LedStatus::pairing);
	CHECK(levels(led, 20) == "##..##..##..##..##..");
	led.set_status(LedStatus::connecting);
	CHECK(levels(led, 20) == "##########..........");
	led.set_status(LedStatus::setup);
	CHECK(levels(led, 20) == "##..................");
	led.set_status(LedStatus::degraded);
	CHECK(levels(led, 20) == "##################..");
	led.set_status(LedStatus::ready);
	CHECK(levels(led, 20) == "....................");
	CHECK(StatusLed::pattern(LedStatus::ready, true).on_mask == 0);
	CHECK(StatusLed::pattern(LedStatus::ready).on_mask == 1);
}

TEST_CASE("StatusLed.SwitchingQuietWhileReadyTakesEffectOnTheNextTick") {
	StatusLed led;
	led.set_status(LedStatus::ready);
	CHECK(levels(led, 3) == "###");
	led.set_quiet(true);
	CHECK(levels(led, 1) == ".");
	led.set_quiet(false);
	CHECK(levels(led, 1) == "#");
	// Quiet in another state: the pattern runs on, unchanged and not restarted.
	led.set_status(LedStatus::setup);
	CHECK(levels(led, 3) == "##.");
	led.set_quiet(true);
	CHECK(levels(led, 17) == ".................");
	CHECK(levels(led, 2) == "##");
}

namespace {

struct ImageReady : hc::LedInput {
	bool loaded = true;
	bool degraded = false;
	bool image_loaded() const override { return loaded; }
	bool image_degraded() const override { return degraded; }
};

/// Runs the controller for `ticks` LED ticks; returns the level the light shows after each.
std::string drive(hc::Controller& controller, hc_test::FakePlatform& p, hc_test::FakeStatusLight& light, int ticks) {
	std::string result;
	for (int i = 0; i < ticks; ++i) {
		controller.run_once();
		bool on = !light.changes.empty() && light.changes.back().second;
		result += on ? '#' : '.';
		p.clock.advance(StatusLed::kTickMs);
	}
	return result;
}

} // namespace

TEST_CASE("Controller.ReadsLedQuietOnStartAndAppliesAChangeWithinOneTick") {
	hc_test::FakePlatform p;
	p.clock.synchronized = true;
	p.network.has_credentials = true;
	REQUIRE(p.settings.set_bool(hc::setting::kLedQuiet, true) == hc::SettingsStatus::ok);
	hc_test::FakeStatusLight light(p.clock);
	hc::Platform platform = p.platform();
	platform.status_light = &light;
	hc::Controller controller(platform);
	ImageReady image;
	controller.set_led_input(&image);
	controller.start();
	CHECK(controller.led_quiet());
	CHECK(p.sink.contains("status LED quiet: dark while ready"));
	controller.connectivity().link_up("192.0.2.10");
	REQUIRE(controller.led_status() == LedStatus::ready);
	CHECK(drive(controller, p, light, 20) == "....................");

	// Degraded lights it with the dropout pattern; the status still names the state.
	image.degraded = true;
	CHECK(drive(controller, p, light, 20) == "##################..");
	image.degraded = false;
	CHECK(drive(controller, p, light, 5) == ".....");
	CHECK(std::string(hc::led_status_name(controller.led_status())) == "ready");

	// Switched off while ready: lit at the next tick; on again: dark at the next tick, stored.
	REQUIRE(controller.set_led_quiet(false));
	CHECK(drive(controller, p, light, 1) == "#");
	REQUIRE(controller.set_led_quiet(true));
	CHECK(drive(controller, p, light, 1) == ".");
	bool stored = false;
	CHECK(p.settings.get_bool(hc::setting::kLedQuiet, stored) == hc::SettingsStatus::ok);
	CHECK(stored);

	// A failed store changes nothing.
	p.settings.fail_writes = true;
	const char* error = nullptr;
	CHECK_FALSE(controller.set_led_quiet(false, &error));
	CHECK(error != nullptr);
	CHECK(controller.led_quiet());
	CHECK(drive(controller, p, light, 3) == "...");
}

TEST_CASE("Controller.AnUnreadableLedQuietIsOff") {
	hc_test::FakePlatform p;
	REQUIRE(p.settings.set_string(hc::setting::kLedQuiet, "yes") == hc::SettingsStatus::ok); // wrong type
	hc::Controller controller(p.platform());
	controller.start();
	CHECK_FALSE(controller.led_quiet());
	CHECK(p.sink.contains("setting led.quiet unreadable"));
}

TEST_CASE("WebApi.LedQuietIsAnOnOffDeviceSetting") {
	hc_test::WebFixture f;
	std::string cookie = f.onboard();
	auto find = [](const hc_test::Json& list) -> const hc_test::Json* {
		for (const auto& s : list["settings"].items) {
			if (s["key"].text == "led.quiet") {
				return &s;
			}
		}
		return nullptr;
	};
	auto list = f.get("/api/settings", cookie);
	REQUIRE(list.status == 200);
	const hc_test::Json* entry = find(list.json);
	REQUIRE(entry != nullptr);
	CHECK((*entry)["value"].text == "off");
	CHECK((*entry)["default"].text == "off");
	CHECK((*entry)["editable"].boolean);
	CHECK_FALSE(f.controller.led_quiet());

	struct Case {
		const char* value;
		bool quiet;
		const char* shown;
	};
	for (Case c : {Case{"on", true, "on"}, Case{"off", false, "off"}, Case{"true", true, "on"}, Case{"false", false, "off"}}) {
		CAPTURE(c.value);
		auto r = f.request(hc::HttpMethod::put, "/api/settings/led.quiet", std::string("{\"value\":\"") + c.value + "\"}",
			cookie);
		REQUIRE_MESSAGE(r.status == 200, r.body);
		CHECK(f.controller.led_quiet() == c.quiet);
		bool stored = !c.quiet;
		CHECK(f.fake.settings.get_bool(hc::setting::kLedQuiet, stored) == hc::SettingsStatus::ok);
		CHECK(stored == c.quiet);
		const hc_test::Json* shown = find(r.json);
		REQUIRE(shown != nullptr);
		CHECK((*shown)["value"].text == c.shown);
	}

	f.request(hc::HttpMethod::put, "/api/settings/led.quiet", "{\"value\":\"on\"}", cookie);
	for (const char* bad : {"maybe", "ON", "1", ""}) {
		CAPTURE(bad);
		auto r = f.request(hc::HttpMethod::put, "/api/settings/led.quiet", std::string("{\"value\":\"") + bad + "\"}",
			cookie);
		CHECK(r.status == 422);
		CHECK(r.json["code"].text == "settings.invalid");
		CHECK(r.json["error"].text == "led.quiet is on or off");
		CHECK(f.controller.led_quiet()); // unchanged
	}

	// The status keeps naming the state, not the light's level.
	auto status = f.get("/api/status", cookie);
	CHECK(status.json["led"].kind == hc_test::Json::Kind::string);
}

TEST_CASE("Controller.DrivesTheStatusLightFromTheNetworkState") {
	hc_test::FakePlatform p;
	p.clock.synchronized = true;
	p.network.has_credentials = false; // boots into pairing
	hc_test::FakeStatusLight light(p.clock);
	hc::Platform platform = p.platform();
	platform.status_light = &light;
	hc::Controller controller(platform);
	controller.start();
	CHECK(controller.led_status() == LedStatus::pairing);

	// Drive in 50 ms steps for 400 ms: fast blink, a change every 100 ms.
	for (int i = 0; i < 8; ++i) {
		controller.run_once();
		p.clock.advance(StatusLed::kTickMs);
	}
	REQUIRE(light.changes.size() >= 5);
	CHECK(light.changes[0] == std::make_pair(uint64_t{1000}, false)); // dark at start
	CHECK(light.changes[1] == std::make_pair(uint64_t{1000}, true));
	CHECK(light.changes[2] == std::make_pair(uint64_t{1100}, false));
	CHECK(light.changes[3] == std::make_pair(uint64_t{1200}, true));
	CHECK(light.changes[4] == std::make_pair(uint64_t{1300}, false));

	// Paired and connecting: slow blink.
	p.network.has_credentials = true;
	controller.connectivity().pairing_succeeded("test");
	p.clock.advance(hc::Connectivity::kSettleMs);
	controller.run_once();
	CHECK(controller.led_status() == LedStatus::connecting);

	// Up: short pulse (no runtime image yet).
	controller.connectivity().link_up("192.0.2.10");
	CHECK(controller.led_status() == LedStatus::setup);
	light.changes.clear();
	for (int i = 0; i < 40; ++i) {
		controller.run_once();
		p.clock.advance(StatusLed::kTickMs);
	}
	// The light was lit (slow blink) and the pulse starts lit, so the first change is the end of the
	// first 100 ms pulse; the next pulse follows 900 ms later and lasts 100 ms.
	REQUIRE(light.changes.size() == 3);
	CHECK_FALSE(light.changes[0].second);
	CHECK(light.changes[1].second);
	CHECK(light.changes[1].first - light.changes[0].first == 900);
	CHECK(light.changes[2].first - light.changes[1].first == 100);
}

TEST_CASE("Controller.HeartbeatNamesAnUnusualNetworkMode") {
	hc_test::FakePlatform p;
	p.network.has_credentials = false;
	hc::Controller controller(p.platform());
	controller.start();
	p.clock.advance(hc::Controller::kHeartbeatPeriodMs);
	controller.run_once();
	CHECK(p.sink.contains("heartbeat: uptime 10 s, network down, unprovisioned, free heap"));
}
