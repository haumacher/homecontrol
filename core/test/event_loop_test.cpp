/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <algorithm>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "doctest.h"
#include "hc/event_loop.h"
#include "test_support.h"

using hc::Callback;
using hc::EventLoop;
using hc::TimerHandle;
using hc_test::ScopedCapture;

namespace {

/// A handler with arbitrary test behavior. Tests may allocate; the loop does not.
struct Action {
	std::function<void()> body;

	void run() { body(); }

	Callback callback() { return Callback::member<Action, &Action::run>(this); }
};

/// Records the order in which named handlers run. Actions live in a deque, so their addresses
/// stay stable while more are added.
struct Recorder {
	std::vector<std::string> order;
	std::deque<Action> actions;

	Callback make(const std::string& name, std::function<void()> extra = nullptr) {
		actions.push_back(Action{[this, name, extra]() {
			order.push_back(name);
			if (extra) {
				extra();
			}
		}});
		return actions.back().callback();
	}
};

using Names = std::vector<std::string>;

} // namespace

TEST_CASE("EventLoop.TimersFireByDeadline") {
	EventLoop loop;
	Recorder rec;
	loop.schedule_at(300, rec.make("c"));
	loop.schedule_at(100, rec.make("a"));
	loop.schedule_at(200, rec.make("b"));

	loop.run_once(1000);
	CHECK(rec.order == Names{"a", "b", "c"});
	CHECK(loop.timer_count() == 0);
}

TEST_CASE("EventLoop.EqualDeadlinesFireInSchedulingOrder") {
	EventLoop loop;
	Recorder rec;
	loop.schedule_at(500, rec.make("first"));
	loop.schedule_at(100, rec.make("early"));
	loop.schedule_at(500, rec.make("second"));
	loop.schedule_at(500, rec.make("third"));
	loop.schedule_at(900, rec.make("late"));
	loop.schedule_at(500, rec.make("fourth"));

	loop.run_once(500);
	CHECK(rec.order == Names{"early", "first", "second", "third", "fourth"});
	loop.run_once(900);
	CHECK(rec.order.back() == "late");
}

TEST_CASE("EventLoop.OrderingSurvivesManyInterleavedTimers") {
	// Exercises the heap with removals in the middle: schedule a full pool in a scrambled order,
	// cancel every third, check the rest fires sorted by (deadline, scheduling order).
	EventLoop loop;
	Recorder rec;
	std::vector<TimerHandle> handles;
	std::vector<std::pair<uint64_t, std::string>> expected;
	for (size_t i = 0; i < EventLoop::kTimerCapacity; ++i) {
		uint64_t deadline = (i * 7) % 11 * 10;
		std::string name = std::to_string(i);
		handles.push_back(loop.schedule_at(deadline, rec.make(name)));
		if (i % 3 != 0) {
			expected.emplace_back(deadline, name);
		}
	}
	for (size_t i = 0; i < handles.size(); i += 3) {
		CHECK(loop.cancel(handles[i]));
	}
	std::stable_sort(expected.begin(), expected.end(),
		[](const auto& a, const auto& b) { return a.first < b.first; });
	Names expected_names;
	for (const auto& entry : expected) {
		expected_names.push_back(entry.second);
	}
	loop.run_once(1000);
	CHECK(rec.order == expected_names);
}

TEST_CASE("EventLoop.OnlyDueTimersFire") {
	EventLoop loop;
	Recorder rec;
	loop.schedule_at(100, rec.make("a"));
	loop.schedule_at(101, rec.make("b"));

	loop.run_once(99);
	CHECK(rec.order.empty());
	loop.run_once(100);
	CHECK(rec.order == Names{"a"});
	loop.run_once(101);
	CHECK(rec.order == Names{"a", "b"});
}

TEST_CASE("EventLoop.CancelPreventsFiring") {
	EventLoop loop;
	Recorder rec;
	TimerHandle a = loop.schedule_at(100, rec.make("a"));
	loop.schedule_at(200, rec.make("b"));

	CHECK(loop.cancel(a));
	CHECK_FALSE(loop.cancel(a)); // Already cancelled.
	CHECK_FALSE(loop.cancel(TimerHandle()));
	CHECK(loop.timer_count() == 1);
	loop.run_once(1000);
	CHECK(rec.order == Names{"b"});
}

