/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_PLATFORM_H
#define HC_ESP_PLATFORM_H

#include "hc/log.h"
#include "hc/platform/clock.h"
#include "hc/platform/system_info.h"

namespace hc {
namespace esp {

/// Monotonic time from esp_timer (microseconds since boot), wall time from time(). The wall clock
/// is reported unsynchronized until SNTP is added (a later Phase 0 package).
class EspClock : public Clock {
public:
	uint64_t monotonic_ms() const override;
	WallTime wall_time() const override;
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

} // namespace esp
} // namespace hc

#endif // HC_ESP_PLATFORM_H
