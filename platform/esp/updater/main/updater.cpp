/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The OTA updater (doc/ota.md): the factory app of the asymmetric partition layout. It reads the
// update instruction the application left in NVS, connects to WiFi with the application's stored
// credentials, streams the image into the application partition (ota_0), verifies it and boots it.
// Without an instruction it boots ota_0 if that holds a verified image (a freshly flashed board, a
// rollback). The decisions are the core's decision table (hc::decide_updater_step); this file only
// executes them.

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "sdkconfig.h"

#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_flash_partitions.h"
#include "esp_http_client.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_wifi.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs_flash.h"

#include "hc/log.h"
#include "hc/network_settings.h"
#include "hc/update_check.h"
#include "hc/version.h"
#include "nvs_settings.h"

namespace {

constexpr const char* kTag = "hc.updater";

/// The application's project name in its app descriptor: the updater installs nothing else.
constexpr const char* kAppProjectName = "homecontrol";

/// How long one WiFi connection may take before the attempt counts as failed.
constexpr uint32_t kWifiTimeoutMs = 30000;
/// Network timeout of one HTTP operation.
constexpr int kHttpTimeoutMs = 15000;
/// Download chunk.
constexpr size_t kChunk = 4096;

constexpr EventBits_t kGotIp = BIT0;
constexpr EventBits_t kDisconnected = BIT1;

hc::esp::NvsSettings g_settings;
EventGroupHandle_t g_events = nullptr;
bool g_wifi_started = false;
int g_last_reason = 0;
uint8_t g_buffer[kChunk];
char g_reason[160];

/// Routes the core's log lines (hc::Log) to the IDF log.
class EspLogSink : public hc::LogSink {
public:
	void write(hc::LogLevel level, const char* tag, const char* message) override {
		switch (level) {
		case hc::LogLevel::error:
			ESP_LOGE(tag, "%s", message);
			break;
		case hc::LogLevel::warn:
			ESP_LOGW(tag, "%s", message);
			break;
		case hc::LogLevel::info:
			ESP_LOGI(tag, "%s", message);
			break;
		case hc::LogLevel::debug:
			ESP_LOGD(tag, "%s", message);
			break;
		}
	}
};

EspLogSink g_sink;

/// Formats a failure reason into g_reason and returns it.
const char* reason(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
const char* reason(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(g_reason, sizeof(g_reason), fmt, args);
	va_end(args);
	return g_reason;
}

void delay_ms(uint32_t ms) {
	vTaskDelay(pdMS_TO_TICKS(ms));
}

// ---- WiFi: plain station, the application's credentials ---------------------------------------

void on_wifi_event(void*, esp_event_base_t base, int32_t id, void* data) {
	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
		esp_wifi_connect();
	} else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
		g_last_reason = static_cast<const wifi_event_sta_disconnected_t*>(data)->reason;
		xEventGroupClearBits(g_events, kGotIp);
		xEventGroupSetBits(g_events, kDisconnected);
	} else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
		const ip_event_got_ip_t* event = static_cast<const ip_event_got_ip_t*>(data);
		ESP_LOGI(kTag, "network up " IPSTR, IP2STR(&event->ip_info.ip));
		xEventGroupSetBits(g_events, kGotIp);
	}
}

