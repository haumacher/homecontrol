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

/// The device's facts (esp_system_info.cpp): free heap, the minimum ever free heap, the largest
/// block and the heap's size from the IDF heap statistics; the FreeRTOS tasks with their stack
/// high-water marks and, with CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS, their CPU share; the die
/// temperature (driver temperature_sensor, installed at the first reading, enabled per reading);
/// chip, flash, IDF and OTA facts; the NVS statistics of the `nvs` partition; the console output
/// queue's figures (console_output.h).
class EspSystemInfo : public SystemInfo {
public:
	MemoryInfo memory() const override;
	bool visit_tasks(TaskVisitor& visitor, const char** reason) const override;
	bool temperature(Temperature& out, const char** reason) const override;
	void visit_chip(DiagnosticsVisitor& visitor) const override;
	bool visit_settings_store(DiagnosticsVisitor& visitor, const char** reason) const override;
	bool console_output(ConsoleOutputInfo& out, const char** reason) const override;

private:
	/// The temperature sensor's handle once installed (temperature_sensor_handle_t).
	mutable void* sensor_ = nullptr;
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
