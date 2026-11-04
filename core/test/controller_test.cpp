/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <string>

#include "doctest.h"
#include "hc/controller.h"
#include "hc/network_settings.h"
#include "hc/version.h"
#include "test_support.h"

using hc::Controller;
using hc::Log;
using hc_test::FakePlatform;

namespace {

/// Drives the controller like a platform: jump the clock to each deadline up to `until_ms`.
void run_until(Controller& controller, hc_test::FakeClock& clock, uint64_t until_ms) {
	for (;;) {
		controller.run_once();
		uint64_t deadline = controller.next_deadline();
		if (deadline > until_ms) {
			clock.now_ms = until_ms;
			controller.run_once();
			return;
		}
		if (deadline > clock.now_ms) {
			clock.now_ms = deadline;
		}
	}
}

} // namespace

TEST_CASE("Controller.InstallsAndRemovesLogSink") {
	FakePlatform p;
	hc::LogSink* previous = Log::sink();
	{
		Controller controller(p.platform());
		CHECK(Log::sink() == &p.sink);
	}
	CHECK(Log::sink() == nullptr);
	Log::set_sink(previous);
}

TEST_CASE("Controller.StartLogsVersionAndFreeHeap") {
	FakePlatform p;
	Controller controller(p.platform());
	controller.start();

	REQUIRE(p.sink.lines.size() >= 2);
	CHECK(p.sink.lines[0] == std::string("I hc.ctl: HomeControl ") + hc::kVersion + " (build " + hc::build_id() + ") starting, reset reason: unknown");
	CHECK(p.sink.contains("I hc.ctl: wall clock not synchronized (reads 0 s)"));
	CHECK(p.sink.contains("I hc.ctl: free heap 300000 B (min 250000 B)"));
	CHECK(controller.started());
	// The clock watch runs every second until the wall clock is synchronized.
	CHECK(controller.next_deadline() == 1000 + Controller::kClockWatchPeriodMs);
}

TEST_CASE("Controller.SynchronizedWallClockLogsLocalTimeAtStart") {
	FakePlatform p;
	p.clock.synchronized = true;
	p.clock.wall_seconds = 1792224000; // 2026-10-17T08:00:00Z
	Controller controller(p.platform());
	controller.start();
	CHECK_FALSE(p.sink.contains("wall clock not synchronized"));
	CHECK(p.sink.contains("I hc.ctl: wall clock synchronized: 2026-10-17 10:00:00 CEST (UTC+02:00)"));
	CHECK(controller.next_deadline() == 1000 + Controller::kHeartbeatPeriodMs);
}

TEST_CASE("Controller.LogsOnceWhenTheClockTurnsSynchronized") {
	FakePlatform p;
	Controller controller(p.platform());
	controller.start();
	run_until(controller, p.clock, 5000);
	CHECK(p.sink.count("wall clock synchronized") == 0);

	// SNTP delivers between two watch ticks.
	p.clock.wall_seconds = 1792224000;
	p.clock.synchronized = true;
	run_until(controller, p.clock, 5500);
	CHECK(p.sink.count("wall clock synchronized") == 0);
	run_until(controller, p.clock, 6000);
	CHECK(p.sink.count("I hc.ctl: wall clock synchronized: 2026-10-17 10:00:00 CEST (UTC+02:00)") == 1);

	run_until(controller, p.clock, 60000);
	CHECK(p.sink.count("wall clock synchronized") == 1);
	CHECK(controller.loop().timer_count() == 2); // heartbeat and the connect timeout; the watch is gone
}

TEST_CASE("Controller.AppliesTheDefaultTimeZoneWhenNoneIsStored") {
	FakePlatform p;
	Controller controller(p.platform());
	controller.start();
	REQUIRE(p.time_zone.applied.size() == 1);
	CHECK(p.time_zone.applied[0] == hc::setting::kDefaultTimeZone);
	CHECK(std::string(controller.time_zone()) == hc::setting::kDefaultTimeZone);
	CHECK(p.sink.contains("I hc.ctl: time zone CET-1CEST,M3.5.0,M10.5.0/3"));
	CHECK(p.sink.count("E ") == 0);
}

TEST_CASE("Controller.AppliesTheStoredTimeZone") {
	FakePlatform p;
	REQUIRE(p.settings.set_string("tz", "GMT0BST,M3.5.0/1,M10.5.0") == hc::SettingsStatus::ok);
	Controller controller(p.platform());
	controller.start();
	REQUIRE(p.time_zone.applied.size() == 1);
	CHECK(p.time_zone.applied[0] == "GMT0BST,M3.5.0/1,M10.5.0");
}

