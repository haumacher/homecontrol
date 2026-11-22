/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/hold_button.h"

#include "hc/log.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.button";

} // namespace

HoldButton::HoldButton(EventLoop& loop, const Clock& clock, const ButtonPin& pin, Callback held, uint32_t hold_ms,
	uint32_t debounce_ms)
	: loop_(loop), clock_(clock), pin_(pin), held_(held), hold_ms_(hold_ms), debounce_ms_(debounce_ms) {}

HoldButton::~HoldButton() {
	loop_.cancel(settle_);
	loop_.cancel(hold_);
}

void HoldButton::start() {
	pressed_ = pin_.pressed();
	armed_ = !pressed_;
}

bool HoldButton::edge() {
	if (settle_.valid()) {
		return true; // a bounce: the settle timer reads the level the pin ends with
	}
	first_edge_ms_ = clock_.monotonic_ms();
	settle_ = loop_.schedule_at(first_edge_ms_ + debounce_ms_,
		Callback::member<HoldButton, &HoldButton::settled>(this, "button.settled"));
	if (!settle_.valid()) {
		Log::error(kTag, "button edge ignored: no timer left");
		return false;
	}
	return true;
}

void HoldButton::settled() {
	settle_ = TimerHandle();
	bool pressed = pin_.pressed();
	if (pressed == pressed_) {
		return; // a glitch: back where it was
	}
	pressed_ = pressed;
	if (!pressed) {
		armed_ = true;
		if (hold_.valid()) {
			loop_.cancel(hold_);
			hold_ = TimerHandle();
		}
		return;
	}
	if (!armed_) {
		return;
	}
	hold_ = loop_.schedule_at(first_edge_ms_ + hold_ms_, Callback::member<HoldButton, &HoldButton::hold_due>(this, "button.hold"));
	if (!hold_.valid()) {
		Log::error(kTag, "button press ignored: no timer left");
	}
}

void HoldButton::hold_due() {
	hold_ = TimerHandle();
	held_();
}

} // namespace hc
