/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_CLOCK_H
#define HC_PLATFORM_CLOCK_H

#include <cstdint>

namespace hc {

/// Wall-clock time as the platform knows it.
struct WallTime {
	/// Seconds since 1970-01-01T00:00:00Z.
	int64_t seconds;

	/// Whether the time comes from a trusted source (SNTP, RTC, host OS). An unsynchronized
	/// value is whatever the chip counted since boot and must not drive schedules.
	bool synchronized;
};

/// The platform clock. Implemented by hc::posix and hc::esp, and by a fake clock in the tests.
class Clock {
public:
	/// Milliseconds on a monotonic time base (never jumps, never goes backwards; the epoch is
	/// arbitrary, typically boot). All event-loop deadlines are on this base.
	virtual uint64_t monotonic_ms() const = 0;

	/// The current wall-clock time.
	virtual WallTime wall_time() const = 0;

protected:
	~Clock() = default;
};

} // namespace hc

#endif // HC_PLATFORM_CLOCK_H
