/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The device's facts for the System page (issue #42): heap, FreeRTOS tasks with their run-time
// statistics, the die temperature, chip and firmware, the NVS statistics, the console output queue.

#include "esp_platform.h"

#include "console_output.h"
#include <cstdio>
#include <memory>
#include <new>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#if SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#endif

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.system";

/// The sensor's measuring range and the accuracy the IDF documents for it (the range table of
/// soc/<target>/temperature_sensor_periph.c: 20..100 C within 2 C). The die runs well above the
/// room's temperature, so the range starts at 20.
constexpr int kTemperatureMin = 20;
constexpr int kTemperatureMax = 100;
constexpr double kTemperatureAccuracy = 2.0;

/// Room for tasks created between counting and listing them.
constexpr UBaseType_t kExtraTasks = 4;

TaskState map_state(eTaskState state) {
	switch (state) {
	case eRunning: return TaskState::running;
	case eReady: return TaskState::ready;
	case eBlocked: return TaskState::blocked;
	case eSuspended: return TaskState::suspended;
	case eDeleted: return TaskState::deleted;
	default: return TaskState::unknown;
	}
}

const char* chip_model(esp_chip_model_t model) {
	switch (model) {
	case CHIP_ESP32: return "ESP32";
	case CHIP_ESP32S2: return "ESP32-S2";
	case CHIP_ESP32S3: return "ESP32-S3";
	case CHIP_ESP32C3: return "ESP32-C3";
	case CHIP_ESP32C2: return "ESP32-C2";
	case CHIP_ESP32C6: return "ESP32-C6";
	case CHIP_ESP32H2: return "ESP32-H2";
	default: return CONFIG_IDF_TARGET;
	}
}

const char* ota_state_name(esp_ota_img_states_t state) {
	switch (state) {
	case ESP_OTA_IMG_NEW: return "new";
	case ESP_OTA_IMG_PENDING_VERIFY: return "pending verify";
	case ESP_OTA_IMG_VALID: return "valid";
	case ESP_OTA_IMG_INVALID: return "invalid";
	case ESP_OTA_IMG_ABORTED: return "aborted";
	default: return "undefined";
	}
}

} // namespace

MemoryInfo EspSystemInfo::memory() const {
	MemoryInfo info{esp_get_free_heap_size(), esp_get_minimum_free_heap_size()};
	info.largest_block = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
	info.total_bytes = static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_DEFAULT));
	return info;
}

bool EspSystemInfo::visit_tasks(TaskVisitor& visitor, const char** reason) const {
#if configUSE_TRACE_FACILITY
	// On the heap for the request only (about 2 KB while the System page polls, not a steady-state path).
	UBaseType_t capacity = uxTaskGetNumberOfTasks() + kExtraTasks;
	std::unique_ptr<TaskStatus_t[]> tasks(new (std::nothrow) TaskStatus_t[capacity]);
	if (!tasks) {
		ESP_LOGW(kTag, "task list refused: no heap for %u task records", static_cast<unsigned>(capacity));
		*reason = "no heap for the task records";
		return false;
	}
	configRUN_TIME_COUNTER_TYPE total = 0;
	UBaseType_t count = uxTaskGetSystemState(tasks.get(), capacity, &total);
	if (count == 0) {
		*reason = "more tasks than expected; try again";
		return false;
	}
	TaskHandle_t self = xTaskGetCurrentTaskHandle();
	for (UBaseType_t i = 0; i < count; ++i) {
		const TaskStatus_t& t = tasks[i];
		TaskInfo info;
		info.name = t.pcTaskName;
		info.state = map_state(t.eCurrentState);
		info.priority = static_cast<int32_t>(t.uxCurrentPriority);
		info.stack_free = static_cast<int64_t>(t.usStackHighWaterMark); // bytes: StackType_t is a byte here
#if configGENERATE_RUN_TIME_STATS
		if (total > 0) {
			// The counter is esp_timer's microseconds since boot per core; a task's share is of all cores.
			uint64_t all = static_cast<uint64_t>(total) * static_cast<uint64_t>(portNUM_PROCESSORS);
			info.cpu_permille = static_cast<int32_t>(static_cast<uint64_t>(t.ulRunTimeCounter) * 1000u / all);
		}
#endif
#if configTASKLIST_INCLUDE_COREID
		if (t.xCoreID >= 0 && t.xCoreID < portNUM_PROCESSORS) {
			info.core = static_cast<int32_t>(t.xCoreID);
		}
#endif
		info.current = t.xHandle == self;
		visitor.task(info);
	}
	return true;
#else
	(void)visitor;
	*reason = "the build lacks CONFIG_FREERTOS_USE_TRACE_FACILITY";
	return false;
#endif
}

