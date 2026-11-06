/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/status_led.h"

namespace hc {

const char* led_status_name(LedStatus status) {
	switch (status) {
	case LedStatus::pairing:
		return "pairing";
	case LedStatus::connecting:
		return "connecting";
	case LedStatus::setup:
		return "setup";
	case LedStatus::degraded:
		return "degraded";
	case LedStatus::ready:
		return "ready";
	}
	return "?";
}

StatusLed::Pattern StatusLed::pattern(LedStatus status, bool quiet) {
	switch (status) {
	case LedStatus::pairing:
		return Pattern{0x3u, 4}; // 100 ms on, 100 ms off
	case LedStatus::connecting:
		return Pattern{0x3FFu, 20}; // 500 ms on, 500 ms off
	case LedStatus::setup:
		return Pattern{0x3u, 20}; // 100 ms on, 900 ms off
	case LedStatus::degraded:
		return Pattern{0x3FFFFu, 20}; // 900 ms on, 100 ms off
	case LedStatus::ready:
		return quiet ? Pattern{0x0u, 1} : Pattern{0x1u, 1}; // dark when quiet, else solid
	}
	return Pattern{0x0u, 1};
}

void StatusLed::set_status(LedStatus status) {
	if (status != status_) {
		status_ = status;
		tick_ = 0;
	}
}

bool StatusLed::tick() {
	Pattern p = pattern(status_, quiet_);
	// A switch of the quiet mode keeps the tick: it only changes the pattern of ready, whose period is
	// one tick, so the tick is in range of either pattern.
	bool on = ((p.on_mask >> tick_) & 1u) != 0;
	tick_ = static_cast<uint8_t>((tick_ + 1) % p.ticks);
	return on;
}

} // namespace hc
