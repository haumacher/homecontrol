/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_BACKOFF_H
#define HC_BACKOFF_H

#include <cstdint>

namespace hc {

/// Exponential backoff with a cap: next() returns initial, 2 * initial, 4 * initial, ... until the
/// cap, then the cap forever. reset() (after a success) starts over. With the defaults the delays
/// are 1, 2, 4, 8, 16, 32, 60, 60, ... seconds. No jitter: one controller per house does not
/// synchronize with anybody.
class Backoff {
public:
	static constexpr uint32_t kDefaultInitialMs = 1000;
	static constexpr uint32_t kDefaultCapMs = 60000;

	/// `initial_ms` 0 is treated as 1; a cap below the initial delay is raised to it.
	explicit Backoff(uint32_t initial_ms = kDefaultInitialMs, uint32_t cap_ms = kDefaultCapMs);

	/// The delay before the next attempt; advances the sequence and counts a failure.
	uint32_t next();

	/// The delay next() would return, without advancing.
	uint32_t peek() const { return current_ms_; }

	/// Back to the initial delay, failure count 0.
	void reset();

	/// Number of next() calls since the last reset().
	uint32_t failures() const { return failures_; }

	uint32_t initial_ms() const { return initial_ms_; }
	uint32_t cap_ms() const { return cap_ms_; }

private:
	uint32_t initial_ms_;
	uint32_t cap_ms_;
	uint32_t current_ms_;
	uint32_t failures_ = 0;
};

} // namespace hc

#endif // HC_BACKOFF_H