void EspSystemInfo::start_temperature_sensor() {
	Temperature reading;
	const char* reason = "";
	if (temperature(reading, &reason)) {
		ESP_LOGI(kTag, "die temperature %.1f C", static_cast<double>(reading.celsius));
	} else {
		ESP_LOGW(kTag, "temperature sensor not started (%s): WiFi may hear nothing until a software restart", reason);
	}
}

bool EspSystemInfo::temperature(Temperature& out, const char** reason) const {
#if SOC_TEMP_SENSOR_SUPPORTED
	if (sensor_ == nullptr) {
		temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(kTemperatureMin, kTemperatureMax);
		temperature_sensor_handle_t handle = nullptr;
		esp_err_t err = temperature_sensor_install(&config, &handle);
		if (err != ESP_OK) {
			ESP_LOGW(kTag, "temperature sensor not installed: %s", esp_err_to_name(err));
			*reason = "the temperature sensor cannot be installed (see the log)";
			return false;
		}
		sensor_ = handle;
	}
	temperature_sensor_handle_t handle = static_cast<temperature_sensor_handle_t>(sensor_);
	// Enabled for the reading only: one reading per request.
	esp_err_t err = temperature_sensor_enable(handle);
	float celsius = 0;
	if (err == ESP_OK) {
		err = temperature_sensor_get_celsius(handle, &celsius);
		temperature_sensor_disable(handle);
	}
	if (err != ESP_OK) {
		ESP_LOGW(kTag, "temperature not read: %s", esp_err_to_name(err));
		*reason = "the temperature sensor did not answer (see the log)";
		return false;
	}
	out.celsius = celsius;
	out.accuracy = kTemperatureAccuracy;
	out.source = "die sensor";
	return true;
#else
	(void)out;
	*reason = "this chip has no temperature sensor";
	return false;
#endif
}

void EspSystemInfo::visit_chip(DiagnosticsVisitor& visitor) const {
	esp_chip_info_t chip;
	esp_chip_info(&chip);
	char revision[12];
	std::snprintf(revision, sizeof(revision), "v%u.%u", static_cast<unsigned>(chip.revision / 100),
		static_cast<unsigned>(chip.revision % 100));
	visitor.text("model", chip_model(chip.model));
	visitor.text("revision", revision);
	visitor.number("cores", chip.cores);
	visitor.number("cpu_mhz", esp_rom_get_cpu_ticks_per_us());
	uint32_t flash = 0;
	if (esp_flash_get_size(nullptr, &flash) == ESP_OK) {
		visitor.number("flash_size", flash);
	}
	visitor.text("flash_mode", CONFIG_ESPTOOLPY_FLASHMODE);
	visitor.text("flash_freq", CONFIG_ESPTOOLPY_FLASHFREQ);
	visitor.text("idf", esp_get_idf_version());
	const esp_partition_t* running = esp_ota_get_running_partition();
	esp_ota_img_states_t state;
	if (running != nullptr && esp_ota_get_state_partition(running, &state) == ESP_OK) {
		visitor.text("ota_state", ota_state_name(state));
	}
}

bool EspSystemInfo::visit_settings_store(DiagnosticsVisitor& visitor, const char** reason) const {
	nvs_stats_t stats;
	esp_err_t err = nvs_get_stats(nullptr, &stats);
	if (err != ESP_OK) {
		*reason = esp_err_to_name(err);
		return false;
	}
	visitor.number("used", stats.used_entries);
	visitor.number("free", stats.free_entries);
	visitor.number("usable", stats.available_entries); // free entries outside the page NVS keeps in reserve
	visitor.number("total", stats.total_entries);
	visitor.number("namespaces", stats.namespace_count);
	return true;
}

bool EspSystemInfo::console_output(ConsoleOutputInfo& out, const char** reason) const {
	if (!hc::esp::console_output().driver_installed()) {
		*reason = "the USB Serial/JTAG driver is not installed: console output is written synchronously";
		return false;
	}
	out = hc::esp::console_output().info();
	return true;
}

} // namespace esp
} // namespace hc
