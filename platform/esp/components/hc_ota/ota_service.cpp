/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "ota_service.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "sdkconfig.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hc/version.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.ota";

/// The fetch task: TLS handshake and the HTTP client.
constexpr uint32_t kFetchStack = 8192;
constexpr UBaseType_t kFetchPriority = 4;
constexpr int kHttpTimeoutMs = 15000;

const char* ota_state_name(esp_ota_img_states_t state) {
	switch (state) {
	case ESP_OTA_IMG_NEW:
		return "new";
	case ESP_OTA_IMG_PENDING_VERIFY:
		return "pending verification";
	case ESP_OTA_IMG_VALID:
		return "valid";
	case ESP_OTA_IMG_INVALID:
		return "invalid";
	case ESP_OTA_IMG_ABORTED:
		return "aborted";
	case ESP_OTA_IMG_UNDEFINED:
		return "undefined (flashed over USB)";
	}
	return "?";
}

} // namespace

OtaService::OtaService(LoopQueue& queue) : queue_(queue) {}

bool OtaService::init(Controller& controller) {
	controller_ = &controller;
	const esp_partition_t* running = esp_ota_get_running_partition();
	from_ota0_ = running != nullptr && running->type == ESP_PARTITION_TYPE_APP &&
		running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0;
	if (!from_ota0_) {
		ESP_LOGE(kTag, "the application runs from partition \"%s\", not from ota_0: updates are disabled; flash it at "
			"the application partition's offset (doc/ota.md)", running != nullptr ? running->label : "?");
	} else {
		esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
		bool known = esp_ota_get_state_partition(running, &state) == ESP_OK;
		ESP_LOGI(kTag, "application %s in partition \"%s\" at 0x%lx (%lu KB), state %s", kVersion, running->label,
			static_cast<unsigned long>(running->address), static_cast<unsigned long>(running->size / 1024),
			known ? ota_state_name(state) : "unknown");
	}
	EventLoop& loop = controller.loop();
	uint64_t now = controller.clock().monotonic_ms();
	loop.schedule_at(now + kConfirmAfterMs, Callback::member<OtaService, &OtaService::confirm_timeout>(this));
	next_check_ms_ = now + kFirstCheckMs;
	loop.schedule_periodic(next_check_ms_, kCheckPeriodMs, Callback::member<OtaService, &OtaService::periodic_check>(this));

	char url[Settings::kMaxStringLength + 1];
	if (controller.settings().get_string(setting::kUpdateUrl, url, sizeof(url)) != SettingsStatus::ok || url[0] == '\0') {
		ESP_LOGI(kTag, "periodic update check disabled: no update.url (console: update url set <url>)");
	} else {
		ESP_LOGI(kTag, "update source %s, first check in %u min, then daily", url,
			static_cast<unsigned>(kFirstCheckMs / 60000));
	}
	return controller.connectivity().add_listener(this);
}

void OtaService::connectivity_changed(const Connectivity& connectivity, LinkState) {
	if (connectivity.is_up() && !confirmed_) {
		confirm();
	}
}

void OtaService::confirm_timeout() {
	if (!confirmed_) {
		ESP_LOGI(kTag, "running for %u s without a network: confirming the application anyway",
			static_cast<unsigned>(kConfirmAfterMs / 1000));
		confirm();
	}
}

void OtaService::confirm() {
	confirmed_ = true;
	if (!from_ota0_) {
		return;
	}
	const esp_partition_t* running = esp_ota_get_running_partition();
	esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
	if (esp_ota_get_state_partition(running, &state) == ESP_OK && state != ESP_OTA_IMG_VALID) {
		esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
		if (err == ESP_OK) {
			ESP_LOGI(kTag, "application %s confirmed (was %s): no rollback", kVersion, ota_state_name(state));
		} else {
			ESP_LOGE(kTag, "cannot confirm the application: %s; the next reset falls back to the updater",
				esp_err_to_name(err));
		}
	}

	Settings& settings = controller_->settings();
	report_upload(settings);
	UpdateInstruction instruction;
	const char* error = nullptr;
	InstructionStatus status = load_update_instruction(settings, instruction, &error);
	if (status == InstructionStatus::pending) {
		if (update_instruction_fulfilled(kVersion, instruction)) {
			ESP_LOGI(kTag, "update to %s completed (%ld download attempt(s))", instruction.version,
				static_cast<long>(instruction.attempts));
			set_last("update to %s completed", instruction.version);
		} else {
			ESP_LOGW(kTag, "update to %s abandoned after %ld attempt(s): the updater booted %s again; the next check "
				"tries again", instruction.version, static_cast<long>(instruction.attempts), kVersion);
			set_last("update to %s abandoned after %ld attempt(s)", instruction.version,
				static_cast<long>(instruction.attempts));
		}
	} else if (status == InstructionStatus::upload) {
		ESP_LOGW(kTag, "firmware upload abandoned: no image was installed (the updater's 10 minutes ran out or the "
			"upload was cancelled); %s runs on", kVersion);
		set_last("firmware upload abandoned: no image was installed");
	} else if (status == InstructionStatus::corrupt) {
		ESP_LOGW(kTag, "discarding an unusable update instruction (%s)", error);
	} else {
		// An updater that predates uploads erases the upload instruction as unreadable, but it does not
		// know update.token: the token left alone says the upload never started.
		char token[kUploadTokenLength + 1];
		if (settings.get_string(setting::kUpdateToken, token, sizeof(token)) != SettingsStatus::not_found) {
			ESP_LOGW(kTag, "firmware upload did not start: the updater discarded the request (it predates uploads; "
				"flash the updater over USB, doc/ota.md)");
			set_last("firmware upload did not start: the updater predates uploads");
			status = InstructionStatus::corrupt; // erase the token below
		}
		std::memset(token, 0, sizeof(token));
	}
	if (status != InstructionStatus::none) {
		clear_update_instruction(settings);
	}
}