/// Starts the station with wifi.ssid / wifi.psk (the same settings, the same rules as the
/// application's driver: no power save, all-channel scan, the transmit power limit).
bool wifi_start(const char*& why) {
	char ssid[hc::setting::kMaxSsidLength + 1];
	char psk[65];
	hc::SettingsStatus status = g_settings.get_string(hc::setting::kWifiSsid, ssid, sizeof(ssid));
	if (status != hc::SettingsStatus::ok) {
		why = reason("no WiFi credentials (wifi.ssid: %s); the application stores them", hc::settings_status_name(status));
		return false;
	}
	status = g_settings.get_string(hc::setting::kWifiPsk, psk, sizeof(psk));
	if (status == hc::SettingsStatus::not_found) {
		psk[0] = '\0';
	} else if (status != hc::SettingsStatus::ok) {
		why = reason("WiFi key unreadable (wifi.psk: %s)", hc::settings_status_name(status));
		return false;
	}

	// The stack is set up once; a failure here is not retried within this boot.
	static bool stack_tried = false;
	static bool stack_ready = false;
	static char hostname[24];
	if (!stack_tried) {
		stack_tried = true;
		g_events = xEventGroupCreate();
		esp_err_t err = esp_netif_init();
		if (err == ESP_OK) {
			err = esp_event_loop_create_default();
		}
		esp_netif_t* netif = err == ESP_OK ? esp_netif_create_default_wifi_sta() : nullptr;
		if (netif != nullptr) {
			uint8_t mac[6] = {};
			esp_read_mac(mac, ESP_MAC_WIFI_STA);
			std::snprintf(hostname, sizeof(hostname), "homecontrol-%02x%02x%02x", mac[3], mac[4], mac[5]);
			esp_netif_set_hostname(netif, hostname);
			wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
			err = esp_wifi_init(&init);
		} else if (err == ESP_OK) {
			err = ESP_FAIL;
		}
		if (err == ESP_OK) {
			err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
		}
		if (err == ESP_OK) {
			err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, nullptr);
		}
		if (err == ESP_OK) {
			err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, nullptr);
		}
		if (err != ESP_OK) {
			ESP_LOGE(kTag, "network stack not started: %s", esp_err_to_name(err));
		}
		stack_ready = err == ESP_OK;
	}
	if (!stack_ready) {
		why = reason("network stack not started (see above); restart the device");
		return false;
	}
	esp_err_t err = ESP_OK;
	wifi_config_t config = {};
	std::strncpy(reinterpret_cast<char*>(config.sta.ssid), ssid, sizeof(config.sta.ssid));
	std::strncpy(reinterpret_cast<char*>(config.sta.password), psk, sizeof(config.sta.password));
	config.sta.threshold.authmode = psk[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
	config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
	std::memset(psk, 0, sizeof(psk));
	err = esp_wifi_set_mode(WIFI_MODE_STA);
	if (err == ESP_OK) {
		err = esp_wifi_set_config(WIFI_IF_STA, &config);
	}
	std::memset(&config, 0, sizeof(config));
	if (err == ESP_OK) {
		err = esp_wifi_start();
	}
	if (err != ESP_OK) {
		why = reason("WiFi not started (%s)", esp_err_to_name(err));
		return false;
	}
	esp_wifi_set_ps(WIFI_PS_NONE);
	esp_wifi_set_max_tx_power(CONFIG_HC_WIFI_MAX_TX_POWER);
	ESP_LOGI(kTag, "WiFi: connecting to \"%s\" as %s (transmit power limit %d/4 dBm)", ssid, hostname,
		CONFIG_HC_WIFI_MAX_TX_POWER);
	g_wifi_started = true;
	return true;
}

/// Waits for an address (reconnecting after each disconnect) for at most kWifiTimeoutMs.
bool wifi_connected(const char*& why) {
	if (!g_wifi_started && !wifi_start(why)) {
		return false;
	}
	TickType_t start = xTaskGetTickCount();
	TickType_t limit = pdMS_TO_TICKS(kWifiTimeoutMs);
	for (;;) {
		TickType_t waited = xTaskGetTickCount() - start;
		if (waited >= limit) {
			break;
		}
		EventBits_t bits = xEventGroupWaitBits(g_events, kGotIp | kDisconnected, pdFALSE, pdFALSE, limit - waited);
		if (bits & kGotIp) {
			return true;
		}
		if (bits & kDisconnected) {
			xEventGroupClearBits(g_events, kDisconnected);
			ESP_LOGW(kTag, "WiFi disconnected (reason %d); reconnecting", g_last_reason);
			delay_ms(1000);
			esp_wifi_connect();
		}
	}
	why = reason("no network within %u s (last WiFi reason %d)", static_cast<unsigned>(kWifiTimeoutMs / 1000),
		g_last_reason);
	return false;
}

