/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_STATUS_LED_H
#define HC_STATUS_LED_H

#include <cstdint>

namespace hc {

/// What the status LED shows (doc/network.md has the table for users).
enum class LedStatus : uint8_t {
	pairing,    ///< Fast blink, 100 ms on / 100 ms off: press the router's WPS button.
	connecting, ///< Slow blink, 500 ms on / 500 ms off: no IP address yet, or reconnecting.
	setup,      ///< Short pulse, 100 ms on / 900 ms off: online, but no runtime image loaded yet.
	degraded,   ///< Short dropout, 900 ms on / 100 ms off: configured, but something is failing.
	ready,      ///< Solid on: everything works (dark with the setting led.quiet on).
};

namespace setting {

/// The quiet status LED (doc/network.md): a boolean, default false. On, the state `ready` is dark;
/// every other state keeps its pattern. A device setting, not part of the runtime image. Persisted
/// name: never rename.
constexpr const char* kLedQuiet = "led.quiet";

} // namespace setting

/// "pairing", "connecting", "setup", "degraded", "ready".
const char* led_status_name(LedStatus status);

/// The blink patterns of the status LED, ported from the phoneblock dongle: a pattern is a bit mask
/// over ticks of kTickMs (bit n set: lit during tick n, least significant bit first) and a period
/// in ticks. tick() is called every kTickMs and returns the level for the current tick; a status
/// change restarts the pattern at its first tick, so a new state is visible at once. In quiet mode
/// (the setting led.quiet) the pattern of `ready` is dark; the other states are unchanged.
class StatusLed {
public:
	static constexpr uint32_t kTickMs = 50;

	struct Pattern {
		uint32_t on_mask;
		uint8_t ticks;
	};

	/// The pattern of `status`; with `quiet`, `ready` is dark (every other state is unchanged).
	static Pattern pattern(LedStatus status, bool quiet = false);

	/// Sets what to show; restarts the pattern if it changed.
	void set_status(LedStatus status);

	LedStatus status() const { return status_; }

	/// Switches the quiet mode; the next tick() shows the pattern it selects.
	void set_quiet(bool quiet) { quiet_ = quiet; }

	bool quiet() const { return quiet_; }

	/// The level for the current tick; advances to the next tick.
	bool tick();

private:
	LedStatus status_ = LedStatus::connecting;
	uint8_t tick_ = 0;
	bool quiet_ = false;
};

} // namespace hc

#endif // HC_STATUS_LED_H