void OtaService::report_upload(Settings& settings) {
	char uploaded[Version::kMaxText + 1];
	SettingsStatus status = settings.get_string(setting::kUpdateUploaded, uploaded, sizeof(uploaded));
	if (status == SettingsStatus::not_found) {
		return;
	}
	Version wanted;
	Version running;
	if (status == SettingsStatus::ok && parse_version(uploaded, wanted) && parse_version(kVersion, running) &&
		compare_versions(wanted, running) == 0) {
		ESP_LOGI(kTag, "update to %s completed (uploaded from the browser)", uploaded);
		set_last("update to %s completed (uploaded)", uploaded);
	} else if (status == SettingsStatus::ok) {
		ESP_LOGW(kTag, "the updater installed the uploaded %s, but %s runs", uploaded, kVersion);
		set_last("uploaded %s, but %s runs", uploaded, kVersion);
	}
	status = settings.erase(setting::kUpdateUploaded);
	if (status != SettingsStatus::ok && status != SettingsStatus::not_found) {
		ESP_LOGE(kTag, "cannot erase %s: %s", setting::kUpdateUploaded, settings_status_name(status));
	}
}

void OtaService::periodic_check() {
	next_check_ms_ = controller_->clock().monotonic_ms() + kCheckPeriodMs;
	char url[Settings::kMaxStringLength + 1];
	if (controller_->settings().get_string(setting::kUpdateUrl, url, sizeof(url)) != SettingsStatus::ok || url[0] == '\0') {
		return; // disabled; said so at start
	}
	const char* error = nullptr;
	if (!check(false, nullptr, &error)) {
		ESP_LOGW(kTag, "periodic update check skipped: %s", error);
		set_last("periodic check skipped: %s", error);
	}
}

bool OtaService::check(bool force, const char* base, const char** error) {
	if (!from_ota0_) {
		*error = "the application does not run from ota_0 (see the log at start)";
		return false;
	}
	if (restart_pending_) {
		*error = "an update is already starting";
		return false;
	}
	if (busy_.load()) {
		*error = "a check is already running";
		return false;
	}
	if (!controller_->connectivity().is_up()) {
		*error = "the network is down";
		return false;
	}
	char stored[Settings::kMaxStringLength + 1];
	if (base == nullptr || base[0] == '\0') {
		SettingsStatus status = controller_->settings().get_string(setting::kUpdateUrl, stored, sizeof(stored));
		if (status != SettingsStatus::ok || stored[0] == '\0') {
			*error = "no update source: update url set <url>";
			return false;
		}
		base = stored;
	}
	if (!update_url_valid(base, error)) {
		return false;
	}
	std::strcpy(base_, base);
	if (!update_manifest_url(base_, manifest_url_, sizeof(manifest_url_))) {
		*error = "manifest URL too long";
		return false;
	}
	force_ = force;
	response_length_ = 0;
	fetch_error_[0] = '\0';
	busy_.store(true);
	ESP_LOGI(kTag, "%s %s", force ? "forced update from" : "checking", manifest_url_);
	if (xTaskCreate(&OtaService::fetch_task, "hc_ota_fetch", kFetchStack, this, kFetchPriority, nullptr) != pdPASS) {
		busy_.store(false);
		*error = "cannot create the fetch task (memory)";
		return false;
	}
	return true;
}

