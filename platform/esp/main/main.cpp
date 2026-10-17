/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Boots the controller on the device and drives its event loop in a dedicated FreeRTOS task.

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_platform.h"
#include "hc/controller.h"

namespace {

constexpr const char* kTag = "hc.main";

/// Stack of the event-loop task. Log formatting needs Log::kMaxMessage bytes plus vsnprintf.
constexpr uint32_t kLoopTaskStack = 6144;
constexpr UBaseType_t kLoopTaskPriority = 5;

/// Upper bound for one sleep, so the loop re-checks its deadline at least once a second.
constexpr uint64_t kMaxSleepMs = 1000;

hc::esp::EspClock g_clock;
hc::esp::EspLogSink g_sink;
hc::esp::EspSystemInfo g_system;

hc::Controller& controller() {
	// Constructed on first use, from app_main, after the IDF is up.
	static hc::Controller instance(g_clock, g_sink, g_system);
	return instance;
}

void loop_task(void*) {
	hc::Controller& ctl = controller();
	for (;;) {
		ctl.run_once();
		uint64_t now = g_clock.monotonic_ms();
		uint64_t deadline = ctl.next_deadline();
		if (deadline <= now) {
			continue;
		}
		uint64_t wait = deadline - now;
		if (wait > kMaxSleepMs) {
			wait = kMaxSleepMs;
		}
		// Round up: waking before the deadline would only cost a spin.
		TickType_t ticks = static_cast<TickType_t>((wait + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS);
		vTaskDelay(ticks);
	}
}

} // namespace

extern "C" void app_main(void) {
	hc::Controller& ctl = controller();
	ctl.start();
	if (xTaskCreate(loop_task, "hc_loop", kLoopTaskStack, nullptr, kLoopTaskPriority, nullptr) != pdPASS) {
		ESP_LOGE(kTag, "cannot create the event-loop task (stack %u B): controller not running",
			static_cast<unsigned>(kLoopTaskStack));
	}
}
