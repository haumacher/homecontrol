/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "esp_platform.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

namespace hc {
namespace esp {

uint64_t EspClock::monotonic_ms() const {
	return static_cast<uint64_t>(esp_timer_get_time()) / 1000u;
}

WallTime EspClock::wall_time() const {
	return WallTime{static_cast<int64_t>(time(nullptr)), synchronized_.load(std::memory_order_acquire)};
}

void EspLogSink::write(LogLevel level, const char* tag, const char* message) {
	// hc::Log has already filtered by its level for the tag (the console's `log level` sets both);
	// ESP_LOG_LEVEL rather than ESP_LOGD, which the build compiles out above
	// CONFIG_LOG_MAXIMUM_LEVEL, so a tag raised to debug at run time is printed.
	switch (level) {
	case LogLevel::error:
		ESP_LOG_LEVEL(ESP_LOG_ERROR, tag, "%s", message);
		break;
	case LogLevel::warn:
		ESP_LOG_LEVEL(ESP_LOG_WARN, tag, "%s", message);
		break;
	case LogLevel::info:
		ESP_LOG_LEVEL(ESP_LOG_INFO, tag, "%s", message);
		break;
	case LogLevel::debug:
		ESP_LOG_LEVEL(ESP_LOG_DEBUG, tag, "%s", message);
		break;
	}
}

MemoryInfo EspSystemInfo::memory() const {
	return MemoryInfo{esp_get_free_heap_size(), esp_get_minimum_free_heap_size()};
}

void EspTimeZone::apply(const char* posix_tz) {
	setenv("TZ", posix_tz, 1);
	tzset();
}

bool EspTimeZone::to_local(int64_t utc_seconds, LocalTime& out) const {
	time_t seconds = static_cast<time_t>(utc_seconds);
	tm local;
	if (localtime_r(&seconds, &local) == nullptr) {
		return false;
	}
	out.year = local.tm_year + 1900;
	out.month = static_cast<uint8_t>(local.tm_mon + 1);
	out.day = static_cast<uint8_t>(local.tm_mday);
	out.hour = static_cast<uint8_t>(local.tm_hour);
	out.minute = static_cast<uint8_t>(local.tm_min);
	out.second = static_cast<uint8_t>(local.tm_sec);
	out.dst = local.tm_isdst > 0;
	if (strftime(out.zone, sizeof(out.zone), "%Z", &local) == 0) {
		out.zone[0] = '\0';
	}
	return true;
}

} // namespace esp
} // namespace hc
