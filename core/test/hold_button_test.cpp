/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <cstdint>

#include "doctest.h"
#include "hc/event_loop.h"
#include "hc/hold_button.h"
#include "test_support.h"

using hc::HoldButton;

namespace {

/// A button on the event loop, its pin a variable, fed edge by edge as the ESP's interrupt does.
struct Bench : hc::ButtonPin {
	hc_test::FakeClock clock;
	hc::EventLoop loop;
	bool level = false;
	int fired = 0;
	HoldButton button;

	explicit Bench(bool pressed_at_start = false)
		: button(loop, clock, *this, hc::Callback::member<Bench, &Bench::held>(this, "test.held")) {
		clock.now_ms = 0;
		level = pressed_at_start;
		button.start();
	}

	bool pressed() const override { return level; }
	void held() { ++fired; }

	/// Runs the loop up to `t` in 1 ms steps.
	void run_until(uint64_t t) {
		while (clock.now_ms < t) {
			clock.advance(1);
			loop.run_once(clock.now_ms);
		}
	}

	/// Sets the pin at `t` and reports the edge.
	void set(bool pressed, uint64_t t) {
		run_until(t);
		if (pressed != level) {
			level = pressed;
			CHECK(button.edge());
		}
	}
};

} // namespace

TEST_CASE("HoldButton.NoTimerWhileTheButtonRests") {
	Bench b;
	CHECK(b.button.armed());
	b.run_until(10000);
	CHECK(b.loop.timer_count() == 0);
	CHECK_FALSE(b.button.timing());
	// A press and its release: timers only while it is down and while it settles.
	b.set(true, 10000);
	CHECK(b.loop.timer_count() == 1); // settling
	b.run_until(10100);
	CHECK(b.button.pressed());
	CHECK(b.loop.timer_count() == 1); // the hold
	b.set(false, 11000);
	b.run_until(11100);
	CHECK_FALSE(b.button.pressed());
	CHECK(b.loop.timer_count() == 0);
	CHECK(b.fired == 0);
}

TEST_CASE("HoldButton.ShortPressDoesNothing") {
	Bench b;
	b.set(true, 1000);
	b.set(false, 3900); // 2.9 s
	b.run_until(10000);
	CHECK(b.fired == 0);
	CHECK(b.loop.timer_count() == 0);
}

TEST_CASE("HoldButton.FiresOnceAfterTheHoldTime") {
	Bench b;
	b.set(true, 1000);
	b.run_until(3999);
	CHECK(b.fired == 0);
	b.run_until(4000); // exactly 3 s after the press began
	CHECK(b.fired == 1);
	b.run_until(20000); // keeps holding: no repeat
	CHECK(b.fired == 1);
	CHECK(b.loop.timer_count() == 0);
	b.set(false, 20000);
	b.set(true, 21000);
	b.run_until(24020); // a new press fires again
	CHECK(b.fired == 2);
}

TEST_CASE("HoldButton.BounceWithinTheDebounceTimeIsIgnored") {
	Bench b;
	// Contact bounce at the press: 5 ms toggles for 30 ms, then held.
	for (uint64_t t = 1000; t < 1030; t += 5) {
		b.set(((t - 1000) / 5) % 2 == 0, t);
	}
	b.set(true, 1030);
	b.run_until(2500);
	CHECK(b.button.pressed());
	// A 30 ms glitch to released in the middle of the hold does not restart the hold time.
	b.set(false, 2500);
	b.set(true, 2530);
	b.run_until(4000);
	CHECK(b.fired == 1); // 3 s after the first edge of the press
	CHECK(b.button.pressed());
}

TEST_CASE("HoldButton.AGlitchShorterThanTheDebounceTimeIsNoPress") {
	// The pin dips for 10 ms (a spike) and nothing else comes: the settle timer reads it released.
	Bench b;
	b.set(true, 1000);
	b.set(false, 1010);
	b.run_until(10000);
	CHECK_FALSE(b.button.pressed());
	CHECK(b.fired == 0);
	CHECK(b.loop.timer_count() == 0);
}

TEST_CASE("HoldButton.AReleaseLongerThanTheDebounceTimeRestartsTheHold") {
	Bench b;
	b.set(true, 1000);
	b.set(false, 3000); // 100 ms released: a real release
	b.run_until(3100);
	CHECK_FALSE(b.button.pressed());
	b.set(true, 3100);
	b.run_until(6099); // 2.999 s of the new press
	CHECK(b.fired == 0);
	b.run_until(6100);
	CHECK(b.fired == 1);
}

TEST_CASE("HoldButton.HeldSinceBootNeverFiresUntilReleased") {
	Bench b(true);
	CHECK_FALSE(b.button.armed());
	CHECK(b.button.pressed());
	b.run_until(10000);
	CHECK(b.fired == 0);
	CHECK(b.loop.timer_count() == 0);
	b.set(false, 10000);
	b.run_until(10100);
	CHECK(b.button.armed());
	b.set(true, 10100);
	b.run_until(13200);
	CHECK(b.fired == 1);
}
