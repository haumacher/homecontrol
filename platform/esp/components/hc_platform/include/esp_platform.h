/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_PLATFORM_H
#define HC_ESP_PLATFORM_H

#include <atomic>

#include "hc/log.h"
#include "hc/platform/clock.h"
#include "hc/platform/system_info.h"
#include "hc/platform/time_zone.h"

namespace hc {
namespace esp {

/// Monotonic time from esp_timer (microseconds since boot), wall time from time(). The wall clock
/// is reported unsynchronized until SNTP delivers the first time (set_synchronized(), called from
/// the SNTP callback in the lwIP task, hence atomic).
class EspClock : public Clock {
public:
	uint64_t monotonic_ms() const override;
	WallTime wall_time() const override;

	/// Marks the wall clock as synchronized. Any task.
	void set_synchronized() { synchronized_.store(true, std::memory_order_release); }

private:
	std::atomic<bool> synchronized_{false};
};

/// Routes core log messages to ESP_LOG* with the core's tag.
class EspLogSink : public LogSink {
public:
	void write(LogLevel level, const char* tag, const char* message) override;
};

/// Free heap and the minimum ever free heap from the IDF heap statistics.
class EspSystemInfo : public SystemInfo {
public:
	MemoryInfo memory() const override;
};

/// newlib's local time: apply() sets TZ and calls tzset().
class EspTimeZone : public TimeZone {
public:
	void apply(const char* posix_tz) override;
	bool to_local(int64_t utc_seconds, LocalTime& out) const override;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_PLATFORM_H
