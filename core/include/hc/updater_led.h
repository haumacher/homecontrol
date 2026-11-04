/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_UPDATER_LED_H
#define HC_UPDATER_LED_H

#include <cstdint>

namespace hc {

/// What the OTA updater shows on the status LED (doc/ota.md "The updater", "The status LED" has the
/// table for users). Every pattern differs by its timing alone from every other one and from the
/// application's (hc::LedStatus), except `connecting`, which is the application's 500/500 on
/// purpose (the same meaning), and `held`, solid like the application's `ready`, but only while a
/// finger is on the button.
enum class UpdaterLedState : uint8_t {
	off,          ///< Dark: before the first decision, while booting the application.
	emergency,    ///< Three 50 ms red flashes per second: no application, no instruction; hold the button.
	held,         ///< Solid amber: the button is down, the 3 s hold is counting.
	window,       ///< Blue, 800 ms on / 800 ms off: the button window is open (upload without the token).
	connecting,   ///< Magenta, 500 ms on / 500 ms off: WiFi has no address yet.
	upload_ready, ///< Cyan, a 100 ms pulse every 1.6 s: serving the upload with the token, waiting.
	busy,         ///< Green, 150 ms on / 150 ms off: ota_0 is being written or verified.
	error,        ///< Red flicker, 50 ms on / 50 ms off: only as the 2 s overlay of UpdaterLed::flash_error().
};

/// "off", "emergency", "held", "window", "connecting", "upload_ready", "busy", "error".
const char* updater_led_state_name(UpdaterLedState state);

/// The updater's LED patterns, ticked like hc::StatusLed: a pattern is a bit mask over ticks of
/// kTickMs (bit n set: lit during tick n, least significant bit first) and a period in ticks (at most
/// 32), shown in one colour (a WS2812; a plain GPIO LED shows the timing only). A state change
/// restarts the pattern at its first tick. flash_error() shows `error` for kErrorTicks over the
/// current state; then the current state (possibly changed meanwhile) restarts at its first tick.
class UpdaterLed {
public:
	static constexpr uint32_t kTickMs = 50;
	/// How long the error overlay lasts: 2 s.
	static constexpr uint32_t kErrorTicks = 2000 / kTickMs;

	struct Colour {
		uint8_t r;
		uint8_t g;
		uint8_t b;
	};

	struct Look {
		uint32_t on_mask;
		uint8_t ticks;
		Colour colour;
	};

	/// What the LED does in one tick.
	struct Output {
		bool on;
		Colour colour;
	};

	static Look look(UpdaterLedState state);

	/// Sets the underlying state; restarts its pattern if it changed (not visible during an error
	/// overlay). `error` starts the overlay instead (flash_error()).
	void set_state(UpdaterLedState state);

	/// Shows `error` for kErrorTicks over the underlying state; again: the overlay starts over.
	void flash_error();

	/// The underlying state.
	UpdaterLedState state() const { return state_; }

	/// What is shown now: `error` during the overlay, the underlying state otherwise.
	UpdaterLedState shown() const { return error_left_ > 0 ? UpdaterLedState::error : state_; }

	/// The output for the current tick; advances to the next tick.
	Output tick();

private:
	UpdaterLedState state_ = UpdaterLedState::off;
	uint8_t tick_ = 0;
	uint32_t error_left_ = 0;
};

} // namespace hc

#endif // HC_UPDATER_LED_H