TEST_CASE("EventLoop.CancelOfFiredOneShotReturnsFalse") {
	EventLoop loop;
	Recorder rec;
	TimerHandle a = loop.schedule_at(100, rec.make("a"));
	loop.run_once(100);
	CHECK_FALSE(loop.cancel(a));
}

TEST_CASE("EventLoop.StaleHandleDoesNotCancelReusedSlot") {
	EventLoop loop;
	Recorder rec;
	TimerHandle old = loop.schedule_at(100, rec.make("old"));
	CHECK(loop.cancel(old));
	TimerHandle fresh = loop.schedule_at(200, rec.make("fresh"));
	CHECK(fresh != old);
	CHECK_FALSE(loop.cancel(old));
	loop.run_once(200);
	CHECK(rec.order == Names{"fresh"});
}

TEST_CASE("EventLoop.CancelDueTimerFromEarlierHandler") {
	// Both timers are due in the same run_once; the first cancels the second, which must not run.
	EventLoop loop;
	Recorder rec;
	TimerHandle second;
	loop.schedule_at(100, rec.make("first", [&]() { CHECK(loop.cancel(second)); }));
	second = loop.schedule_at(100, rec.make("second"));

	loop.run_once(100);
	CHECK(rec.order == Names{"first"});
	CHECK(loop.timer_count() == 0);
}

TEST_CASE("EventLoop.CancelFromPostedCallbackStopsDueTimer") {
	EventLoop loop;
	Recorder rec;
	TimerHandle timer = loop.schedule_at(100, rec.make("timer"));
	loop.post(rec.make("event", [&]() { CHECK(loop.cancel(timer)); }));

	loop.run_once(100);
	CHECK(rec.order == Names{"event"});
}

TEST_CASE("EventLoop.PeriodicTimerRepeatsWithoutDrift") {
	EventLoop loop;
	Recorder rec;
	loop.schedule_periodic(100, 50, rec.make("tick"));

	loop.run_once(100);
	CHECK(loop.next_deadline() == 150);
	loop.run_once(160); // Late by 10 ms: the next deadline stays on the grid.
	CHECK(loop.next_deadline() == 200);
	loop.run_once(200);
	CHECK(rec.order.size() == 3);
	CHECK(loop.timer_count() == 1);
}

TEST_CASE("EventLoop.PeriodicTimerFiresOncePerRunAndSkipsMissedPeriods") {
	ScopedCapture capture;
	EventLoop loop;
	Recorder rec;
	loop.schedule_periodic(100, 50, rec.make("tick"));

	loop.run_once(370); // Deadlines 100..350 were due: fires once, skips 5 periods.
	CHECK(rec.order.size() == 1);
	CHECK(loop.next_deadline() == 400);
	CHECK(capture.sink.contains("missed 5 period(s), next at 400 ms"));
	CHECK(loop.missed_periods() == 5);
}

TEST_CASE("EventLoop.CountsMissedPeriodsAndTheLongestTurn") {
	// A handler that overruns its period (issue #42): the loop counts the skipped periods and keeps
	// the longest turn, measured on its clock, with the time the turn began.
	ScopedCapture capture;
	hc_test::FakeClock clock;
	clock.now_ms = 100;
	EventLoop loop;
	loop.set_clock(&clock);
	CHECK(loop.missed_periods() == 0);
	CHECK(loop.longest_turn_ms() == 0);
	int ticks = 0;
	Action slow{[&]() {
		if (++ticks == 1) {
			clock.advance(350); // the first tick takes 350 ms
		} else {
			clock.advance(5);
		}
	}};
	loop.schedule_periodic(100, 100, slow.callback());

	loop.run_once(clock.now_ms); // at 100; the handler returns at 450
	CHECK(ticks == 1);
	CHECK(loop.longest_turn_ms() == 350);
	CHECK(loop.longest_turn_at_ms() == 100);
	CHECK(loop.missed_periods() == 0); // not yet noticed: the timer re-armed for 200
	CHECK(loop.next_deadline() == 200);

	loop.run_once(clock.now_ms); // at 450: 200 fires, 300 and 400 were missed
	CHECK(ticks == 2);
	CHECK(loop.missed_periods() == 2);
	CHECK(capture.sink.contains("missed 2 period(s), next at 500 ms"));
	CHECK(loop.next_deadline() == 500);
	CHECK(loop.longest_turn_ms() == 350); // the 5-ms turn does not replace it
	CHECK(loop.longest_turn_at_ms() == 100);

	// Work the platform runs on the loop's thread outside run_once counts as a turn too.
	loop.account_turn(1000, 1400);
	CHECK(loop.longest_turn_ms() == 400);
	CHECK(loop.longest_turn_at_ms() == 1000);
	loop.account_turn(2000, 1990); // a clock that went backwards is not a turn
	CHECK(loop.longest_turn_ms() == 400);
}

