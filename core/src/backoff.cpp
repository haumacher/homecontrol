/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/backoff.h"

namespace hc {

Backoff::Backoff(uint32_t initial_ms, uint32_t cap_ms)
	: initial_ms_(initial_ms == 0 ? 1 : initial_ms),
	  cap_ms_(cap_ms < initial_ms_ ? initial_ms_ : cap_ms),
	  current_ms_(initial_ms_) {}

uint32_t Backoff::next() {
	uint32_t result = current_ms_;
	if (failures_ < UINT32_MAX) {
		++failures_;
	}
	// Doubling saturates at the cap (and cannot overflow: current <= cap <= UINT32_MAX).
	current_ms_ = current_ms_ > cap_ms_ / 2 ? cap_ms_ : current_ms_ * 2;
	return result;
}

void Backoff::reset() {
	current_ms_ = initial_ms_;
	failures_ = 0;
}

} // namespace hc
