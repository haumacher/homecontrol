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

TEST_CASE("Controller.HeartbeatLogsOnlyWhatChanged") {
	// homecontrol #98: the heartbeat runs every 10 s but writes a line only when the network changed
	// in kind (state, mode, address), so the log ring is not flooded.
	FakePlatform p;
	Controller controller(p.platform());
	controller.start();

	p.clock.advance(9999);
	controller.run_once();
	CHECK(p.sink.count("heartbeat") == 0);

	// The first beat reports the network the start began to connect (down at the start).
	p.clock.advance(1);
	controller.run_once();
	CHECK(p.sink.count("heartbeat") == 1);
	CHECK(p.sink.lines.back() ==
		"I hc.ctl: heartbeat: uptime 10 s, network connecting, free heap 300000 B (min 250000 B)");

	controller.connectivity().link_up("192.0.2.10");
	CHECK(p.sink.lines.back() == "I hc.net: network up 192.0.2.10");
	p.system.free_bytes = 280000;
	p.system.min_free_bytes = 270000;
	run_until(controller, p.clock, 1000 + 20000);
	CHECK(p.sink.count("heartbeat") == 2);
	CHECK(p.sink.lines.back() ==
		"I hc.ctl: heartbeat: uptime 20 s, network up 192.0.2.10, free heap 280000 B (min 270000 B)");

	// Steady state: ten minutes without a line, in the sink or the ring.
	uint32_t last = controller.log_ring().last_seq();
	p.system.free_bytes = 260000; // the heap moves, the network does not
	run_until(controller, p.clock, 1000 + 20000 + 600000);
	CHECK(p.sink.count("heartbeat") == 2);
	CHECK(controller.log_ring().last_seq() == last);
	CHECK(controller.uptime_ms() == 620000);

	// The address changes: one line.
	controller.connectivity().link_up("192.0.2.11");
	run_until(controller, p.clock, 1000 + 630000);
	CHECK(p.sink.count("heartbeat") == 3);
	CHECK(p.sink.contains("I hc.ctl: heartbeat: uptime 630 s, network up 192.0.2.11, free heap 260000 B"));

	controller.connectivity().link_down("test");
	run_until(controller, p.clock, 1000 + 640000);
	CHECK(p.sink.count("heartbeat") == 4);
	CHECK(p.sink.contains("heartbeat: uptime 640 s, network "));
}

TEST_CASE("Controller.HeartbeatWarnsOncePerNewHeapLowBelowTheLowWaterMark") {
	FakePlatform p;
	p.clock.synchronized = true;
	Controller controller(p.platform());
	controller.start();
	controller.connectivity().link_up("192.0.2.10");
	run_until(controller, p.clock, 1000 + 10000);
	REQUIRE(p.sink.count("heartbeat") == 1); // the network came up
	CHECK(p.sink.count("W hc.ctl") == 0);

	// A low minimum above the mark: nothing.
	p.system.min_free_bytes = Controller::kHeapLowWaterBytes;
	run_until(controller, p.clock, 1000 + 30000);
	CHECK(p.sink.count("heartbeat") == 1);

	// Below it: one warning with the figures and the time, not repeated while the minimum stays.
	p.system.free_bytes = 20000;
	p.system.min_free_bytes = 12000;
	run_until(controller, p.clock, 1000 + 40000);
	CHECK(p.sink.count("W hc.ctl") == 1);
	CHECK(p.sink.lines.back() == "W hc.ctl: heartbeat: uptime 40 s, network up 192.0.2.10, free heap 20000 B (min "
		"12000 B): the minimum is a new low below 16384 B");
	run_until(controller, p.clock, 1000 + 100000);
	CHECK(p.sink.count("W hc.ctl") == 1);

	// A new low: one more warning, in the ring too.
	p.system.min_free_bytes = 3188;
	run_until(controller, p.clock, 1000 + 110000);
	CHECK(p.sink.count("W hc.ctl") == 2);
	CHECK(p.sink.contains("(min 3188 B): the minimum is a new low below 16384 B"));
	struct Last : hc::LogLineVisitor {
		std::string text;
		bool line(const hc::LogLine& l) override {
			text = l.message;
			return true;
		}
	} newest;
	REQUIRE(controller.log_ring().read_tail(1, newest) == 1);
	CHECK(newest.text.find("min 3188 B") != std::string::npos);
	run_until(controller, p.clock, 1000 + 200000);
	CHECK(p.sink.count("W hc.ctl") == 2);
}