TEST_CASE("EventLoop.WithoutAClockTurnsAreNotMeasured") {
	EventLoop loop;
	Action body{[]() {}};
	loop.schedule_at(10, body.callback());
	loop.run_once(5000);
	CHECK(loop.longest_turn_ms() == 0);
	CHECK(loop.longest_turn_at_ms() == 0);
}

TEST_CASE("EventLoop.PeriodicTimerCancelsItselfInHandler") {
	EventLoop loop;
	Recorder rec;
	TimerHandle self;
	int runs = 0;
	self = loop.schedule_periodic(100, 100, rec.make("tick", [&]() {
		if (++runs == 2) {
			CHECK(loop.cancel(self));
		}
	}));

	loop.run_once(100);
	loop.run_once(200);
	loop.run_once(300);
	loop.run_once(400);
	CHECK(runs == 2);
	CHECK(loop.timer_count() == 0);
	CHECK(loop.next_deadline() == EventLoop::kNever);
}

TEST_CASE("EventLoop.PeriodicSelfCancelThenRescheduleReusingSlot") {
	// The handler cancels itself and schedules a new timer, which reuses the freed slot. The loop
	// must not mistake the new timer for the periodic one and reschedule it.
	EventLoop loop;
	Recorder rec;
	TimerHandle self;
	TimerHandle replacement;
	self = loop.schedule_periodic(100, 100, rec.make("periodic", [&]() {
		CHECK(loop.cancel(self));
		replacement = loop.schedule_at(1000, rec.make("replacement"));
	}));

	loop.run_once(100);
	CHECK(loop.timer_count() == 1);
	CHECK(loop.next_deadline() == 1000);
	loop.run_once(1000);
	CHECK(rec.order == Names{"periodic", "replacement"});
	CHECK(loop.timer_count() == 0);
}

TEST_CASE("EventLoop.PeriodWithZeroIsRefused") {
	ScopedCapture capture;
	EventLoop loop;
	Recorder rec;
	CHECK_FALSE(loop.schedule_periodic(100, 0, rec.make("x")).valid());
	CHECK(capture.sink.contains("E hc.loop: refusing periodic timer with period 0"));
	CHECK(loop.timer_count() == 0);
}

TEST_CASE("EventLoop.PostedEventsRunInFifoOrderBeforeTimers") {
	EventLoop loop;
	Recorder rec;
	loop.schedule_at(100, rec.make("timer"));
	loop.post(rec.make("e1"));
	loop.post(rec.make("e2"));
	loop.post(rec.make("e3"));

	loop.run_once(100);
	CHECK(rec.order == Names{"e1", "e2", "e3", "timer"});
	CHECK(loop.event_count() == 0);
}

TEST_CASE("EventLoop.PostFromHandlerRunsInNextRunOnce") {
	EventLoop loop;
	Recorder rec;
	loop.post(rec.make("outer", [&]() { loop.post(rec.make("inner")); }));
	loop.schedule_at(100, rec.make("timer", [&]() { loop.post(rec.make("from-timer")); }));

	loop.run_once(100);
	CHECK(rec.order == Names{"outer", "timer"});
	CHECK(loop.event_count() == 2);
	CHECK(loop.next_deadline() == 0); // Due immediately: the platform must not sleep.

	loop.run_once(100);
	CHECK(rec.order == Names{"outer", "timer", "inner", "from-timer"});
	CHECK(loop.next_deadline() == EventLoop::kNever);
}

TEST_CASE("EventLoop.TimerScheduledDueFromHandlerRunsInNextRunOnce") {
	EventLoop loop;
	Recorder rec;
	loop.schedule_at(200, rec.make("old"));
	loop.schedule_at(100, rec.make("first", [&]() { loop.schedule_at(50, rec.make("new")); }));

	loop.run_once(200);
	CHECK(rec.order == Names{"first", "old"});
	CHECK(loop.next_deadline() == 50);
	loop.run_once(200);
	CHECK(rec.order == Names{"first", "old", "new"});
}

