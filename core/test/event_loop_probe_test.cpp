/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe tests written at review time: they compose the event loop's features (generations,
// periodic timers, cancellation from handlers, pool exhaustion, the controller's heartbeat) in
// ways the implementation was not written against. See the workflow skill, section 4.

#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "doctest.h"
#include "hc/controller.h"
#include "hc/event_loop.h"
#include "test_support.h"

using hc::Callback;
using hc::EventLoop;
using hc::TimerHandle;
using hc_test::ScopedCapture;

namespace {

struct Probe {
	std::function<void()> body;
	void run() { body(); }
	Callback callback() { return Callback::member<Probe, &Probe::run>(this); }
};

struct Probes {
	std::deque<Probe> probes;
	std::vector<std::string> order;

	Callback make(const std::string& name, std::function<void()> extra = nullptr) {
		probes.push_back(Probe{[this, name, extra]() {
			order.push_back(name);
			if (extra) {
				extra();
			}
		}});
		return probes.back().callback();
	}
};

} // namespace

TEST_CASE("EventLoopProbe.ThousandGenerationsOfOneSlotKeepStaleHandlesHarmless") {
	ScopedCapture capture;
	EventLoop loop;
	Probes p;
	// Fill all but one slot so every schedule below reuses the same free slot.
	std::vector<TimerHandle> filler;
	for (size_t i = 0; i + 1 < EventLoop::kTimerCapacity; ++i) {
		filler.push_back(loop.schedule_at(1000000, p.make("filler")));
	}
	TimerHandle first = loop.schedule_at(10, p.make("gen0"));
	REQUIRE(first.valid());
	TimerHandle previous = first;
	for (int generation = 1; generation <= 1000; ++generation) {
		REQUIRE(loop.cancel(previous));
		TimerHandle next = loop.schedule_at(10, p.make("gen" + std::to_string(generation)));
		REQUIRE(next.valid());
		CHECK(next != previous);
		CHECK_FALSE(loop.cancel(first));    // the oldest handle never cancels the new occupant
		CHECK_FALSE(loop.cancel(previous)); // nor does the one just cancelled
		previous = next;
	}
	loop.run_once(10);
	CHECK(p.order == std::vector<std::string>{"gen1000"});
	CHECK(loop.timer_count() == EventLoop::kTimerCapacity - 1);
	CHECK(capture.sink.count("E hc.loop") == 0);
}

TEST_CASE("EventLoopProbe.OneShotCancelsPeriodicWhileBothDueInSameRun") {
	ScopedCapture capture;
	EventLoop loop;
	Probes p;
	TimerHandle periodic = loop.schedule_periodic(100, 100, p.make("tick"));
	// Due in the same run_once as the third tick. The periodic timer is re-armed with a new
	// scheduling order after every tick, so the one-shot scheduled here, earlier, runs first and
	// cancels the third tick before it fires.
	loop.schedule_at(300, p.make("stop", [&]() { CHECK(loop.cancel(periodic)); }));
	for (uint64_t now = 0; now <= 1000; now += 10) {
		loop.run_once(now);
	}
	CHECK(p.order == std::vector<std::string>{"tick", "tick", "stop"});
	CHECK(loop.timer_count() == 0);
	CHECK(loop.next_deadline() == EventLoop::kNever);
	CHECK(capture.sink.count("W hc.loop") == 0);
}

TEST_CASE("EventLoopProbe.PeriodicCancelledByEarlierTimerInSameRunNeverFires") {
	ScopedCapture capture;
	EventLoop loop;
	Probes p;
	TimerHandle periodic = loop.schedule_periodic(500, 100, p.make("tick"));
	loop.schedule_at(500, p.make("killer", [&]() { CHECK(loop.cancel(periodic)); }));
	// Both due at 500; the periodic was scheduled first and fires first; the killer then cancels
	// it so it must not fire again at 600 even though 600 is also due in this (late) run.
	loop.run_once(700);
	CHECK(p.order == std::vector<std::string>{"tick", "killer"});
	loop.run_once(1000);
	CHECK(p.order.size() == 2);
	CHECK(loop.timer_count() == 0);
}

