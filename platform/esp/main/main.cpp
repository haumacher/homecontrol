/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Boots the controller on the device: NVS, the platform objects, the event-loop task with the
// network bring-up (WiFi with WPS pairing, mDNS, SNTP, the WPS button), the status LED, the USB
// Serial/JTAG port shared by Improv Wi-Fi provisioning and the serial console, and the OTA update
// (the confirmation of a new image, the update check; doc/ota.md), the web server (doc/web.md) and
// the runtime image (doc/image.md).

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "console.h"
#include "esp_platform.h"
#include "esp_status_light.h"
#include "hc/controller.h"
#include "external_service.h"
#include "ehome_service.h"
#include "i18n_service.h"
#include "mqtt_service.h"
#include "zigbee_service.h"
#include "littlefs_blob_store.h"
#include "knx_service.h"
#include "loop_queue.h"
#include "net_services.h"
#include "nvs_settings.h"
#include "ota_service.h"
#include "serial_mux.h"
#include "wifi_driver.h"
#include "wps_button.h"
// ---- web server (issue #5) ----
#include "hc/datapoint.h"
#include "web_server.h"
// ---- end web server ----
// ---- runtime image (issue #8) ----
#include "hc/image/image_service.h"
#include "hc/image/memory_blob_store.h"
#include "image_console.h"
// ---- end runtime image ----
// ---- diagnostics (issue #4) ----
#include "esp_diagnostics.h"
// ---- end diagnostics ----

