/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "esp_load_probe.h"

#include <cstring>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hc/log.h"
#if defined(CONFIG_HC_IMAGE_HEAP_BY_TASK)
#include "esp_heap_task_info.h"
#endif

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.image";

#if defined(CONFIG_HC_IMAGE_HEAP_PHASES)
constexpr LogLevel kLevel = LogLevel::info;
#else
constexpr LogLevel kLevel = LogLevel::debug;
#endif

/// The lines after the load: 3 s, then 7 s later (10 s after it).
constexpr uint64_t kFirstAfterUs = 3000000;
constexpr uint64_t kSecondAfterUs = 7000000;

uint32_t free_now() { return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_DEFAULT)); }
uint32_t minimum() { return static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT)); }
uint32_t largest() { return static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)); }

#if defined(CONFIG_HC_IMAGE_HEAP_BY_TASK)
// Static: nothing is allocated while the probe measures (everything runs on the loop).
heap_task_totals_t g_totals[32];
#if configUSE_TRACE_FACILITY
TaskStatus_t g_tasks[32];
#endif

const char* task_name(void* task) {
#if configUSE_TRACE_FACILITY
	UBaseType_t n = uxTaskGetSystemState(g_tasks, sizeof(g_tasks) / sizeof(g_tasks[0]), nullptr);
	for (UBaseType_t i = 0; i < n; ++i) {
		if (g_tasks[i].xHandle == task) {
			return g_tasks[i].pcTaskName;
		}
	}
	return "(ended task)";
#else
	(void)task;
	return "?";
#endif
}
#endif

} // namespace

EspLoadProbe::EspLoadProbe(LoopQueue& queue) : queue_(queue) {}

EspLoadProbe::~EspLoadProbe() {
	if (timer_ != nullptr) {
		esp_timer_stop(timer_);
		esp_timer_delete(timer_);
	}
}

#if defined(CONFIG_HC_IMAGE_HEAP_BY_TASK)
void EspLoadProbe::snapshot(TaskHeap* out, size_t& count) {
	size_t totals = 0;
	heap_task_info_params_t params = {};
	params.caps[0] = 0; // every block
	params.mask[0] = 0;
	params.totals = g_totals;
	params.num_totals = &totals;
	params.max_totals = sizeof(g_totals) / sizeof(g_totals[0]);
	heap_caps_get_per_task_info(&params);
	count = 0;
	for (size_t i = 0; i < totals && count < kTasks; ++i) {
		out[count++] = TaskHeap{g_totals[i].task, static_cast<uint32_t>(g_totals[i].size[0])};
	}
}
#endif

void EspLoadProbe::log_tasks(const char* when) {
#if defined(CONFIG_HC_IMAGE_HEAP_BY_TASK)
	TaskHeap now[kTasks];
	size_t count = 0;
	snapshot(now, count);
	for (size_t i = 0; i < count; ++i) {
		uint32_t before = 0;
		for (size_t k = 0; k < begin_count_; ++k) {
			if (begin_[k].task == now[i].task) {
				before = begin_[k].bytes;
			}
		}
		int32_t delta = static_cast<int32_t>(now[i].bytes) - static_cast<int32_t>(before);
		if (delta >= 256 || delta <= -256) {
			Log::write(kLevel, kTag, "swap heap: %s: task %-16s holds %6u B (%+6d B since the load began)", when,
				task_name(now[i].task), static_cast<unsigned>(now[i].bytes), static_cast<int>(delta));
		}
	}
#else
	(void)when;
#endif
}

void EspLoadProbe::load_begun() {
	active_ = Log::enabled(kLevel, kTag);
	if (!active_) {
		return;
	}
	if (timer_ != nullptr) {
		esp_timer_stop(timer_); // a previous load's lines after it: this load's follow
		after_step_ = 0;
	}
	at_begin_ = free_now();
	since_boot_ = minimum();
	lowest_ = at_begin_;
#if defined(CONFIG_HC_IMAGE_HEAP_BY_TASK)
	snapshot(begin_, begin_count_);
#endif
	if (heap_caps_monitor_local_minimum_free_size_start() != ESP_OK) {
		// Another monitor runs (none in this firmware): no phase minimum, the lines say so.
		active_ = false;
		Log::write(kLevel, kTag, "swap heap: no phase minimum (the heap's local monitor is busy)");
		return;
	}
	Log::write(kLevel, kTag, "swap heap: begin: free %u B, largest block %u B, lowest since boot %u B",
		static_cast<unsigned>(at_begin_), static_cast<unsigned>(largest()), static_cast<unsigned>(since_boot_));
}