// ---- The application partition ----------------------------------------------------------------

hc::AppImageState inspect(const esp_partition_t* app) {
	esp_partition_pos_t position = {app->address, app->size};
	esp_image_metadata_t metadata = {};
	if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &metadata) != ESP_OK) {
		return hc::AppImageState::missing;
	}
	esp_app_desc_t description = {};
	if (esp_ota_get_partition_description(app, &description) == ESP_OK) {
		ESP_LOGI(kTag, "ota_0 holds %s %s", description.project_name, description.version);
	}
	esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
	if (esp_ota_get_state_partition(app, &state) == ESP_OK &&
		(state == ESP_OTA_IMG_ABORTED || state == ESP_OTA_IMG_INVALID)) {
		return hc::AppImageState::aborted;
	}
	return hc::AppImageState::valid;
}

// ---- The download -------------------------------------------------------------------------------

/// Reads exactly `length` bytes unless the stream ends; returns the count or -1 on an error.
int read_fully(esp_http_client_handle_t client, uint8_t* buffer, size_t length) {
	size_t done = 0;
	while (done < length) {
		int n = esp_http_client_read(client, reinterpret_cast<char*>(buffer + done), static_cast<int>(length - done));
		if (n < 0) {
			return -1;
		}
		if (n == 0) {
			break;
		}
		done += static_cast<size_t>(n);
	}
	return static_cast<int>(done);
}

/// Checks the start of the image before anything is erased: an ESP image for this chip whose app
/// descriptor names the application and the version the instruction announced.
bool check_header(const uint8_t* data, size_t length, const hc::UpdateInstruction& instruction, const char*& why) {
	constexpr size_t kDescOffset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
	if (length < kDescOffset + sizeof(esp_app_desc_t)) {
		why = reason("image shorter than its header (%u bytes)", static_cast<unsigned>(length));
		return false;
	}
	esp_image_header_t header;
	std::memcpy(&header, data, sizeof(header));
	if (header.magic != ESP_IMAGE_HEADER_MAGIC) {
		why = reason("not an ESP firmware image (magic 0x%02x; an error page?)", header.magic);
		return false;
	}
	if (header.chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
		why = reason("image is for another chip (chip id %u, this is %u)", static_cast<unsigned>(header.chip_id),
			static_cast<unsigned>(CONFIG_IDF_FIRMWARE_CHIP_ID));
		return false;
	}
	esp_app_desc_t description;
	std::memcpy(&description, data + kDescOffset, sizeof(description));
	if (description.magic_word != ESP_APP_DESC_MAGIC_WORD) {
		why = reason("image has no app descriptor");
		return false;
	}
	description.project_name[sizeof(description.project_name) - 1] = '\0';
	description.version[sizeof(description.version) - 1] = '\0';
	if (std::strcmp(description.project_name, kAppProjectName) != 0) {
		why = reason("image is \"%s\", not the application \"%s\"", description.project_name, kAppProjectName);
		return false;
	}
	hc::Version announced;
	hc::Version found;
	if (!hc::parse_version(instruction.version, announced) || !hc::parse_version(description.version, found) ||
		hc::compare_versions(announced, found) != 0) {
		why = reason("image is version %s, the manifest announced %s", description.version, instruction.version);
		return false;
	}
	ESP_LOGI(kTag, "image: %s %s, built %s %s, IDF %s", description.project_name, description.version,
		description.date, description.time, description.idf_ver);
	return true;
}

