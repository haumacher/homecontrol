/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/hold_button.h"

namespace hc {

HoldButton::HoldButton(uint32_t hold_ms, uint32_t debounce_ms) : hold_ms_(hold_ms), debounce_ms_(debounce_ms) {}

bool HoldButton::sample(bool pressed, uint64_t now_ms) {
	if (!have_sample_ || pressed != raw_) {
		have_sample_ = true;
		raw_ = pressed;
		raw_since_ = now_ms;
	}
	if (now_ms - raw_since_ >= debounce_ms_ && (!stable_known_ || stable_ != raw_)) {
		stable_known_ = true;
		stable_ = raw_;
		stable_since_ = raw_since_;
		if (!stable_) {
			armed_ = true;
			fired_ = false;
		}
	}
	if (armed_ && stable_known_ && stable_ && !fired_ && now_ms - stable_since_ >= hold_ms_) {
		fired_ = true;
		return true;
	}
	return false;
}

} // namespace hc