TEST_CASE("Controller.HeartbeatWarnsWithThePlatformsAccountOfTheLow") {
	// homecontrol #110: the platform measured the low at the allocation that reached it; the warning
	// carries its account, within one log line.
	FakePlatform p;
	Controller controller(p.platform());
	controller.start();
	controller.connectivity().link_up("192.168.178.86");
	run_until(controller, p.clock, 1000 + 10000);
	p.system.free_bytes = 48120;
	p.system.min_free_bytes = 2084;
	p.system.low_detail = "lowest 1840 B at 20.9 s: HP 0 B (block 0 B), LP 1840 B (block 1840 B); "
		"hc_ehome_tx took 1600 B DMA";
	run_until(controller, p.clock, 1000 + 20000);
	CHECK(p.sink.lines.back() == "W hc.ctl: heartbeat: uptime 20 s, network up 192.168.178.86, free heap 48120 B "
		"(min 2084 B): the minimum is a new low below 16384 B; lowest 1840 B at 20.9 s: HP 0 B (block 0 B), "
		"LP 1840 B (block 1840 B); hc_ehome_tx took 1600 B DMA");
	CHECK(p.sink.lines.back().size() - std::string("W hc.ctl: ").size() < hc::Log::kMaxMessage);

	// A detail longer than the room is cut, the line stays one line.
	p.system.min_free_bytes = 1000;
	p.system.low_detail = std::string(400, 'x');
	run_until(controller, p.clock, 1000 + 30000);
	CHECK(p.sink.count("W hc.ctl") == 2);
	CHECK(p.sink.lines.back().find("(min 1000 B): the minimum is a new low below 16384 B; xxx") != std::string::npos);
	CHECK(p.sink.lines.back().size() - std::string("W hc.ctl: ").size() < hc::Log::kMaxMessage);
}

TEST_CASE("Controller.HeartbeatReportsFailedAllocationsOncePerBeat") {
	// homecontrol #110: a failed allocation is news whatever the minimum says: one warning per beat
	// with the count and the first of them, nothing while none failed.
	FakePlatform p;
	Controller controller(p.platform());
	controller.start();
	controller.connectivity().link_up("192.0.2.10");
	run_until(controller, p.clock, 1000 + 10000);
	CHECK(p.sink.count("allocation") == 0);

	p.system.failures = 3;
	p.system.failure_detail =
		"1600 B DMA for hc_ehome_tx at 20.94 s; free 1840 B: HP 0 B (block 0 B), LP 1840 B (block 1840 B)";
	run_until(controller, p.clock, 1000 + 20000);
	CHECK(p.sink.count("allocation") == 1);
	CHECK(p.sink.lines.back() == "W hc.ctl: 3 allocation(s) failed since the last heartbeat, the first: 1600 B DMA "
		"for hc_ehome_tx at 20.94 s; free 1840 B: HP 0 B (block 0 B), LP 1840 B (block 1840 B)");
	CHECK(p.system.failures == 0); // taken

	run_until(controller, p.clock, 1000 + 60000);
	CHECK(p.sink.count("allocation") == 1);

	// The POSIX platform counts none: the default says 0.
	struct Plain : hc::SystemInfo {
		hc::MemoryInfo memory() const override { return hc::MemoryInfo{1, 1}; }
	} plain;
	char text[16] = "untouched";
	CHECK(plain.failed_allocations(text, sizeof(text)) == 0);
	CHECK(!plain.heap_low(text, sizeof(text)));
	CHECK(std::string(text) == "untouched");
}

TEST_CASE("Controller.HeartbeatNamesTheLowOfItsWindowWhenItIsNoNewLowSinceBoot") {
	// homecontrol #110: at 900 s the house controller fell to 1880 B of HP SRAM without a new low
	// since boot; the window's low names that moment and its task, once per beat.
	FakePlatform p;
	Controller controller(p.platform());
	controller.start();
	controller.connectivity().link_up("192.0.2.10");
	p.system.min_free_bytes = 1144;
	p.system.low_detail = "lowest 2140 B at 21.05 s: HP 1136 B (block 448 B), LP 1004 B (block 640 B); hc_fetch took 516 B";
	run_until(controller, p.clock, 1000 + 30000);
	REQUIRE(p.sink.count("new low below") == 1);
	CHECK(p.sink.count("heap low in the last") == 0); // the boot-wide line names it already

	p.system.window_detail = "lowest 10024 B at 900.58 s: HP 1880 B (block 512 B), LP 8144 B (block 8064 B); tiT took "
		"1682 B DMA";
	run_until(controller, p.clock, 1000 + 40000);
	CHECK(p.sink.count("heap low in the last") == 1);
	CHECK(p.sink.lines.back() == "W hc.ctl: heap low in the last 10 s: lowest 10024 B at 900.58 s: HP 1880 B (block "
		"512 B), LP 8144 B (block 8064 B); tiT took 1682 B DMA");
	CHECK(p.sink.count("new low below") == 1); // the boot-wide line stays as it is

	// A window without a low below the mark: nothing.
	run_until(controller, p.clock, 1000 + 100000);
	CHECK(p.sink.count("heap low in the last") == 1);
}

TEST_CASE("Controller.HeartbeatWarnsNothingWithoutAMeasuredHeap") {
	FakePlatform p;
	p.system.free_bytes = 0; // the POSIX platform has no heap figure
	p.system.min_free_bytes = 0;
	Controller controller(p.platform());
	controller.start();
	run_until(controller, p.clock, 1000 + 60000);
	CHECK(p.sink.count("W hc.ctl") == 0);
}