/// Downloads the instruction's image into `app`, verifies it and makes it the boot partition.
/// `erased` tells whether the old application was already touched when it fails.
bool download(const hc::UpdateInstruction& instruction, const esp_partition_t* app, bool& erased, const char*& why) {
	erased = false;
	if (static_cast<uint32_t>(instruction.size) > app->size) {
		why = reason("image of %ld bytes does not fit the application partition (%lu bytes)",
			static_cast<long>(instruction.size), static_cast<unsigned long>(app->size));
		return false;
	}
	esp_http_client_config_t config = {};
	config.url = instruction.image_url;
	config.timeout_ms = kHttpTimeoutMs;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
	config.crt_bundle_attach = esp_crt_bundle_attach; // https:// servers are checked against the IDF's CA bundle
#endif
	config.keep_alive_enable = false;
	esp_http_client_handle_t client = esp_http_client_init(&config);
	if (client == nullptr) {
		why = reason("HTTP client not created (URL or memory)");
		return false;
	}
	bool ok = false;
	esp_ota_handle_t ota = 0;
	bool ota_open = false;
	mbedtls_sha256_context sha;
	mbedtls_sha256_init(&sha);
	do {
		esp_err_t err = esp_http_client_open(client, 0);
		if (err != ESP_OK) {
			why = reason("cannot connect: %s", esp_err_to_name(err));
			break;
		}
		int64_t length = esp_http_client_fetch_headers(client);
		int status = esp_http_client_get_status_code(client);
		if (status != 200) {
			why = reason("HTTP status %d", status);
			break;
		}
		if (length > 0 && length != instruction.size) {
			why = reason("server sends %ld bytes, the manifest announced %ld", static_cast<long>(length),
				static_cast<long>(instruction.size));
			break;
		}
		size_t head = instruction.size < static_cast<int32_t>(kChunk) ? static_cast<size_t>(instruction.size) : kChunk;
		int n = read_fully(client, g_buffer, head);
		if (n < 0) {
			why = reason("read error at the start of the image");
			break;
		}
		if (!check_header(g_buffer, static_cast<size_t>(n), instruction, why)) {
			break;
		}

		// From here on the old application is gone.
		err = esp_ota_begin(app, static_cast<size_t>(instruction.size), &ota);
		erased = true;
		if (err != ESP_OK) {
			why = reason("esp_ota_begin: %s", esp_err_to_name(err));
			break;
		}
		ota_open = true;
		mbedtls_sha256_starts(&sha, 0);
		size_t total = 0;
		size_t next_report = 256 * 1024;
		bool failed = false;
		while (n > 0) {
			mbedtls_sha256_update(&sha, g_buffer, static_cast<size_t>(n));
			err = esp_ota_write(ota, g_buffer, static_cast<size_t>(n));
			if (err != ESP_OK) {
				why = reason("esp_ota_write at %u: %s", static_cast<unsigned>(total), esp_err_to_name(err));
				failed = true;
				break;
			}
			total += static_cast<size_t>(n);
			if (total >= next_report) {
				ESP_LOGI(kTag, "%u of %ld bytes", static_cast<unsigned>(total), static_cast<long>(instruction.size));
				next_report += 256 * 1024;
			}
			if (total >= static_cast<size_t>(instruction.size)) {
				break;
			}
			size_t want = static_cast<size_t>(instruction.size) - total;
			n = read_fully(client, g_buffer, want < kChunk ? want : kChunk);
			if (n < 0) {
				why = reason("read error after %u bytes", static_cast<unsigned>(total));
				failed = true;
				break;
			}
		}
		if (failed) {
			break;
		}
		if (total != static_cast<size_t>(instruction.size)) {
			why = reason("connection ended after %u of %ld bytes", static_cast<unsigned>(total),
				static_cast<long>(instruction.size));
			break;
		}
		uint8_t digest[32];
		mbedtls_sha256_finish(&sha, digest);
		char hex[65];
		for (int i = 0; i < 32; ++i) {
			std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
		}
		if (std::strcmp(hex, instruction.sha256) != 0) {
			why = reason("SHA-256 mismatch: got %.16s..., the manifest announced %.16s...", hex, instruction.sha256);
			break;
		}
		ota_open = false;
		err = esp_ota_end(ota); // verifies the image (structure and its appended hash)
		if (err != ESP_OK) {
			why = reason("image verification failed: %s", esp_err_to_name(err));
			break;
		}
		err = esp_ota_set_boot_partition(app);
		if (err != ESP_OK) {
			why = reason("cannot select the new image for boot: %s", esp_err_to_name(err));
			break;
		}
		ok = true;
	} while (false);
	if (ota_open) {
		esp_ota_abort(ota);
	}
	mbedtls_sha256_free(&sha);
	esp_http_client_close(client);
	esp_http_client_cleanup(client);
	return ok;
}

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
		ESP_LOGE(kTag, "NVS unavailable (%s): no instruction and no WiFi credentials can be read", esp_err_to_name(err));
		return;
	}
	g_settings.open();
}

