/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <string>

#include "doctest.h"
#include "hc/controller.h"
#include "hc/status_led.h"
#include "test_support.h"

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
