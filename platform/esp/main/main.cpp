/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Boots the controller on the device: NVS, the platform objects, the event-loop task with the
// network bring-up (WiFi with WPS pairing, mDNS, SNTP, the WPS button), the status LED, and the
// USB Serial/JTAG port shared by Improv Wi-Fi provisioning and the serial console.

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "console.h"
#include "esp_platform.h"
#include "esp_status_light.h"
#include "hc/controller.h"
#include "loop_queue.h"
#include "net_services.h"
#include "nvs_settings.h"
#include "serial_mux.h"
#include "wifi_driver.h"
#include "wps_button.h"

namespace {

constexpr const char* kTag = "hc.main";

/// Stack of the event-loop task: log formatting (Log::kMaxMessage plus vsnprintf), console
/// command bodies with printf, NVS writes and the esp_wifi/mDNS/SNTP calls made on the loop.
constexpr uint32_t kLoopTaskStack = 8192;
constexpr UBaseType_t kLoopTaskPriority = 5;

/// Upper bound for one sleep, so the loop re-checks its deadline at least once a second.
constexpr uint64_t kMaxSleepMs = 1000;

hc::esp::EspClock g_clock;
hc::esp::EspLogSink g_sink;
hc::esp::EspSystemInfo g_system;
hc::esp::EspTimeZone g_time_zone;
hc::esp::EspStatusLight g_light;
bool g_light_ready = false;
hc::esp::NvsSettings g_settings;
hc::esp::LoopQueue g_queue;
hc::esp::WifiDriver g_wifi(g_settings, g_queue);
hc::esp::NetServices g_services(g_settings, g_clock);
hc::esp::SerialMux g_serial;

hc::Controller& controller() {
	// Constructed on first use, from app_main, after the IDF is up.
	static hc::Controller instance(hc::Platform{g_clock, g_sink, g_system, g_settings, g_time_zone, g_wifi,
		g_light_ready ? &g_light : nullptr, &g_serial, &g_wifi});
	return instance;
}

void loop_task(void*) {
	hc::Controller& ctl = controller();
	g_wifi.init(ctl.connectivity());
	g_services.init(ctl.connectivity(), g_wifi.hostname());
	bool serial_ready = g_serial.init();
	ctl.start(); // opens the Improv window
	if (serial_ready) {
		g_serial.start_improv(g_queue, ctl.improv(), g_wifi.hostname());
	}

	static hc::esp::WpsButton button(ctl.loop(), g_clock, ctl.connectivity());
	button.start();
	if (serial_ready) {
		static hc::esp::ConsoleContext console{ctl, g_queue, g_wifi, g_services, hc::esp::SerialMux::kConsoleInput};
		hc::esp::console_start(console);
	}

	for (;;) {
		ctl.run_once();
		uint64_t now = g_clock.monotonic_ms();
		uint64_t deadline = ctl.next_deadline();
		TickType_t ticks = 0;
		if (deadline > now) {
			uint64_t wait = deadline - now;
			if (wait > kMaxSleepMs) {
				wait = kMaxSleepMs;
			}
			// Round up: waking before the deadline would only cost a spin.
			ticks = static_cast<TickType_t>((wait + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS);
		}
		// Sleeps until the deadline or until another task posts a message, and runs the messages.
		g_queue.dispatch(ticks);
	}
}

/// Initializes the NVS partition; a full or format-changed partition is erased (logged).
void init_nvs() {
	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_LOGW(kTag, "NVS partition unusable (%s): erasing it, stored settings are lost", esp_err_to_name(err));
		err = nvs_flash_erase();
		if (err == ESP_OK) {
			err = nvs_flash_init();
		}
	}
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "NVS unavailable (%s): settings cannot be read or stored", esp_err_to_name(err));
		return;
	}
	g_settings.open();
}

} // namespace

extern "C" void app_main(void) {
	init_nvs();
	if (!g_queue.init()) {
		ESP_LOGE(kTag, "controller not running: no event-loop queue");
		return;
	}
	g_light_ready = g_light.init();
	controller(); // installs the log sink before anything logs through hc::Log
	if (xTaskCreate(loop_task, "hc_loop", kLoopTaskStack, nullptr, kLoopTaskPriority, nullptr) != pdPASS) {
		ESP_LOGE(kTag, "cannot create the event-loop task (stack %u B): controller not running",
			static_cast<unsigned>(kLoopTaskStack));
	}
}
