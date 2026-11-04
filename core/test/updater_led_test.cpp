/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <string>

#include "doctest.h"
#include "hc/status_led.h"
#include "hc/updater_led.h"

using hc::UpdaterLed;
using hc::UpdaterLedState;

namespace {

/// The levels of `ticks` consecutive ticks as a string of '#' (lit) and '.' (dark).
std::string levels(UpdaterLed& led, int ticks) {
	std::string result;
	for (int i = 0; i < ticks; ++i) {
		result += led.tick().on ? '#' : '.';
	}
	return result;
}

/// One period of a pattern as '#' and '.'.
std::string period(UpdaterLedState state) {
	UpdaterLed::Look look = UpdaterLed::look(state);
	std::string result;
	for (int i = 0; i < look.ticks; ++i) {
		result += ((look.on_mask >> i) & 1u) != 0 ? '#' : '.';
	}
	return result;
}

std::string period(hc::LedStatus status) {
	hc::StatusLed::Pattern pattern = hc::StatusLed::pattern(status);
	std::string result;
	for (int i = 0; i < pattern.ticks; ++i) {
		result += ((pattern.on_mask >> i) & 1u) != 0 ? '#' : '.';
	}
	return result;
}

/// A pattern's levels over `ticks` ticks from tick `from` of its period on, repeated as often as needed.
std::string repeated(const std::string& one_period, int ticks, int from = 0) {
	std::string result;
	while (static_cast<int>(result.size()) < from + ticks) {
		result += one_period;
	}
	return result.substr(static_cast<size_t>(from), static_cast<size_t>(ticks));
}

/// Whether two patterns look the same to someone who sees the timing only (no colour), whenever they
/// start watching: one is a shift of the other over 8 s (the common multiple of every period here).
bool same_timing(const std::string& a, const std::string& b) {
	constexpr int kTicks = 160;
	std::string seen = repeated(a, kTicks);
	for (int shift = 0; shift < kTicks; ++shift) {
		if (repeated(b, kTicks, shift) == seen) {
			return true;
		}
	}
	return false;
}

constexpr UpdaterLedState kAll[] = {UpdaterLedState::off, UpdaterLedState::emergency, UpdaterLedState::held,
	UpdaterLedState::window, UpdaterLedState::connecting, UpdaterLedState::upload_ready, UpdaterLedState::busy,
	UpdaterLedState::error};

} // namespace

TEST_CASE("UpdaterLed.EveryPatternOverItsPeriod") {
	UpdaterLed led;
	CHECK(led.state() == UpdaterLedState::off);
	CHECK(levels(led, 20) == "....................");
	led.set_state(UpdaterLedState::emergency);
	CHECK(levels(led, 40) == "#.#.#...............#.#.#...............");
	led.set_state(UpdaterLedState::held);
	CHECK(levels(led, 20) == "####################");
	led.set_state(UpdaterLedState::window);
	CHECK(levels(led, 34) == "################................##");
	led.set_state(UpdaterLedState::connecting);
	CHECK(levels(led, 22) == "##########..........##");
	led.set_state(UpdaterLedState::upload_ready);
	CHECK(levels(led, 34) == "##..............................##");
	led.set_state(UpdaterLedState::busy);
	CHECK(levels(led, 12) == "###...###...");
	CHECK(period(UpdaterLedState::error) == "#.");
}

TEST_CASE("UpdaterLed.Colours") {
	UpdaterLed led;
	led.set_state(UpdaterLedState::emergency);
	UpdaterLed::Output out = led.tick();
	CHECK(out.on);
	CHECK(out.colour.r > 0);
	CHECK(out.colour.g == 0);
	CHECK(out.colour.b == 0);
	led.set_state(UpdaterLedState::window);
	out = led.tick();
	CHECK(out.colour.b > 0);
	CHECK(out.colour.r == 0);
	led.set_state(UpdaterLedState::busy);
	out = led.tick();
	CHECK(out.colour.g > 0);
	CHECK(out.colour.r == 0);
	CHECK(out.colour.b == 0);
	// Dim: a few percent at most, like the application's colour.
	for (UpdaterLedState state : kAll) {
		UpdaterLed::Colour c = UpdaterLed::look(state).colour;
		CHECK(c.r <= 16);
		CHECK(c.g <= 16);
		CHECK(c.b <= 16);
	}
	// The lit states differ in colour from each other, except the two reds (emergency, error).
	for (UpdaterLedState a : kAll) {
		for (UpdaterLedState b : kAll) {
			if (a == b || a == UpdaterLedState::off || b == UpdaterLedState::off) {
				continue;
			}
			bool reds = (a == UpdaterLedState::emergency || a == UpdaterLedState::error) &&
				(b == UpdaterLedState::emergency || b == UpdaterLedState::error);
			UpdaterLed::Colour ca = UpdaterLed::look(a).colour;
			UpdaterLed::Colour cb = UpdaterLed::look(b).colour;
			bool same = ca.r == cb.r && ca.g == cb.g && ca.b == cb.b;
			CHECK_MESSAGE(same == reds, hc::updater_led_state_name(a), " / ", hc::updater_led_state_name(b));
		}
	}
}