TEST_CASE("Controller.InvalidStoredTimeZoneFallsBackToTheDefaultAndSaysSo") {
	FakePlatform p;
	REQUIRE(p.settings.set_string("tz", "CET-1CEST") == hc::SettingsStatus::ok);
	Controller controller(p.platform());
	controller.start();
	REQUIRE(p.time_zone.applied.size() == 1);
	CHECK(p.time_zone.applied[0] == hc::setting::kDefaultTimeZone);
	CHECK(p.sink.contains("E hc.ctl: setting tz is invalid (a zone with daylight saving time needs both transition rules"));
}

TEST_CASE("Controller.WrongTypedTimeZoneFallsBackToTheDefaultAndSaysSo") {
	FakePlatform p;
	REQUIRE(p.settings.set_int("tz", 1) == hc::SettingsStatus::ok);
	Controller controller(p.platform());
	controller.start();
	CHECK(p.time_zone.applied[0] == hc::setting::kDefaultTimeZone);
	CHECK(p.sink.contains("E hc.ctl: setting tz unreadable (stored with another type)"));
}

TEST_CASE("Controller.SetTimeZoneStoresAppliesAndRefusesGarbage") {
	FakePlatform p;
	p.clock.synchronized = true;
	p.clock.wall_seconds = 1792224000;
	Controller controller(p.platform());
	controller.start();

	CHECK(controller.set_time_zone("UTC0"));
	CHECK(p.time_zone.applied.back() == "UTC0");
	char stored[64];
	CHECK(p.settings.get_string("tz", stored, sizeof(stored)) == hc::SettingsStatus::ok);
	CHECK(std::string(stored) == "UTC0");
	CHECK(p.sink.contains("I hc.ctl: time zone set to UTC0, local time 2026-10-17 10:00:00 CEST (UTC+02:00)"));

	const char* error = nullptr;
	CHECK_FALSE(controller.set_time_zone("Europe/Berlin", &error));
	CHECK(error != nullptr);
	CHECK(p.time_zone.applied.back() == "UTC0");
	CHECK(p.sink.contains("E hc.ctl: refusing time zone \"Europe/Berlin\""));
	CHECK(std::string(controller.time_zone()) == "UTC0");

	p.settings.fail_writes = true;
	CHECK_FALSE(controller.set_time_zone("CET-1", &error));
	CHECK(std::string(error) == "storage error");
	CHECK(std::string(controller.time_zone()) == "UTC0");
}

TEST_CASE("Controller.SecondStartIsRefused") {
	FakePlatform p;
	Controller controller(p.platform());
	controller.start();
	size_t timers = controller.loop().timer_count();
	controller.start();
	CHECK(p.sink.count("starting") == 1);
	CHECK(p.sink.contains("E hc.ctl: refusing start: controller already started"));
	CHECK(controller.loop().timer_count() == timers);
	CHECK(p.network.connects.size() == 1);
}

TEST_CASE("Controller.StartConnectsTheNetwork") {
	FakePlatform p;
	Controller controller(p.platform());
	controller.start();
	CHECK(p.network.connects == std::vector<uint64_t>{1000});
	CHECK(controller.connectivity().state() == hc::LinkState::connecting);
	CHECK(p.sink.contains("I hc.net: network connecting (attempt 1)"));
}

TEST_CASE("Controller.HeartbeatEveryTenSecondsWithNetworkState") {
	FakePlatform p;
	Controller controller(p.platform());
	controller.start();

	p.clock.advance(9999);
	controller.run_once();
	CHECK(p.sink.count("heartbeat") == 0);

	p.clock.advance(1);
	controller.run_once();
	CHECK(p.sink.count("heartbeat") == 1);
	CHECK(p.sink.lines.back() ==
		"I hc.ctl: heartbeat: uptime 10 s, network connecting, free heap 300000 B (min 250000 B)");

	controller.connectivity().link_up("192.0.2.10");
	CHECK(p.sink.lines.back() == "I hc.net: network up 192.0.2.10");
	p.system.free_bytes = 280000;
	p.system.min_free_bytes = 270000;
	run_until(controller, p.clock, 1000 + 60000);
	CHECK(p.sink.count("heartbeat") == 6);
	CHECK(p.sink.lines.back() ==
		"I hc.ctl: heartbeat: uptime 60 s, network up 192.0.2.10, free heap 280000 B (min 270000 B)");
	CHECK(controller.uptime_ms() == 60000);

	controller.connectivity().link_down("test");
	p.clock.advance(500);
	controller.run_once();
	run_until(controller, p.clock, 1000 + 70000);
	CHECK(p.sink.contains("heartbeat: uptime 70 s, network "));
}
