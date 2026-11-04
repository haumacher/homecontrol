/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "esp_load_probe.h"

#include "esp_heap_caps.h"
#include "hc/log.h"
#include "sdkconfig.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.image";

#if defined(CONFIG_HC_IMAGE_HEAP_PHASES)
constexpr LogLevel kLevel = LogLevel::info;
#else
constexpr LogLevel kLevel = LogLevel::debug;
#endif

uint32_t free_now() { return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_DEFAULT)); }
uint32_t minimum() { return static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT)); }

} // namespace

void EspLoadProbe::load_begun() {
	active_ = Log::enabled(kLevel, kTag);
	if (!active_) {
		return;
	}
	at_begin_ = free_now();
	since_boot_ = minimum();
	lowest_ = at_begin_;
	if (heap_caps_monitor_local_minimum_free_size_start() != ESP_OK) {
		// Another monitor runs (none in this firmware): no phase minimum, the lines say so.
		active_ = false;
		Log::write(kLevel, kTag, "swap heap: no phase minimum (the heap's local monitor is busy)");
		return;
	}
	Log::write(kLevel, kTag, "swap heap: begin: free %u B, lowest since boot %u B", static_cast<unsigned>(at_begin_),
		static_cast<unsigned>(since_boot_));
}

void EspLoadProbe::load_phase(const char* name) {
	if (!active_) {
		return;
	}
	// The minimum since the phase began, then the monitor restarts at the free heap of now (the IDF
	// keeps the lowest of all restarts for the minimum since boot it restores at the stop).
	uint32_t low = minimum();
	uint32_t free = free_now();
	heap_caps_monitor_local_minimum_free_size_start();
	lowest_ = low < lowest_ ? low : lowest_;
	since_boot_ = low < since_boot_ ? low : since_boot_;
	Log::write(kLevel, kTag, "swap heap: %-14s free %6u B, lowest in the phase %6u B (%5d B below the start)", name,
		static_cast<unsigned>(free), static_cast<unsigned>(low),
		static_cast<int>(static_cast<int64_t>(at_begin_) - static_cast<int64_t>(low)));
}

void EspLoadProbe::load_ended(bool ok) {
	if (!active_) {
		return;
	}
	active_ = false;
	uint32_t low = minimum();
	lowest_ = low < lowest_ ? low : lowest_;
	since_boot_ = low < since_boot_ ? low : since_boot_;
	heap_caps_monitor_local_minimum_free_size_stop();
	Log::write(kLevel, kTag, "swap heap: end (%s): free %u B, lowest of the load %u B (%d B below the start), lowest "
		"since boot %u B", ok ? "loaded" : "not loaded", static_cast<unsigned>(free_now()),
		static_cast<unsigned>(lowest_), static_cast<int>(static_cast<int64_t>(at_begin_) - static_cast<int64_t>(lowest_)),
		static_cast<unsigned>(since_boot_));
}

} // namespace esp
} // namespace hc
