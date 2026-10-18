/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The OTA updater (doc/ota.md): the factory app of the asymmetric partition layout. It reads the
// update instruction the application left in NVS, connects to WiFi with the application's stored
// credentials, streams the image into the application partition (ota_0), verifies it and boots it.
// The image comes from a download (the instruction names its URL) or from the browser (the
// instruction carries an upload token; the updater serves an upload page and PUT /firmware on port
// 80, doc/ota.md "Upload from the browser"). Without an instruction it boots ota_0 if that holds a
// verified image (a freshly flashed board, a rollback). The decisions are the core's decision table
// (hc::decide_updater_step, hc::decide_upload_expired); this file only executes them.

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "sdkconfig.h"

#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_flash_partitions.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs_flash.h"

#include "hc/json.h"
#include "hc/log.h"
#include "hc/network_settings.h"
#include "hc/update_check.h"
#include "hc/version.h"
#include "nvs_settings.h"

namespace {

constexpr const char* kTag = "hc.updater";

/// The application's project name in its app descriptor (`project(homecontrol)` in
/// platform/esp/CMakeLists.txt): the updater installs nothing else.
constexpr const char* kAppProjectName = "homecontrol";

// The core reads the start of an image by these offsets (hc::app_image); they must be the IDF's.
static_assert(hc::app_image::kMagic == ESP_IMAGE_HEADER_MAGIC, "image magic");
static_assert(offsetof(esp_image_header_t, chip_id) == hc::app_image::kChipIdOffset, "chip id offset");
static_assert(sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) == hc::app_image::kDescOffset,
	"app descriptor offset");
