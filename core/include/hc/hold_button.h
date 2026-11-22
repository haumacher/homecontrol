/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_HOLD_BUTTON_H
#define HC_HOLD_BUTTON_H

#include <cstdint>

#include "hc/event_loop.h"
#include "hc/platform/clock.h"

namespace hc {

/// The level of a button's pin, read on the event loop's thread (the ESP: gpio_get_level of the
/// BOOT button; the tests: a variable).
class ButtonPin {
public:
	/// Whether the button is pressed now.
	virtual bool pressed() const = 0;

protected:
	~ButtonPin() = default;
};

/// Recognizes a button held down for a while (the BOOT button that starts WPS) from its edges, not
/// by polling (homecontrol #101): the platform calls edge() on the event loop after the pin changed
/// (the ESP's interrupt hands it over). An edge starts a settle timer of debounce_ms; the level the
/// pin has when it fires counts, so contact bounce and glitches shorter than that are ignored (the
/// edges within debounce_ms of the first one only restart nothing). A press that settles starts a
/// one-shot timer of hold_ms, measured from the press's first edge; the release cancels it, its
/// expiry calls `held` once per press. While nobody touches the button no timer runs.
///
/// The button arms only after it has been seen released once: a button held (or a pin stuck low)
/// since boot never fires. start() reads the pin once.
///
/// Every call on the event loop's thread.
class HoldButton {
public:
	static constexpr uint32_t kDefaultHoldMs = 3000;
	static constexpr uint32_t kDefaultDebounceMs = 50;

	HoldButton(EventLoop& loop, const Clock& clock, const ButtonPin& pin, Callback held,
		uint32_t hold_ms = kDefaultHoldMs, uint32_t debounce_ms = kDefaultDebounceMs);
	~HoldButton();
	HoldButton(const HoldButton&) = delete;
	HoldButton& operator=(const HoldButton&) = delete;

	/// Reads the pin: released arms the button, pressed waits for a release.
	void start();

	/// The pin changed (any direction, possibly several times since the last call). Starts the
	/// settle timer unless it runs; refused (false, logged) only when the loop has no timer left.
	bool edge();

	/// The settled level.
	bool pressed() const { return pressed_; }

	/// Whether a press can fire (the button was seen released since the start).
	bool armed() const { return armed_; }

	/// Whether a timer runs (settle or hold): false while the button rests.
	bool timing() const { return settle_.valid() || hold_.valid(); }

	uint32_t hold_ms() const { return hold_ms_; }

private:
	void settled();
	void hold_due();

	EventLoop& loop_;
	const Clock& clock_;
	const ButtonPin& pin_;
	Callback held_;
	uint32_t hold_ms_;
	uint32_t debounce_ms_;
	TimerHandle settle_;
	TimerHandle hold_;
	/// The monotonic time of the first edge of the bounce the settle timer waits out.
	uint64_t first_edge_ms_ = 0;
	bool pressed_ = false;
	bool armed_ = false;
};

} // namespace hc

#endif // HC_HOLD_BUTTON_H