TEST_CASE("EventLoop.SelfReschedulingHandlerTerminatesEachRun") {
	// A handler that re-posts itself forever must not make run_once loop forever.
	EventLoop loop;
	int runs = 0;
	Action again;
	again.body = [&]() {
		++runs;
		loop.post(again.callback());
	};
	loop.post(again.callback());
	loop.run_once(0);
	loop.run_once(0);
	loop.run_once(0);
	CHECK(runs == 3);
}

TEST_CASE("EventLoop.TimerPoolExhaustionRefusesAndLogs") {
	ScopedCapture capture;
	EventLoop loop;
	Recorder rec;
	std::vector<TimerHandle> handles;
	for (size_t i = 0; i < EventLoop::kTimerCapacity; ++i) {
		TimerHandle handle = loop.schedule_at(100 + i, rec.make("t"));
		CHECK(handle.valid());
		handles.push_back(handle);
	}
	CHECK(capture.sink.lines.empty());

	TimerHandle refused = loop.schedule_at(5000, rec.make("refused"));
	CHECK_FALSE(refused.valid());
	CHECK(capture.sink.count("E hc.loop: timer pool exhausted (capacity 32), refusing timer at 5000 ms") == 1);
	CHECK(loop.timer_count() == EventLoop::kTimerCapacity);

	// Freeing one slot makes room again.
	CHECK(loop.cancel(handles[0]));
	CHECK(loop.schedule_at(6000, rec.make("accepted")).valid());
}

TEST_CASE("EventLoop.EventQueueExhaustionRefusesAndLogs") {
	ScopedCapture capture;
	EventLoop loop;
	Recorder rec;
	for (size_t i = 0; i < EventLoop::kEventCapacity; ++i) {
		CHECK(loop.post(rec.make("e" + std::to_string(i))));
	}
	CHECK_FALSE(loop.post(rec.make("refused")));
	CHECK(capture.sink.count("E hc.loop: event queue full (capacity 32), refusing post") == 1);

	loop.run_once(0);
	CHECK(rec.order.size() == EventLoop::kEventCapacity);
	CHECK(rec.order.front() == "e0");
	CHECK(rec.order.back() == "e31");
	CHECK(loop.post(rec.make("accepted")));
}

TEST_CASE("EventLoop.EmptyCallbacksAreRefused") {
	ScopedCapture capture;
	EventLoop loop;
	CHECK_FALSE(loop.schedule_at(100, Callback()).valid());
	CHECK_FALSE(loop.post(Callback()));
	CHECK(capture.sink.contains("refusing timer without callback"));
	CHECK(capture.sink.contains("refusing post without callback"));
}

TEST_CASE("EventLoop.NestedRunOnceIsRefused") {
	ScopedCapture capture;
	EventLoop loop;
	Recorder rec;
	loop.schedule_at(200, rec.make("later"));
	loop.post(rec.make("nest", [&]() { loop.run_once(1000); }));

	loop.run_once(0);
	CHECK(rec.order == Names{"nest"});
	CHECK(capture.sink.contains("E hc.loop: refusing nested run_once from within a handler"));
}

TEST_CASE("EventLoop.NextDeadline") {
	EventLoop loop;
	Recorder rec;
	CHECK(loop.next_deadline() == EventLoop::kNever);

	TimerHandle late = loop.schedule_at(500, rec.make("late"));
	CHECK(loop.next_deadline() == 500);
	TimerHandle early = loop.schedule_at(300, rec.make("early"));
	CHECK(loop.next_deadline() == 300);
	loop.schedule_periodic(400, 1000, rec.make("periodic"));
	CHECK(loop.next_deadline() == 300);

	CHECK(loop.cancel(early));
	CHECK(loop.next_deadline() == 400);

	loop.post(rec.make("event"));
	CHECK(loop.next_deadline() == 0);
	loop.run_once(100); // Runs only the event.
	CHECK(loop.next_deadline() == 400);

	loop.run_once(400); // The periodic timer moves to 1400.
	CHECK(loop.next_deadline() == 500);
	CHECK(loop.cancel(late));
	CHECK(loop.next_deadline() == 1400);
}