static_assert(hc::app_image::kDescMagic == ESP_APP_DESC_MAGIC_WORD, "app descriptor magic");
static_assert(offsetof(esp_app_desc_t, version) == hc::app_image::kVersionOffset, "version offset");
static_assert(offsetof(esp_app_desc_t, project_name) == hc::app_image::kProjectOffset, "project name offset");
static_assert(sizeof(esp_app_desc_t) == hc::app_image::kDescSize, "app descriptor size");

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
char g_ip[16] = {};
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
		std::snprintf(g_ip, sizeof(g_ip), IPSTR, IP2STR(&event->ip_info.ip));
		ESP_LOGI(kTag, "network up %s", g_ip);
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
/// descriptor names the application (hc::check_app_image). `version`, if given, must be the
/// descriptor's version (the download: the version the manifest announced). Fills `info`.
bool check_header(const uint8_t* data, size_t length, const char* version, hc::AppImageInfo& info, const char*& why) {
	if (!hc::check_app_image(data, length, kAppProjectName, static_cast<uint16_t>(CONFIG_IDF_FIRMWARE_CHIP_ID), info,
			g_reason, sizeof(g_reason))) {
		why = g_reason;
		return false;
	}
	if (version != nullptr) {
		hc::Version announced;
		hc::Version found;
		if (!hc::parse_version(version, announced) || !hc::parse_version(info.version, found) ||
			hc::compare_versions(announced, found) != 0) {
			why = reason("image is version %s, the manifest announced %s", info.version, version);
			return false;
		}
	}
	// The build date and time, byte-wise from the buffer (it is not aligned for esp_app_desc_t).
	char date[sizeof(esp_app_desc_t::date) + 1] = {};
	char time[sizeof(esp_app_desc_t::time) + 1] = {};
	std::memcpy(date, data + hc::app_image::kDescOffset + offsetof(esp_app_desc_t, date), sizeof(date) - 1);
	std::memcpy(time, data + hc::app_image::kDescOffset + offsetof(esp_app_desc_t, time), sizeof(time) - 1);
	ESP_LOGI(kTag, "image: %s %s, built %s %s", info.project, info.version, date, time);
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
		hc::AppImageInfo info;
		if (!check_header(g_buffer, static_cast<size_t>(n), instruction.version, info, why)) {
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

// ---- The upload from the browser (doc/ota.md "Upload from the browser") --------------------------
//
// esp_http_server on port 80 with one task: the handlers run one after the other, so a /status
// request during an upload waits until the upload ends. The upload state is written by the server
// task and read by app_main (atomics); the error text only by the server task.

enum class UploadState : uint8_t { awaiting, writing, verifying, done, failed };

const char* upload_state_name(UploadState state) {
	switch (state) {
	case UploadState::awaiting:
		return "awaiting_upload";
	case UploadState::writing:
		return "writing";
	case UploadState::verifying:
		return "verifying";
	case UploadState::done:
		return "done";
	case UploadState::failed:
		return "failed";
	}
	return "?";
}

/// Network timeout of one receive while an image streams in, and how many in a row are tolerated.
constexpr int kUploadRecvTimeoutS = 10;
constexpr int kUploadRecvRetries = 3;

std::atomic<UploadState> g_upload_state{UploadState::awaiting};
std::atomic<uint32_t> g_upload_received{0};
std::atomic<uint32_t> g_upload_expected{0};
/// When the wait for the upload ends (esp_timer microseconds), 0: no limit.
std::atomic<int64_t> g_upload_deadline_us{0};
std::atomic<bool> g_upload_abort{false};
std::atomic<bool> g_upload_restart{false};
char g_upload_error[160] = {};
char g_upload_token[hc::kUploadTokenLength + 1] = {};
char g_upload_version[33] = {};
const esp_partition_t* g_upload_app = nullptr;

/// The page for a browser that arrives at the updater by itself (a reload during the upload, a
/// bookmark): self-contained, English. The application's Settings page drives the normal upload and
/// leaves the token in this origin's sessionStorage (same IP, port 80).
const char kUploadPage[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>HomeControl updater</title>
<style>
body{font-family:system-ui,sans-serif;max-width:34rem;margin:2rem auto;padding:0 1rem;color:#222;background:#fafafa}
h1{font-size:1.3rem}p{line-height:1.4}.muted{color:#666}.error{color:#b00020}.ok{color:#1b5e20}
button{font:inherit;padding:.4rem .9rem;margin:.3rem .3rem .3rem 0}progress{width:100%;height:1.2rem}
#upload{display:none}
</style></head><body>
<h1>HomeControl updater</h1>
<p id="state" class="muted">Reading the state…</p>
<div id="upload">
<p>Choose the firmware file (<code>homecontrol.bin</code> of a release) to install.</p>
<p><input type="file" id="file" accept=".bin,application/octet-stream"> <button id="send">Install</button></p>
<progress id="progress" max="100" value="0" hidden></progress>
</div>
<p id="message" role="status"></p>
<p><button id="abort">Start the application</button></p>
<script>
const $ = (id) => document.getElementById(id);
const key = "hc.upload.token";
const token = sessionStorage.getItem(key);
function say(text, cls) { $("message").textContent = text; $("message").className = cls || ""; }
async function state() {
	try {
		const s = await (await fetch("/status", { cache: "no-store" })).json();
		const left = s.remaining_s === null ? "no time limit (the device has no application to start)"
			: `${s.remaining_s} s left, then the application starts again`;
		$("state").textContent = `Updater ${s.updater}: ${s.state.replace("_", " ")}, ${left}.`;
		if (s.error) say(s.error, "error");
		return s;
	} catch (e) { return null; }
}
async function waitForApplication(prefix) {
	for (let i = 0; i < 120; i++) {
		await new Promise((r) => setTimeout(r, 2000));
		try {
			const r = await fetch("/api/auth/session", { cache: "no-store" });
			if (r.ok) {
				const s = await r.json();
				say(`${prefix} The application ${s.version} (${s.build}) runs. `, "ok");
				const a = document.createElement("a"); a.href = "/"; a.textContent = "Open it";
				$("message").appendChild(a);
				return;
			}
		} catch (e) { /* not yet */ }
	}
	say(`${prefix} The application did not answer at this address; its address may have changed (see the router's list of devices).`, "error");
}
$("abort").onclick = async () => {
	if (!confirm("Leave the updater and start the installed application?")) return;
	try {
		const r = await fetch("/abort", { method: "POST" });
		const body = await r.json();
		if (!r.ok) { say(body.error, "error"); return; }
		say("The application starts…");
		waitForApplication("");
	} catch (e) { say("The updater does not answer.", "error"); }
};
$("send").onclick = () => {
	const file = $("file").files[0];
	if (!file) { say("Choose a file first.", "error"); return; }
	const xhr = new XMLHttpRequest();
	xhr.open("PUT", "/firmware");
	xhr.setRequestHeader("Authorization", `Bearer ${token}`);
	xhr.upload.onprogress = (e) => {
		if (!e.lengthComputable) return;
		const p = Math.round((100 * e.loaded) / e.total);
		$("progress").value = p;
		say(p < 100 ? `Uploading… ${p} %` : "Verifying the image…");
	};
	xhr.onload = () => {
		let body = {};
		try { body = JSON.parse(xhr.responseText); } catch (e) { /* keep {} */ }
		if (xhr.status === 200) {
			sessionStorage.removeItem(key);
			say(`Installed ${body.version}; the device restarts…`);
			waitForApplication(`Installed ${body.version}.`);
		} else if (xhr.status === 401) {
			say("The access key of this upload is not valid. Start the upload again from the application's Settings page.", "error");
		} else {
			say(body.error || `The updater refused the file (HTTP ${xhr.status}).`, "error");
			$("send").disabled = false;
		}
	};
	xhr.onerror = () => { say("The connection broke off. Reload this page and try again.", "error"); $("send").disabled = false; };
	$("send").disabled = true;
	$("progress").hidden = false;
	xhr.send(file);
};
state().then((s) => {
	if (s && s.state === "done") { say("The image is installed; the device restarts…"); waitForApplication("Installed."); return; }
	if (token) { $("upload").style.display = "block"; return; }
	say("This device waits for a firmware file. The upload is started from the Settings page of the application, which leaves its access key in that browser tab; this tab has none. Start the application to return.");
});
setInterval(() => { if ($("send").disabled === false) state(); }, 5000);
</script></body></html>
)HTML";

void send_json(httpd_req_t* request, const char* status, const char* body, size_t length) {
	httpd_resp_set_status(request, status);
	httpd_resp_set_type(request, "application/json");
	httpd_resp_set_hdr(request, "Cache-Control", "no-store");
	httpd_resp_send(request, body, static_cast<ssize_t>(length));
}

/// Refuses with {"error": reason, "code": code} and logs why.
esp_err_t refuse(httpd_req_t* request, const char* status, const char* code, const char* why) {
	ESP_LOGW(kTag, "%s %s refused (%s): %s", request->method == HTTP_PUT ? "PUT" : request->method == HTTP_POST ? "POST" : "GET",
		request->uri, code, why);
	char body[256];
	hc::JsonWriter json(body, sizeof(body));
	json.begin_object().member("error", why).member("code", code).end_object();
	send_json(request, status, body, json.length());
	return ESP_OK;
}

esp_err_t handle_page(httpd_req_t* request) {
	httpd_resp_set_type(request, "text/html; charset=utf-8");
	httpd_resp_set_hdr(request, "Cache-Control", "no-store");
	httpd_resp_send(request, kUploadPage, static_cast<ssize_t>(sizeof(kUploadPage) - 1));
	return ESP_OK;
}

esp_err_t handle_status(httpd_req_t* request) {
	char body[320];
	hc::JsonWriter json(body, sizeof(body));
	json.begin_object()
		.member("updater", esp_app_get_description()->version)
		.member("state", upload_state_name(g_upload_state.load()))
		.member("received", g_upload_received.load())
		.member("expected", g_upload_expected.load());
	json.key("remaining_s");
	int64_t deadline = g_upload_deadline_us.load();
	if (deadline == 0) {
		json.null();
	} else {
		int64_t left = (deadline - esp_timer_get_time()) / 1000000;
		json.number(static_cast<uint32_t>(left > 0 ? left : 0));
	}
	json.member("error", g_upload_error).end_object();
	send_json(request, "200 OK", body, json.length());
	return ESP_OK;
}

esp_err_t handle_abort(httpd_req_t* request) {
	UploadState state = g_upload_state.load();
	if (state == UploadState::writing || state == UploadState::verifying || state == UploadState::done) {
		return refuse(request, "409 Conflict", "updater.busy", "an image is being installed or was installed");
	}
	if (inspect(g_upload_app) != hc::AppImageState::valid) {
		return refuse(request, "409 Conflict", "updater.no_application",
			"ota_0 holds no bootable application (missing or rolled back): upload an image");
	}
	ESP_LOGI(kTag, "upload cancelled from the browser: starting the application");
	static const char kOk[] = "{\"ok\": true}";
	send_json(request, "200 OK", kOk, sizeof(kOk) - 1);
	g_upload_abort.store(true);
	return ESP_OK;
}

/// Receives up to `length` bytes; tolerates kUploadRecvRetries timeouts in a row. -1 on an error or
/// when the client went away.
int receive(httpd_req_t* request, uint8_t* buffer, size_t length) {
	for (int timeouts = 0;;) {
		int n = httpd_req_recv(request, reinterpret_cast<char*>(buffer), length);
		if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < kUploadRecvRetries) {
			continue;
		}
		return n > 0 ? n : -1;
	}
}

/// Ends a failed upload: state, error text, log.
esp_err_t upload_failed(httpd_req_t* request, const char* status, const char* code, bool erased, const char* why) {
	std::snprintf(g_upload_error, sizeof(g_upload_error), "%s%s", why,
		erased ? " (the application partition was overwritten; upload the image again)" : "");
	g_upload_state.store(UploadState::failed);
	return refuse(request, status, code, g_upload_error);
}

esp_err_t handle_firmware(httpd_req_t* request) {
	UploadState state = g_upload_state.load();
	if (state == UploadState::writing || state == UploadState::verifying || state == UploadState::done) {
		return refuse(request, "409 Conflict", "updater.busy",
			state == UploadState::done ? "the image is installed; the device restarts" : "an upload is running");
	}
	char authorization[64] = {};
	size_t header = httpd_req_get_hdr_value_len(request, "Authorization");
	if (header == 0 || header >= sizeof(authorization) ||
		httpd_req_get_hdr_value_str(request, "Authorization", authorization, sizeof(authorization)) != ESP_OK ||
		!hc::upload_authorized(g_upload_token, authorization)) {
		std::memset(authorization, 0, sizeof(authorization));
		return refuse(request, "401 Unauthorized", "updater.unauthorized",
			"missing or wrong upload token: start the upload from the application's Settings page");
	}
	std::memset(authorization, 0, sizeof(authorization));
	size_t total = request->content_len;
	if (total == 0) {
		return refuse(request, "411 Length Required", "updater.length_required", "the image must be sent with a Content-Length");
	}
	if (total > g_upload_app->size) {
		return refuse(request, "413 Content Too Large", "updater.too_large",
			reason("the image has %u bytes, the application partition %lu", static_cast<unsigned>(total),
				static_cast<unsigned long>(g_upload_app->size)));
	}
	g_upload_error[0] = '\0';
	g_upload_expected.store(static_cast<uint32_t>(total));
	g_upload_received.store(0);

	// The start of the image is checked before the flash is touched.
	size_t head = total < kChunk ? total : kChunk;
	size_t have = 0;
	while (have < head) {
		int n = receive(request, g_buffer + have, head - have);
		if (n < 0) {
			return upload_failed(request, "400 Bad Request", "updater.connection", false,
				reason("the connection ended after %u bytes", static_cast<unsigned>(have)));
		}
		have += static_cast<size_t>(n);
	}
	hc::AppImageInfo info;
	const char* why = "";
	if (!check_header(g_buffer, have, nullptr, info, why)) {
		return upload_failed(request, "422 Unprocessable Content", "updater.wrong_image", false, why);
	}
	ESP_LOGI(kTag, "upload: %s %s, %u bytes from the browser", info.project, info.version, static_cast<unsigned>(total));

	g_upload_state.store(UploadState::writing);
	esp_ota_handle_t ota = 0;
	esp_err_t err = esp_ota_begin(g_upload_app, total, &ota); // erases what the image needs
	if (err != ESP_OK) {
		return upload_failed(request, "500 Internal Server Error", "updater.flash", true,
			reason("esp_ota_begin: %s", esp_err_to_name(err)));
	}
	size_t done = 0;
	size_t next_report = 256 * 1024;
	size_t n = have;
	for (;;) {
		err = esp_ota_write(ota, g_buffer, n);
		if (err != ESP_OK) {
			esp_ota_abort(ota);
			return upload_failed(request, "500 Internal Server Error", "updater.flash", true,
				reason("esp_ota_write at %u: %s", static_cast<unsigned>(done), esp_err_to_name(err)));
		}
		done += n;
		g_upload_received.store(static_cast<uint32_t>(done));
		if (done >= next_report) {
			ESP_LOGI(kTag, "%u of %u bytes", static_cast<unsigned>(done), static_cast<unsigned>(total));
			next_report += 256 * 1024;
		}
		if (done >= total) {
			break;
		}
		size_t want = total - done < kChunk ? total - done : kChunk;
		int got = receive(request, g_buffer, want);
		if (got < 0) {
			esp_ota_abort(ota);
			return upload_failed(request, "400 Bad Request", "updater.connection", true,
				reason("the connection ended after %u of %u bytes", static_cast<unsigned>(done), static_cast<unsigned>(total)));
		}
		n = static_cast<size_t>(got);
	}
	g_upload_state.store(UploadState::verifying);
	err = esp_ota_end(ota); // verifies the image (structure and its appended hash)
	if (err != ESP_OK) {
		return upload_failed(request, "422 Unprocessable Content", "updater.verify_failed", true,
			reason("the image does not verify: %s", esp_err_to_name(err)));
	}

	// Installed. The report for the application first, then the instruction goes (with the token):
	// a power loss in between boots the verified image through the decision table.
	std::snprintf(g_upload_version, sizeof(g_upload_version), "%s", info.version);
	hc::SettingsStatus stored = g_settings.set_string(hc::setting::kUpdateUploaded, g_upload_version);
	if (stored != hc::SettingsStatus::ok) {
		ESP_LOGW(kTag, "cannot store %s (%s): the application will not report the upload", hc::setting::kUpdateUploaded,
			hc::settings_status_name(stored));
	}
	hc::clear_update_instruction(g_settings);
	std::memset(g_upload_token, 0, sizeof(g_upload_token));
	err = esp_ota_set_boot_partition(g_upload_app);
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "cannot select the new image for boot (%s); the updater boots it after the restart",
			esp_err_to_name(err));
	}
	g_upload_state.store(UploadState::done);
	ESP_LOGI(kTag, "upload of %s written and verified; restarting into it", g_upload_version);
	char body[96];
	hc::JsonWriter json(body, sizeof(body));
	json.begin_object().member("ok", true).member("version", g_upload_version).end_object();
	send_json(request, "200 OK", body, json.length());
	g_upload_restart.store(true);
	return ESP_OK;
}

bool start_upload_server(const char*& why) {
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.stack_size = 6144;
	config.lru_purge_enable = true;
	config.recv_wait_timeout = kUploadRecvTimeoutS;
	config.send_wait_timeout = kUploadRecvTimeoutS;
	httpd_handle_t server = nullptr;
	esp_err_t err = httpd_start(&server, &config);
	if (err != ESP_OK) {
		why = reason("HTTP server not started: %s", esp_err_to_name(err));
		return false;
	}
	static const httpd_uri_t kRoutes[] = {
		{"/", HTTP_GET, &handle_page, nullptr},
		{"/status", HTTP_GET, &handle_status, nullptr},
		{"/firmware", HTTP_PUT, &handle_firmware, nullptr},
		{"/abort", HTTP_POST, &handle_abort, nullptr},
	};
	for (const httpd_uri_t& route : kRoutes) {
		httpd_register_uri_handler(server, &route);
	}
	return true;
}

/// Serves the upload until an image is installed (restart into it), the browser cancels, or the
/// timeout runs out and hc::decide_upload_expired() says to boot ota_0 (restart into it). Does not
/// return: when booting ota_0 fails it serves on without a limit.
void serve_upload(const esp_partition_t* app, const hc::UpdateInstruction& instruction, uint32_t timeout_ms) {
	g_upload_app = app;
	std::memcpy(g_upload_token, instruction.token, sizeof(g_upload_token));
	g_upload_deadline_us.store(timeout_ms != 0 ? esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000 : 0);
	static bool server_started = false;
	bool announced = false;
	int64_t next_reminder_us = 0;
	const char* why = "";
	if (!g_wifi_started && !wifi_start(why)) {
		ESP_LOGE(kTag, "upload: %s", why);
	}
	for (;;) {
		if (g_upload_restart.load()) {
			delay_ms(1000); // the answer leaves first
			esp_restart();
		}
		if (g_upload_abort.exchange(false)) {
			boot_application(app); // returns only on failure
			ESP_LOGE(kTag, "staying in the updater");
		}
		if (g_wifi_started) {
			EventBits_t bits = xEventGroupGetBits(g_events);
			if (bits & kDisconnected) {
				xEventGroupClearBits(g_events, kDisconnected);
				ESP_LOGW(kTag, "WiFi disconnected (reason %d); reconnecting", g_last_reason);
				announced = false;
				delay_ms(1000);
				esp_wifi_connect();
			} else if ((bits & kGotIp) && !announced) {
				if (!server_started) {
					server_started = start_upload_server(why);
					if (!server_started) {
						ESP_LOGE(kTag, "upload: %s", why);
					}
				}
				announced = server_started;
				next_reminder_us = 0;
			}
		}
		int64_t now = esp_timer_get_time();
		int64_t deadline = g_upload_deadline_us.load();
		UploadState state = g_upload_state.load();
		bool running = state == UploadState::writing || state == UploadState::verifying;
		if (deadline != 0 && now >= deadline) {
			hc::UpdaterStep step = hc::decide_upload_expired(running ? hc::AppImageState::missing : inspect(app), running);
			if (!running) {
				ESP_LOGI(kTag, "%s: %s", hc::updater_action_name(step.action), step.reason);
			}
			if (step.action == hc::UpdaterAction::boot_app) {
				boot_application(app); // returns only on failure
				ESP_LOGE(kTag, "staying in the updater, waiting for an upload without a limit");
				g_upload_deadline_us.store(0);
			} else if (!running) {
				g_upload_deadline_us.store(0);
			}
		}
		if (now >= next_reminder_us && !running) {
			next_reminder_us = now + static_cast<int64_t>(hc::kUploadReminderMs) * 1000;
			deadline = g_upload_deadline_us.load();
			char left[48];
			if (deadline != 0) {
				std::snprintf(left, sizeof(left), "%u s left", static_cast<unsigned>((deadline - now) / 1000000));
			} else {
				std::snprintf(left, sizeof(left), "no time limit");
			}
			if (announced) {
				ESP_LOGI(kTag, "waiting for the firmware upload at http://%s/ (%s, state %s)", g_ip, left,
					upload_state_name(state));
			} else {
				ESP_LOGI(kTag, "waiting for the network to serve the firmware upload (%s)", left);
			}
		}
		delay_ms(500);
	}
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
		case hc::UpdaterAction::serve_upload:
			serve_upload(app, instruction, step.timeout_ms); // does not return
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
