/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <string>

#include "doctest.h"
#include "hc/controller.h"
#include "hc/version.h"
#include "test_support.h"

using hc::Controller;
using hc::Log;
using hc_test::CapturingSink;
using hc_test::FakeClock;
using hc_test::FakeSystemInfo;

namespace {

/// Drives the controller like a platform: jump the clock to each deadline up to `until_ms`.
void run_until(Controller& controller, FakeClock& clock, uint64_t until_ms) {
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
	FakeClock clock;
	FakeSystemInfo system;
	CapturingSink sink;
	hc::LogSink* previous = Log::sink();
	{
		Controller controller(clock, sink, system);
		CHECK(Log::sink() == &sink);
	}
	CHECK(Log::sink() == nullptr);
	Log::set_sink(previous);
}

TEST_CASE("Controller.StartLogsVersionAndFreeHeap") {
	FakeClock clock;
	FakeSystemInfo system;
	CapturingSink sink;
	Controller controller(clock, sink, system);
	controller.start();

	REQUIRE(sink.lines.size() >= 2);
	CHECK(sink.lines[0] == std::string("I hc.ctl: HomeControl 0.1.0-dev (build ") + hc::build_id() + ") starting");
	CHECK(sink.contains("I hc.ctl: wall clock not synchronized (reads 0 s)"));
	CHECK(sink.contains("I hc.ctl: free heap 300000 B (min 250000 B)"));
	CHECK(controller.started());
	CHECK(controller.next_deadline() == 1000 + Controller::kHeartbeatPeriodMs);
}

TEST_CASE("Controller.SynchronizedWallClockIsNotReported") {
	FakeClock clock;
	clock.synchronized = true;
	clock.wall_seconds = 1792224000;
	FakeSystemInfo system;
	CapturingSink sink;
	Controller controller(clock, sink, system);
	controller.start();
	CHECK_FALSE(sink.contains("wall clock not synchronized"));
}

TEST_CASE("Controller.SecondStartIsRefused") {
	FakeClock clock;
	FakeSystemInfo system;
	CapturingSink sink;
	Controller controller(clock, sink, system);
	controller.start();
	controller.start();
	CHECK(sink.count("starting") == 1);
	CHECK(sink.contains("E hc.ctl: refusing start: controller already started"));
	CHECK(controller.loop().timer_count() == 1);
}

TEST_CASE("Controller.HeartbeatEveryTenSeconds") {
	FakeClock clock;
	FakeSystemInfo system;
	CapturingSink sink;
	Controller controller(clock, sink, system);
	controller.start();

	clock.advance(9999);
	controller.run_once();
	CHECK(sink.count("heartbeat") == 0);

	clock.advance(1);
	controller.run_once();
	CHECK(sink.count("heartbeat") == 1);
	CHECK(sink.lines.back() == "I hc.ctl: heartbeat: uptime 10 s, free heap 300000 B (min 250000 B)");

	system.free_bytes = 280000;
	system.min_free_bytes = 270000;
	run_until(controller, clock, 1000 + 60000);
	CHECK(sink.count("heartbeat") == 6);
	CHECK(sink.lines.back() == "I hc.ctl: heartbeat: uptime 60 s, free heap 280000 B (min 270000 B)");
	CHECK(controller.uptime_ms() == 60000);
}
