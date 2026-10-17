/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_HOLD_BUTTON_H
#define HC_HOLD_BUTTON_H

#include <cstdint>

namespace hc {

/// Recognizes a button held down for a while (the BOOT button that starts WPS). Fed with the raw
/// level and the monotonic time, either by polling or on every edge; sample() returns true exactly
/// once per press, when the debounced press has lasted hold_ms. A level counts only after it has
/// been stable for debounce_ms, so contact bounce and glitches shorter than that are ignored; the
/// hold time is measured from the start of the stable level. The button arms only after it has
/// been seen released once: a button held (or a pin stuck low) since boot never fires.
class HoldButton {
public:
	static constexpr uint32_t kDefaultHoldMs = 3000;
	static constexpr uint32_t kDefaultDebounceMs = 50;

	explicit HoldButton(uint32_t hold_ms = kDefaultHoldMs, uint32_t debounce_ms = kDefaultDebounceMs);

	/// Feeds the raw level at `now_ms`; true once when the hold time is reached.
	bool sample(bool pressed, uint64_t now_ms);

	/// The debounced level.
	bool pressed() const { return stable_known_ && stable_; }

	/// Whether a press can fire (the button was seen released since the start).
	bool armed() const { return armed_; }

	uint32_t hold_ms() const { return hold_ms_; }

private:
	uint32_t hold_ms_;
	uint32_t debounce_ms_;
	uint64_t raw_since_ = 0;
	uint64_t stable_since_ = 0;
	bool raw_ = false;
	bool stable_ = false;
	bool have_sample_ = false;
	bool stable_known_ = false;
	bool armed_ = false;
	bool fired_ = false;
};

} // namespace hc

#endif // HC_HOLD_BUTTON_H