TEST_CASE("EventLoopProbe.ClockJumpFiresPeriodicOnceAndKeepsTheGrid") {
	ScopedCapture capture;
	EventLoop loop;
	Probes p;
	loop.schedule_periodic(10000, 10000, p.make("tick"));
	loop.run_once(10000);
	REQUIRE(p.order.size() == 1);
	// The platform slept through 3.5 periods (a suspend, a long blocking call): the deadline at
	// 20000 fires now, the ones at 30000 and 40000 are the two missed periods.
	loop.run_once(45000);
	CHECK(p.order.size() == 2);
	CHECK(loop.next_deadline() == 50000);
	CHECK(capture.sink.count("W hc.loop") == 1);
	CHECK(capture.sink.contains("missed 2 period(s), next at 50000 ms"));
	loop.run_once(50000);
	CHECK(p.order.size() == 3);
	CHECK(loop.next_deadline() == 60000);
}

TEST_CASE("EventLoopProbe.PoolRecoversAfterExhaustionThroughCancelAndFiring") {
	ScopedCapture capture;
	EventLoop loop;
	Probes p;
	std::vector<TimerHandle> handles;
	for (size_t i = 0; i < EventLoop::kTimerCapacity; ++i) {
		handles.push_back(loop.schedule_at(100 + i, p.make("t")));
	}
	CHECK_FALSE(loop.schedule_at(50, p.make("refused")).valid());
	CHECK(capture.sink.count("E hc.loop") == 1);
	// Cancelling one makes room for exactly one.
	REQUIRE(loop.cancel(handles[7]));
	TimerHandle replacement = loop.schedule_at(1, p.make("replacement"));
	CHECK(replacement.valid());
	CHECK_FALSE(loop.schedule_at(1, p.make("refused again")).valid());
	CHECK(capture.sink.count("E hc.loop") == 2);
	// Firing everything frees everything; a full new set fits.
	loop.run_once(1000);
	CHECK(p.order.size() == EventLoop::kTimerCapacity);
	CHECK(p.order.front() == "replacement");
	CHECK(loop.timer_count() == 0);
	for (size_t i = 0; i < EventLoop::kTimerCapacity; ++i) {
		CHECK(loop.schedule_at(2000, p.make("again")).valid());
	}
	CHECK(capture.sink.count("E hc.loop") == 2);
}

TEST_CASE("EventLoopProbe.HandlerReschedulingAtTheSameDeadlineDefersToNextRun") {
	ScopedCapture capture;
	EventLoop loop;
	Probes p;
	loop.schedule_at(100, p.make("first", [&]() {
		CHECK(loop.schedule_at(100, p.make("second")).valid());
	}));
	loop.run_once(100);
	CHECK(p.order == std::vector<std::string>{"first"});
	CHECK(loop.next_deadline() == 100); // due: the platform must call again without sleeping
	loop.run_once(100);
	CHECK(p.order == std::vector<std::string>{"first", "second"});
	CHECK(loop.next_deadline() == EventLoop::kNever);
}

TEST_CASE("ControllerProbe.HeartbeatSurvivesClockJumpWithOneBeatAndRightUptime") {
	ScopedCapture capture;
	hc_test::FakePlatform p;
	p.clock.synchronized = true; // no clock watch: only the heartbeat and the connect timeout run
	hc::Controller controller(p.platform());
	controller.start();
	controller.connectivity().link_up("192.0.2.10"); // no connect timeout either
	controller.run_once();
	p.clock.advance(25000);
	controller.run_once();
	CHECK(p.sink.count("heartbeat") == 1);
	CHECK(p.sink.contains("heartbeat: uptime 25 s"));
	CHECK(controller.uptime_ms() == 25000);
	CHECK(p.sink.count("W hc.loop") == 1); // the missed period is visible, not silent
	CHECK(controller.next_deadline() == p.clock.now_ms + 5000); // back on the 10 s grid
	p.clock.advance(5000);
	controller.run_once();
	CHECK(p.sink.count("heartbeat") == 2);
}