namespace {

constexpr const char* kTag = "hc.main";

/// Stack of the event-loop task: log formatting (Log::kMaxMessage plus vsnprintf), console
/// command bodies with printf, NVS writes and the esp_wifi/mDNS/SNTP calls made on the loop, and
/// the runtime image's load (the validation of rules and schedules runs on the loop; `image
/// status` prints the reserve left).
constexpr uint32_t kLoopTaskStack = 12288;
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
hc::esp::OtaService g_ota(g_queue);
// ---- web server (issue #5): the datapoint model the technologies fill, and the server ----
hc::DatapointModel g_model(g_clock);
// ---- end web server ----
// ---- runtime image (issue #8) and persistence (issue #21): the blobs (the stored image, persisted
// values, history rings) live on the LittleFS partition under /storage/state ----
hc::esp::LittleFsBlobStore g_blobs;
// ---- end runtime image ----
// ---- diagnostics (issue #4): reset reason, boot record, core dump, task watchdog (doc/diagnostics.md) ----
hc::esp::EspDiagnostics g_diag;
// ---- end diagnostics ----

hc::Controller& controller() {
	// Constructed on first use, from app_main, after the IDF is up.
	static hc::Controller instance(hc::Platform{g_clock, g_sink, g_system, g_settings, g_time_zone, g_wifi,
		g_light_ready ? &g_light : nullptr, &g_serial, &g_wifi, &g_diag});
	return instance;
}

// ---- KNX (issue #7): the tunnel to the KNXnet/IP gateway of the setting knx.gateway, the adapter
// between the bus and the datapoint model, and the telegram log; its console command joins the
// console through the extension table ----
hc::esp::KnxService& knx() {
	static hc::esp::KnxService instance(controller(), g_queue, g_model);
	return instance;
}

int knx_console(void*, int argc, char** argv) {
	return hc::esp::knx_console_command(knx(), argc, argv);
}
// ---- end KNX ----

// ---- runtime image (issue #8): the loader, the rules, the schedules, persistence and history of
// the image, and the console command `image` ----
hc::image::ImageService& image() {
	static hc::image::ImageService instance(controller(), g_model, g_blobs, knx().adapter(), &knx().tunnel());
	return instance;
}

// ---- Zigbee (issue #15): the coordinator, the device database and the cluster datapoints; its
// console command joins the console through the extension table ----
hc::esp::ZigbeeService& zigbee() {
	static hc::esp::ZigbeeService instance(controller(), g_queue, g_model);
	return instance;
}

int zigbee_console(void*, int argc, char** argv) {
	return hc::esp::zigbee_console_command(zigbee(), argc, argv);
}
// ---- end Zigbee ----

// ---- external values (issue #17): weather and other HTTP JSON sources as datapoints ----
hc::esp::ExternalService& external() {
	static hc::esp::ExternalService instance(controller(), g_queue, g_model);
	return instance;
}
// ---- end external values ----

// ---- language packs (issue #24): the release's pack of ui.lang from the update server ----
hc::esp::I18nService& i18n() {
	static hc::esp::I18nService instance(controller(), g_queue, g_blobs);
	return instance;
}
// ---- end language packs ----

// ---- MQTT (issue #18): states, commands, Home Assistant discovery and the history export, to the
// broker the runtime image's mqtt member names (doc/mqtt.md) ----
hc::esp::MqttService& mqtt() {
	static hc::esp::MqttService instance(controller(), g_queue, g_model, &image().history(), g_wifi.hostname());
	return instance;
}
// ---- end MQTT ----

hc::esp::ImageConsole& image_console() {
	static hc::esp::ImageConsole instance(image(), g_queue);
	return instance;
}
// ---- end runtime image ----

void loop_task(void*) {
	hc::Controller& ctl = controller();
	g_queue.set_loop(&ctl.loop()); // the messages' run time counts as loop turns (the System page)
	g_wifi.init(ctl.connectivity());
	g_services.init(ctl.connectivity(), g_wifi.hostname());
	g_ota.init(ctl);
	bool serial_ready = g_serial.init();
	ctl.start(); // opens the Improv window
	// ---- diagnostics ----
	uint64_t started_ms = g_clock.monotonic_ms();
	g_diag.report();
	g_diag.start(ctl.loop(), g_clock);
	// ---- end diagnostics ----
	knx().init();
	zigbee().init(image()); // the adapter becomes a dynamic spec provider of the image service
	// ---- runtime image ----
	g_blobs.start(); // mounts /storage (shared with the web server's UI files) and opens /storage/state
	image().set_ntp_changed(hc::Callback::member<hc::esp::NetServices, &hc::esp::NetServices::restart_sntp>(&g_services));
	image().set_external(&external().values());
	image().set_mqtt(&mqtt().mqtt());
	image().start();
	i18n().start(); // language packs: the boot sync (after the blob store started)
	hc::Log::info(kTag, "runtime image started; event-loop stack reserve %u B",
		static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
	// ---- end runtime image ----
	if (serial_ready) {
		g_serial.start_improv(g_queue, ctl.improv(), g_wifi.hostname());
	}

	static hc::esp::WpsButton button(ctl.loop(), g_clock, ctl.connectivity());
	button.start();
	// ---- web server (issue #5) ----
	static hc::esp::WebServer web(ctl, g_model, g_queue, g_services, g_ota, g_wifi.hostname());
	web.set_image(&image());
	web.set_language_packs(&i18n().packs());
	web.set_mqtt(&mqtt().mqtt());
	web.set_knx(&knx().tunnel(), &knx().telegrams());
	web.set_zigbee(zigbee().adapter(), zigbee().unavailable_reason());
	web.set_external(&external().values());
	web.set_blob_store(&g_blobs);
	web.set_diagnostics(&g_diag);
	web.start();
	// ---- end web server ----
	// ---- E-Home connection (issue #41, doc/ehome.md): outbound to the owner's E-Home, switched on
	// in the Settings ----
	static hc::esp::EhomeService ehome(ctl, g_queue, web);
	ehome.start();
	// ---- end E-Home connection ----
	if (serial_ready) {
		// The commands other components contribute: the web server's, the storage's, the KNX
		// service's, the Zigbee coordinator's, the image's and the external values'.
		static hc::esp::ConsoleCommand extensions[8] = {
			*web.console_commands(),
			*g_blobs.console_commands(),
			{"knx", hc::esp::kKnxCommandHint, hc::esp::kKnxCommandHelp, &knx_console, nullptr},
			{"zigbee", hc::esp::kZigbeeCommandHint, hc::esp::kZigbeeCommandHelp, &zigbee_console, nullptr},
			{"image", hc::esp::kImageCommandHint, hc::esp::kImageCommandHelp, &hc::esp::ImageConsole::run,
				&image_console()},
			*external().console_command(),
			*i18n().console_command(),
			*mqtt().console_command(),
		};
		static hc::esp::ConsoleContext console{ctl, g_queue, g_wifi, g_services, g_ota,
			hc::esp::SerialMux::kConsoleInput, extensions, sizeof(extensions) / sizeof(extensions[0]), &g_diag, &g_blobs};
		hc::esp::console_start(console);
	}

	// ---- diagnostics: the loop under the task watchdog, fed every turn (a turn sleeps at most
	// kMaxSleepMs); the time the start-up above held the loop's timers back ----
	hc::Log::info(kTag, "event loop running %llu ms after the controller started",
		static_cast<unsigned long long>(g_clock.monotonic_ms() - started_ms));
	g_diag.watch_loop();
	// ---- end diagnostics ----
	for (;;) {
		g_diag.feed(); // diagnostics
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
	g_diag.init(); // diagnostics: the reset reason and the boot record, before the controller logs
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
