/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "planned_restart.h"

#include "console_output.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.restart";

Callback g_before;

void restart_now(void*) {
	if (g_before) {
		g_before(); // the factory reset's erase; it restarts itself
	}
	ESP_LOGW(kTag, "restarting");
	console_output().drain(500);
	vTaskDelay(pdMS_TO_TICKS(100)); // let the log line out
	esp_restart();
}

} // namespace

void planned_restart(Controller& controller, const char* reason, Callback before) {
	if (controller.shutdown().running() || controller.shutdown().finished()) {
		ESP_LOGW(kTag, "%s refused: a restart is already under way", reason);
		return;
	}
	g_before = before;
	controller.shutdown().begin(reason, Callback{&restart_now, nullptr, "ota.planned_restart"});
}

} // namespace esp
} // namespace hc
