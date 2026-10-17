/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "esp_platform.h"

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
	return WallTime{static_cast<int64_t>(time(nullptr)), false};
}

void EspLogSink::write(LogLevel level, const char* tag, const char* message) {
	switch (level) {
	case LogLevel::error:
		ESP_LOGE(tag, "%s", message);
		break;
	case LogLevel::warn:
		ESP_LOGW(tag, "%s", message);
		break;
	case LogLevel::info:
		ESP_LOGI(tag, "%s", message);
		break;
	case LogLevel::debug:
		ESP_LOGD(tag, "%s", message);
		break;
	}
}

MemoryInfo EspSystemInfo::memory() const {
	return MemoryInfo{esp_get_free_heap_size(), esp_get_minimum_free_heap_size()};
}

} // namespace esp
} // namespace hc