void OtaService::fetch_task(void* context) {
	OtaService* self = static_cast<OtaService*>(context);
	esp_http_client_config_t config = {};
	config.url = self->manifest_url_;
	config.timeout_ms = kHttpTimeoutMs;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
	config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
	esp_http_client_handle_t client = esp_http_client_init(&config);
	if (client == nullptr) {
		std::snprintf(self->fetch_error_, sizeof(self->fetch_error_), "HTTP client not created");
	} else {
		esp_err_t err = esp_http_client_open(client, 0);
		if (err != ESP_OK) {
			std::snprintf(self->fetch_error_, sizeof(self->fetch_error_), "cannot connect: %s", esp_err_to_name(err));
		} else {
			esp_http_client_fetch_headers(client);
			int status = esp_http_client_get_status_code(client);
			if (status != 200) {
				std::snprintf(self->fetch_error_, sizeof(self->fetch_error_), "HTTP status %d", status);
			} else {
				size_t length = 0;
				for (;;) {
					int n = esp_http_client_read(client, self->response_ + length,
						static_cast<int>(sizeof(self->response_) - length));
					if (n < 0) {
						std::snprintf(self->fetch_error_, sizeof(self->fetch_error_), "read error");
						break;
					}
					if (n == 0) {
						break;
					}
					length += static_cast<size_t>(n);
					if (length == sizeof(self->response_)) {
						break; // more than kMaxLength: the parser refuses it
					}
				}
				self->response_length_ = length;
			}
		}
		esp_http_client_close(client);
		esp_http_client_cleanup(client);
	}
	if (!self->queue_.post(&OtaService::handle_fetched, self, 0, nullptr, "update check result")) {
		self->busy_.store(false);
	}
	vTaskDelete(nullptr);
}

void OtaService::handle_fetched(void* context, const LoopMessage&) {
	static_cast<OtaService*>(context)->finish_check();
}

void OtaService::finish_check() {
	last_at_ms_ = controller_->clock().monotonic_ms();
	if (fetch_error_[0] != '\0') {
		ESP_LOGW(kTag, "update check failed: %s: %s", manifest_url_, fetch_error_);
		set_last("check failed: %s", fetch_error_);
		busy_.store(false);
		return;
	}
	static UpdateManifest manifest; // kept off the loop task's stack
	const char* error = nullptr;
	if (!parse_update_manifest(response_, response_length_, manifest, &error)) {
		ESP_LOGW(kTag, "update check: manifest %s refused: %s", manifest_url_, error);
		set_last("manifest refused: %s", error);
		busy_.store(false);
		return;
	}
	static UpdateCheckResult result;
	result = check_update(kVersion, manifest, base_, force_);
	busy_.store(false);
	switch (result.verdict) {
	case UpdateVerdict::up_to_date:
		ESP_LOGI(kTag, "up to date: running %s, the source offers %s (%s)", kVersion, manifest.version, result.reason);
		set_last("up to date (source offers %s)", manifest.version);
		return;
	case UpdateVerdict::refused:
		ESP_LOGW(kTag, "update to %s refused: %s", manifest.version, result.reason);
		set_last("update to %s refused: %s", manifest.version, result.reason);
		return;
	case UpdateVerdict::apply:
		begin_update(manifest, result.image_url);
		return;
	}
}

void OtaService::begin_update(const UpdateManifest& manifest, const char* image_url) {
	const esp_partition_t* app = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
	const esp_partition_t* updater =
		esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
	if (app == nullptr || updater == nullptr) {
		ESP_LOGE(kTag, "update to %s refused: the partition table has no %s partition", manifest.version,
			app == nullptr ? "ota_0" : "updater (factory)");
		set_last("update refused: partition table without updater or ota_0");
		return;
	}
	if (manifest.size > app->size) {
		ESP_LOGE(kTag, "update to %s refused: the image has %lu bytes, the application partition %lu",
			manifest.version, static_cast<unsigned long>(manifest.size), static_cast<unsigned long>(app->size));
		set_last("update to %s refused: image larger than the application partition", manifest.version);
		return;
	}
	Settings& settings = controller_->settings();
	const char* error = nullptr;
	if (!store_update_instruction(settings, manifest, image_url, &error)) {
		ESP_LOGE(kTag, "update to %s refused: %s", manifest.version, error);
		set_last("update to %s refused: %s", manifest.version, error);
		clear_update_instruction(settings);
		return;
	}
	esp_err_t err = esp_ota_set_boot_partition(updater);
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "update to %s refused: cannot boot the updater: %s (flash it over USB, doc/ota.md)",
			manifest.version, esp_err_to_name(err));
		set_last("update refused: updater not bootable (%s)", esp_err_to_name(err));
		clear_update_instruction(settings);
		return;
	}
	restart_pending_ = true;
	ESP_LOGW(kTag, "update %s -> %s (%lu bytes, %s): restarting into the updater in %u s", kVersion, manifest.version,
		static_cast<unsigned long>(manifest.size), image_url, static_cast<unsigned>(kRestartGraceMs / 1000));
	set_last("updating to %s", manifest.version);
	controller_->loop().schedule_at(controller_->clock().monotonic_ms() + kRestartGraceMs,
		Callback::member<OtaService, &OtaService::restart_into_updater>(this));
}