TEST_CASE("UpdaterLed.PatternsDifferByTimingAlone") {
	// A plain GPIO LED shows no colour: every pattern looks different from every other updater pattern
	// and from every application pattern, except connecting (the application's 500/500 on purpose) and
	// held (solid like the application's ready, but only while the button is down).
	for (UpdaterLedState a : kAll) {
		for (UpdaterLedState b : kAll) {
			if (a != b) {
				CHECK_MESSAGE(!same_timing(period(a), period(b)), hc::updater_led_state_name(a), " / ",
					hc::updater_led_state_name(b));
			}
		}
		for (hc::LedStatus app : {hc::LedStatus::pairing, hc::LedStatus::connecting, hc::LedStatus::setup,
				 hc::LedStatus::degraded, hc::LedStatus::ready}) {
			bool same = same_timing(period(a), period(app));
			bool expected = (a == UpdaterLedState::connecting && app == hc::LedStatus::connecting) ||
				(a == UpdaterLedState::held && app == hc::LedStatus::ready);
			CHECK_MESSAGE(same == expected, hc::updater_led_state_name(a), " / ", hc::led_status_name(app));
		}
	}
}

TEST_CASE("UpdaterLed.AChangeRestartsThePatternButTheSameStateDoesNot") {
	UpdaterLed led;
	led.set_state(UpdaterLedState::busy);
	CHECK(levels(led, 4) == "###.");
	led.set_state(UpdaterLedState::busy);
	CHECK(levels(led, 4) == "..##"); // continued, wrapped after 6 ticks
	led.set_state(UpdaterLedState::window);
	CHECK(levels(led, 2) == "##");
	led.set_state(UpdaterLedState::emergency);
	CHECK(levels(led, 6) == "#.#.#.");
	led.set_state(UpdaterLedState::connecting);
	CHECK(levels(led, 1) == "#");
}

TEST_CASE("UpdaterLed.TheErrorOverlayLastsTwoSecondsThenTheUnderlyingStateReturns") {
	UpdaterLed led;
	led.set_state(UpdaterLedState::upload_ready);
	CHECK(levels(led, 5) == "##...");
	led.flash_error();
	CHECK(led.shown() == UpdaterLedState::error);
	CHECK(led.state() == UpdaterLedState::upload_ready);
	UpdaterLed::Output out = led.tick();
	CHECK(out.on);
	CHECK(out.colour.r > 0);
	CHECK(out.colour.b == 0);
	CHECK(levels(led, static_cast<int>(UpdaterLed::kErrorTicks) - 1) == repeated("#.", 40).substr(1));
	CHECK(led.shown() == UpdaterLedState::upload_ready);
	// Back to the underlying state, from its first tick.
	CHECK(levels(led, 34) == "##..............................##");

	// A state change during the overlay is not visible until it ends, then starts at its first tick.
	led.flash_error();
	CHECK(levels(led, 10) == "#.#.#.#.#.");
	led.set_state(UpdaterLedState::busy);
	CHECK(led.shown() == UpdaterLedState::error);
	CHECK(levels(led, 30) == repeated("#.", 30));
	CHECK(led.shown() == UpdaterLedState::busy);
	CHECK(levels(led, 6) == "###...");

	// set_state(error) is the overlay; a second error starts it over.
	led.set_state(UpdaterLedState::error);
	CHECK(led.state() == UpdaterLedState::busy);
	CHECK(levels(led, 20) == repeated("#.", 20));
	led.flash_error();
	CHECK(levels(led, 40) == repeated("#.", 40));
	CHECK(levels(led, 3) == "###");
}

TEST_CASE("UpdaterLed.Names") {
	CHECK(std::string(hc::updater_led_state_name(UpdaterLedState::off)) == "off");
	CHECK(std::string(hc::updater_led_state_name(UpdaterLedState::emergency)) == "emergency");
	CHECK(std::string(hc::updater_led_state_name(UpdaterLedState::held)) == "held");
	CHECK(std::string(hc::updater_led_state_name(UpdaterLedState::window)) == "window");
	CHECK(std::string(hc::updater_led_state_name(UpdaterLedState::connecting)) == "connecting");
	CHECK(std::string(hc::updater_led_state_name(UpdaterLedState::upload_ready)) == "upload_ready");
	CHECK(std::string(hc::updater_led_state_name(UpdaterLedState::busy)) == "busy");
	CHECK(std::string(hc::updater_led_state_name(UpdaterLedState::error)) == "error");
}