void boot_application(const esp_partition_t* app) {
	esp_err_t err = esp_ota_set_boot_partition(app);
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "cannot boot the application in %s: %s", app->label, esp_err_to_name(err));
		return;
	}
	ESP_LOGI(kTag, "restarting into the application");
	delay_ms(100); // let the log line out
	esp_restart();
}

} // namespace

extern "C" void app_main(void) {
	hc::Log::set_sink(&g_sink);
	ESP_LOGI(kTag, "HomeControl updater %s (%s)", hc::kVersion, hc::build_id());
	init_nvs();

	const esp_partition_t* app = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
	if (app == nullptr) {
		for (;;) {
			ESP_LOGE(kTag, "the partition table has no application partition (ota_0): flash the complete firmware over USB");
			delay_ms(hc::kUpdateIdleRetryMs);
		}
	}

	for (;;) {
		hc::UpdateInstruction instruction;
		const char* error = nullptr;
		hc::InstructionStatus status = hc::load_update_instruction(g_settings, instruction, &error);
		if (status == hc::InstructionStatus::corrupt) {
			ESP_LOGE(kTag, "update instruction unusable (%s): discarded", error);
			hc::clear_update_instruction(g_settings);
		}
		hc::AppImageState image = inspect(app);
		hc::UpdaterStep step = hc::decide_updater_step(status, instruction.attempts, image);
		ESP_LOGI(kTag, "instruction %s%s%s, attempts %ld, ota_0 %s: %s (%s)", hc::instruction_status_name(status),
			status == hc::InstructionStatus::pending ? " for " : "",
			status == hc::InstructionStatus::pending ? instruction.version : "", static_cast<long>(instruction.attempts),
			hc::app_image_state_name(image), hc::updater_action_name(step.action), step.reason);
		if (step.delay_ms > 0) {
			ESP_LOGI(kTag, "next step in %u s", static_cast<unsigned>(step.delay_ms / 1000));
			delay_ms(step.delay_ms);
		}
		switch (step.action) {
		case hc::UpdaterAction::boot_app:
			boot_application(app); // returns only on failure
			ESP_LOGE(kTag, "staying in the updater; trying again in %u s",
				static_cast<unsigned>(hc::kUpdateIdleRetryMs / 1000));
			delay_ms(hc::kUpdateIdleRetryMs);
			break;
		case hc::UpdaterAction::wait:
			break;
		case hc::UpdaterAction::download: {
			int32_t attempt = instruction.attempts + 1;
			hc::SettingsStatus counted = g_settings.set_int(hc::setting::kUpdateAttempts, attempt);
			if (counted != hc::SettingsStatus::ok) {
				ESP_LOGE(kTag, "cannot count the attempt (%s)", hc::settings_status_name(counted));
			}
			ESP_LOGI(kTag, "attempt %ld: downloading %s %s (%ld bytes)", static_cast<long>(attempt), instruction.version,
				instruction.image_url, static_cast<long>(instruction.size));
			const char* why = "";
			bool erased = false;
			if (wifi_connected(why) && download(instruction, app, erased, why)) {
				ESP_LOGI(kTag, "update to %s written and verified; restarting into it", instruction.version);
				delay_ms(100);
				esp_restart();
			}
			ESP_LOGE(kTag, "attempt %ld failed: %s%s", static_cast<long>(attempt), why,
				erased ? " (the application partition was already overwritten)" : " (the application is untouched)");
			break;
		}
		}
	}
}