bool OtaService::start_upload(const char* token, const char** error) {
	const char* refusal = nullptr;
	if (!from_ota0_) {
		refusal = "the application does not run from ota_0 (see the log at start)";
	} else if (restart_pending_) {
		refusal = "an update is already starting";
	} else if (busy_.load()) {
		refusal = "an update check is running; try again in a moment";
	}
	if (refusal != nullptr) {
		*error = refusal;
		ESP_LOGW(kTag, "firmware upload refused: %s", refusal);
		return false;
	}
	const esp_partition_t* updater =
		esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
	if (updater == nullptr ||
		esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr) == nullptr) {
		*error = "the partition table has no updater (factory) or ota_0 partition";
		ESP_LOGE(kTag, "firmware upload refused: %s", *error);
		return false;
	}
	static esp_app_desc_t description; // kept off the loop task's stack
	if (esp_ota_get_partition_description(updater, &description) != ESP_OK) {
		*error = "the updater partition holds no valid updater; flash it over USB (doc/ota.md)";
		ESP_LOGE(kTag, "firmware upload refused: %s", *error);
		return false;
	}
	description.version[sizeof(description.version) - 1] = '\0';
	if (!updater_supports_upload(description.version)) {
		*error = "the updater in this device predates firmware uploads; it is updated over USB only (doc/ota.md)";
		ESP_LOGW(kTag, "firmware upload refused: updater %s: %s", description.version, *error);
		return false;
	}
	Settings& settings = controller_->settings();
	if (!store_upload_instruction(settings, token, error)) {
		ESP_LOGE(kTag, "firmware upload refused: %s", *error);
		clear_update_instruction(settings);
		return false;
	}
	esp_err_t err = esp_ota_set_boot_partition(updater);
	if (err != ESP_OK) {
		clear_update_instruction(settings);
		*error = "the updater cannot be booted; flash it over USB (doc/ota.md)";
		ESP_LOGE(kTag, "firmware upload refused: cannot boot the updater: %s", esp_err_to_name(err));
		return false;
	}
	restart_pending_ = true;
	ESP_LOGW(kTag, "firmware upload: instruction stored for updater %s, the updater boots next", description.version);
	set_last("restarting into the updater for a firmware upload");
	return true;
}

void OtaService::restart_into_updater() {
	ESP_LOGW(kTag, "restarting into the updater");
	esp_restart();
}

bool OtaService::set_url(const char* url, const char** error) {
	Settings& settings = controller_->settings();
	if (url == nullptr || url[0] == '\0') {
		SettingsStatus status = settings.erase(setting::kUpdateUrl);
		if (status != SettingsStatus::ok && status != SettingsStatus::not_found) {
			*error = settings_status_name(status);
			return false;
		}
		ESP_LOGI(kTag, "update source cleared: periodic check disabled");
		return true;
	}
	if (!update_url_valid(url, error)) {
		return false;
	}
	SettingsStatus status = settings.set_string(setting::kUpdateUrl, url);
	if (status != SettingsStatus::ok) {
		*error = settings_status_name(status);
		return false;
	}
	ESP_LOGI(kTag, "update source set to %s", url);
	return true;
}

void OtaService::print_status() const {
	const esp_partition_t* running = esp_ota_get_running_partition();
	esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
	bool known = running != nullptr && esp_ota_get_state_partition(running, &state) == ESP_OK;
	std::printf("version   %s (%s)\n", kVersion, build_id());
	std::printf("partition %s, state %s%s\n", running != nullptr ? running->label : "?",
		known ? ota_state_name(state) : "unknown", confirmed_ ? "" : ", not yet confirmed");
	char url[Settings::kMaxStringLength + 1];
	if (controller_->settings().get_string(setting::kUpdateUrl, url, sizeof(url)) == SettingsStatus::ok && url[0] != '\0') {
		std::printf("source    %s\n", url);
		uint64_t now = controller_->clock().monotonic_ms();
		std::printf("next      in %llu min\n",
			static_cast<unsigned long long>(next_check_ms_ > now ? (next_check_ms_ - now) / 60000 : 0));
	} else {
		std::printf("source    none (update url set <url>): periodic check disabled\n");
	}
	if (busy_.load()) {
		std::printf("check     running (%s)\n", manifest_url_);
	}
	if (last_at_ms_ != 0) {
		uint64_t ago = (controller_->clock().monotonic_ms() - last_at_ms_) / 1000;
		std::printf("last      %s (%llu s ago)\n", last_, static_cast<unsigned long long>(ago));
	} else {
		std::printf("last      %s\n", last_);
	}
}

void OtaService::set_last(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(last_, sizeof(last_), fmt, args);
	va_end(args);
	last_at_ms_ = controller_->clock().monotonic_ms();
}

} // namespace esp
} // namespace hc
