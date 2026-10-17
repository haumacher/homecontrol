/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <cstdint>

#include "doctest.h"
#include "hc/hold_button.h"

using hc::HoldButton;

namespace {

/// Polls `button` every `step_ms` from `from` (inclusive) to `to` (exclusive) with `level`;
/// returns how often it fired.
int poll(HoldButton& button, bool level, uint64_t from, uint64_t to, uint64_t step_ms = 20) {
	int fired = 0;
	for (uint64_t t = from; t < to; t += step_ms) {
		fired += button.sample(level, t) ? 1 : 0;
	}
	return fired;
}

} // namespace

TEST_CASE("HoldButton.ShortPressDoesNothing") {
	HoldButton button;
	CHECK(poll(button, false, 0, 1000) == 0);
	CHECK(button.armed());
	CHECK(poll(button, true, 1000, 3900) == 0); // 2.9 s
	CHECK(poll(button, false, 3900, 10000) == 0);
}

TEST_CASE("HoldButton.FiresOnceAfterTheHoldTime") {
	HoldButton button;
	poll(button, false, 0, 1000);
	CHECK_FALSE(button.sample(true, 1000));
	CHECK_FALSE(button.sample(true, 3999));
	CHECK(button.sample(true, 4000)); // exactly 3 s after the press began
	CHECK(poll(button, true, 4001, 20000) == 0); // keeps holding: no repeat
	CHECK(poll(button, false, 20000, 21000) == 0);
	CHECK(poll(button, true, 21000, 24020) == 1); // a new press fires again
}

TEST_CASE("HoldButton.BounceShorterThanTheDebounceTimeIsIgnored") {
	HoldButton button;
	poll(button, false, 0, 1000);
	// Contact bounce at the press: 5 ms toggles for 30 ms, then held.
	int fired = 0;
	for (uint64_t t = 1000; t < 1030; t += 5) {
		fired += button.sample(((t - 1000) / 5) % 2 == 0, t) ? 1 : 0;
	}
	fired += poll(button, true, 1030, 2500, 5);
	// A 30 ms glitch to released in the middle of the hold does not restart the hold time.
	fired += poll(button, false, 2500, 2530, 5);
	fired += poll(button, true, 2530, 4035, 5);
	CHECK(fired == 1);
	CHECK(button.pressed());
}

TEST_CASE("HoldButton.AReleaseLongerThanTheDebounceTimeRestartsTheHold") {
	HoldButton button;
	poll(button, false, 0, 1000);
	CHECK(poll(button, true, 1000, 3000) == 0);
	CHECK(poll(button, false, 3000, 3100) == 0); // 100 ms released: a real release
	CHECK_FALSE(button.pressed());
	CHECK(poll(button, true, 3100, 6000) == 0);  // 2.9 s of the new press
	CHECK(poll(button, true, 6000, 6200) == 1);
}

TEST_CASE("HoldButton.HeldSinceBootNeverFiresUntilReleased") {
	HoldButton button;
	CHECK(poll(button, true, 0, 10000) == 0);
	CHECK_FALSE(button.armed());
	CHECK(button.pressed());
	CHECK(poll(button, false, 10000, 10100) == 0);
	CHECK(button.armed());
	CHECK(poll(button, true, 10100, 13200) == 1);
}

TEST_CASE("HoldButton.EdgeFedWorksLikePolling") {
	// Fed only on edges plus one check at the end: the hold time is measured from the press edge.
	HoldButton button(3000, 50);
	CHECK_FALSE(button.sample(false, 0));
	CHECK_FALSE(button.sample(false, 100)); // released and stable: armed
	CHECK_FALSE(button.sample(true, 1000));
	CHECK_FALSE(button.sample(true, 1060)); // debounced
	CHECK(button.sample(true, 4000));
}
