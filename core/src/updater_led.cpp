/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/updater_led.h"

namespace hc {

const char* updater_led_state_name(UpdaterLedState state) {
	switch (state) {
	case UpdaterLedState::off:
		return "off";
	case UpdaterLedState::emergency:
		return "emergency";
	case UpdaterLedState::held:
		return "held";
	case UpdaterLedState::window:
		return "window";
	case UpdaterLedState::connecting:
		return "connecting";
	case UpdaterLedState::upload_ready:
		return "upload_ready";
	case UpdaterLedState::busy:
		return "busy";
	case UpdaterLedState::error:
		return "error";
	}
	return "?";
}

UpdaterLed::Look UpdaterLed::look(UpdaterLedState state) {
	// Dim colours, like the application's cyan (0, 8, 16): a few percent is bright enough.
	constexpr Colour kRed{16, 0, 0};
	constexpr Colour kAmber{16, 6, 0};
	constexpr Colour kBlue{0, 0, 16};
	constexpr Colour kMagenta{12, 0, 12};
	constexpr Colour kCyan{0, 8, 16};
	constexpr Colour kGreen{0, 16, 0};
	switch (state) {
	case UpdaterLedState::off:
		return Look{0x0u, 1, Colour{0, 0, 0}};
	case UpdaterLedState::emergency:
		return Look{0x15u, 20, kRed}; // three 50 ms flashes, then dark to 1 s
	case UpdaterLedState::held:
		return Look{0x1u, 1, kAmber}; // solid
	case UpdaterLedState::window:
		return Look{0xFFFFu, 32, kBlue}; // 800 ms on, 800 ms off
	case UpdaterLedState::connecting:
		return Look{0x3FFu, 20, kMagenta}; // 500 ms on, 500 ms off
	case UpdaterLedState::upload_ready:
		return Look{0x3u, 32, kCyan}; // 100 ms on, 1500 ms off
	case UpdaterLedState::busy:
		return Look{0x7u, 6, kGreen}; // 150 ms on, 150 ms off
	case UpdaterLedState::error:
		return Look{0x1u, 2, kRed}; // 50 ms on, 50 ms off
	}
	return Look{0x0u, 1, Colour{0, 0, 0}};
}

void UpdaterLed::set_state(UpdaterLedState state) {
	if (state == UpdaterLedState::error) {
		flash_error();
		return;
	}
	if (state != state_) {
		state_ = state;
		if (error_left_ == 0) {
			tick_ = 0;
		}
	}
}

void UpdaterLed::flash_error() {
	error_left_ = kErrorTicks;
	tick_ = 0;
}

UpdaterLed::Output UpdaterLed::tick() {
	Look l = look(shown());
	bool on = ((l.on_mask >> tick_) & 1u) != 0;
	if (error_left_ > 0) {
		if (--error_left_ == 0) {
			tick_ = 0; // the underlying state starts over
			return Output{on, l.colour};
		}
	}
	tick_ = static_cast<uint8_t>((tick_ + 1) % l.ticks);
	return Output{on, l.colour};
}

} // namespace hc