void EspLoadProbe::load_phase(const char* name) {
#if defined(CONFIG_HC_IMAGE_LOAD_YIELD)
	// One tick for the tasks below the loop's priority: while the load holds the loop they cannot take
	// what the network delivers to them, and every packet waiting for them holds a WiFi buffer of the
	// heap (homecontrol #70: about 20 KB in the first 170 ms of a deploy on the C6).
	vTaskDelay(1);
#endif
	if (!active_) {
		return;
	}
	// The minimum since the phase began, then the monitor restarts at the free heap of now (the IDF
	// keeps the lowest of all restarts for the minimum since boot it restores at the stop).
	uint32_t low = minimum();
	uint32_t free = free_now();
	uint32_t block = largest();
	heap_caps_monitor_local_minimum_free_size_start();
	lowest_ = low < lowest_ ? low : lowest_;
	since_boot_ = low < since_boot_ ? low : since_boot_;
	Log::write(kLevel, kTag, "swap heap: %-13s free %6u B, largest %6u B, lowest in the phase %6u B (%5d below the start)",
		name, static_cast<unsigned>(free), static_cast<unsigned>(block), static_cast<unsigned>(low),
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
	Log::write(kLevel, kTag, "swap heap: end (%s): free %u B, largest block %u B, lowest of the load %u B (%d below "
		"the start), lowest since boot %u B", ok ? "loaded" : "not loaded", static_cast<unsigned>(free_now()),
		static_cast<unsigned>(largest()), static_cast<unsigned>(lowest_),
		static_cast<int>(static_cast<int64_t>(at_begin_) - static_cast<int64_t>(lowest_)),
		static_cast<unsigned>(since_boot_));
	log_tasks("end");
	if (timer_ == nullptr) {
		// Created at the first measured load (the probe is a static object, built before the IDF's timer).
		esp_timer_create_args_t args = {};
		args.callback = &EspLoadProbe::after;
		args.arg = this;
		args.dispatch_method = ESP_TIMER_TASK;
		args.name = "hc_load_probe";
		if (esp_timer_create(&args, &timer_) != ESP_OK) {
			timer_ = nullptr;
			Log::write(kLevel, kTag, "swap heap: no lines after the load (no timer)");
		}
	}
	if (timer_ != nullptr && esp_timer_start_once(timer_, kFirstAfterUs) == ESP_OK) {
		after_step_ = 1;
	}
}

void EspLoadProbe::after(void* context) {
	// On the esp_timer task (a small stack): the line is written on the loop.
	EspLoadProbe& self = *static_cast<EspLoadProbe*>(context);
	self.queue_.post(&EspLoadProbe::after_on_loop, &self, 0, nullptr, "heap after an image load");
}

void EspLoadProbe::after_on_loop(void* context, const LoopMessage&) {
	EspLoadProbe& self = *static_cast<EspLoadProbe*>(context);
	int step = self.after_step_;
	if (step == 0) {
		return; // a load began meanwhile
	}
	const char* when = step == 1 ? "3 s after" : "10 s after";
	Log::write(kLevel, kTag, "swap heap: %s the load: free %u B, largest block %u B, lowest since boot %u B", when,
		static_cast<unsigned>(free_now()), static_cast<unsigned>(largest()), static_cast<unsigned>(minimum()));
	self.log_tasks(when);
	if (step == 1 && esp_timer_start_once(self.timer_, kSecondAfterUs) == ESP_OK) {
		self.after_step_ = 2;
	} else {
		self.after_step_ = 0;
	}
}

} // namespace esp
} // namespace hc
