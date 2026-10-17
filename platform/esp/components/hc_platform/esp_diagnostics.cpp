/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "esp_diagnostics.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "esp_core_dump.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "hc/log.h"
#include "nvs.h"
#include "sdkconfig.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.diag";

/// The boot record's NVS namespace and keys (frozen, doc/diagnostics.md).
constexpr const char* kNamespace = "diag";
constexpr const char* kBoots = "boots";
constexpr const char* kReason = "reason";
constexpr const char* kUptime = "uptime";
constexpr const char* kBrownouts = "brownouts";

/// How often the uptime of the current run is written.
constexpr uint32_t kUptimePeriodMs = 60000;

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
constexpr bool kCoreDumpToFlash = true;
#else
constexpr bool kCoreDumpToFlash = false;
#endif

ResetReason map_reset_reason(esp_reset_reason_t reason) {
	switch (reason) {
	case ESP_RST_POWERON: return ResetReason::power_on;
	case ESP_RST_EXT: return ResetReason::external;
	case ESP_RST_SW: return ResetReason::software;
	case ESP_RST_PANIC: return ResetReason::panic;
	case ESP_RST_INT_WDT: return ResetReason::interrupt_watchdog;
	case ESP_RST_TASK_WDT: return ResetReason::task_watchdog;
	case ESP_RST_WDT: return ResetReason::other_watchdog;
	case ESP_RST_DEEPSLEEP: return ResetReason::deep_sleep;
	case ESP_RST_BROWNOUT: return ResetReason::brownout;
	case ESP_RST_SDIO: return ResetReason::sdio;
	case ESP_RST_USB: return ResetReason::usb;
	case ESP_RST_JTAG: return ResetReason::jtag;
	case ESP_RST_EFUSE: return ResetReason::efuse;
	case ESP_RST_PWR_GLITCH: return ResetReason::power_glitch;
	case ESP_RST_CPU_LOCKUP: return ResetReason::cpu_lockup;
	default: return ResetReason::unknown;
	}
}

uint32_t uptime_s() {
	return static_cast<uint32_t>(esp_timer_get_time() / 1000000);
}

/// Reads a u32 of the record; a missing key is 0.
esp_err_t get_u32(nvs_handle_t handle, const char* key, uint32_t& out) {
	esp_err_t err = nvs_get_u32(handle, key, &out);
	if (err == ESP_ERR_NVS_NOT_FOUND) {
		out = 0;
		return ESP_OK;
	}
	return err;
}

nvs_handle_t g_handle = 0;
bool g_handle_open = false;

} // namespace

void EspDiagnostics::init() {
	reason_ = map_reset_reason(esp_reset_reason());
	read_record();
	check_coredump();
}

void EspDiagnostics::read_record() {
	esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &g_handle);
	if (err != ESP_OK) {
		record_error_ = esp_err_to_name(err);
		return;
	}
	g_handle_open = true;
	uint32_t boots = 0;
	uint32_t brownouts = 0;
	uint32_t uptime = 0;
	err = get_u32(g_handle, kBoots, boots);
	if (err == ESP_OK) {
		err = get_u32(g_handle, kBrownouts, brownouts);
	}
	if (err == ESP_OK) {
		err = get_u32(g_handle, kUptime, uptime);
	}
	if (err != ESP_OK) {
		record_error_ = esp_err_to_name(err);
		return;
	}
	boots_ = boots + 1;
	brownouts_ = brownouts + (reason_ == ResetReason::brownout ? 1 : 0);
	previous_uptime_s_ = uptime;
	// One commit for the whole update; the uptime starts again from 0, so a run that dies within
	// its first minute does not report the run before it.
	err = nvs_set_u32(g_handle, kBoots, boots_);
	if (err == ESP_OK) {
		err = nvs_set_u32(g_handle, kBrownouts, brownouts_);
	}
	if (err == ESP_OK) {
		err = nvs_set_u32(g_handle, kReason, static_cast<uint32_t>(reason_));
	}
	if (err == ESP_OK) {
		err = nvs_set_u32(g_handle, kUptime, 0);
	}
	if (err == ESP_OK) {
		err = nvs_commit(g_handle);
	}
	if (err != ESP_OK) {
		record_error_ = esp_err_to_name(err);
		return;
	}
	last_written_uptime_s_ = 0;
	record_ok_ = true;
}

void EspDiagnostics::check_coredump() {
	partition_ = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr) != nullptr;
	if (!partition_ || !kCoreDumpToFlash) {
		return;
	}
	if (esp_core_dump_image_check() != ESP_OK) {
		return; // none stored (erased), or not valid
	}
	size_t address = 0;
	size_t size = 0;
	if (esp_core_dump_image_get(&address, &size) != ESP_OK || size == 0) {
		return;
	}
	coredump_address_ = address;
	coredump_size_ = size;
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
	// The summary carries a stack dump of about 1 KB: too much for app_main's stack, taken from the
	// heap for this moment only.
	esp_core_dump_summary_t* summary = new (std::nothrow) esp_core_dump_summary_t;
	if (summary != nullptr) {
		if (esp_core_dump_get_summary(summary) == ESP_OK) {
			summary_ok_ = true;
			std::snprintf(coredump_task_, sizeof(coredump_task_), "%.16s", summary->exc_task);
			coredump_pc_ = summary->exc_pc;
		}
		delete summary;
	}
	if (esp_core_dump_get_panic_reason(panic_reason_, sizeof(panic_reason_)) != ESP_OK) {
		panic_reason_[0] = '\0';
	}
#endif
}

void EspDiagnostics::report() {
	if (!record_ok_) {
		Log::warn(kTag, "boot record (NVS namespace %s) unavailable: %s; boot count and previous uptime unknown",
			kNamespace, record_error_ != nullptr ? record_error_ : "not initialized");
	}
	if (!partition_) {
		Log::warn(kTag, "no coredump partition: the partition table predates the diagnostics (issue #4); flash the "
			"full image (write_flash @flash_args) to get core dumps");
		return;
	}
	if (!kCoreDumpToFlash) {
		Log::warn(kTag, "core dumps to flash are disabled in this build (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)");
		return;
	}
	if (coredump_present()) {
		Log::warn(kTag, "core dump stored (%u B, from %s): task %s, PC 0x%08" PRIx32 "%s%s; GET /api/coredump downloads "
			"it, 'coredump clear' erases it",
			static_cast<unsigned>(coredump_size_), reset_reason_is_crash(reason_) ? "this reset" : "an earlier crash",
			summary_ok_ ? coredump_task_ : "?", coredump_pc_, panic_reason_[0] != '\0' ? ", " : "", panic_reason_);
	} else if (reset_reason_is_crash(reason_)) {
		Log::warn(kTag, "the %s left no valid core dump", reset_reason_name(reason_));
	}
}

void EspDiagnostics::start(EventLoop& loop, const Clock& clock) {
	if (!record_ok_) {
		return; // report() said why
	}
	uptime_timer_ = loop.schedule_periodic(clock.monotonic_ms() + kUptimePeriodMs, kUptimePeriodMs,
		Callback::member<EspDiagnostics, &EspDiagnostics::write_uptime>(this));
	if (!uptime_timer_.valid()) {
		Log::error(kTag, "refusing the uptime record: no timer slot left; the previous uptime will be missing");
	}
}

void EspDiagnostics::write_uptime() {
	// One u32 entry (32 bytes of NVS) and a commit per minute; a late loop that missed periods
	// writes once (the periodic timer coalesces them). Unchanged values are not written.
	uint32_t now = uptime_s();
	if (!g_handle_open || now == last_written_uptime_s_) {
		return;
	}
	esp_err_t err = nvs_set_u32(g_handle, kUptime, now);
	if (err == ESP_OK) {
		err = nvs_commit(g_handle);
	}
	if (err != ESP_OK) {
		Log::warn(kTag, "cannot store the uptime in the boot record: %s", esp_err_to_name(err));
		return;
	}
	last_written_uptime_s_ = now;
}

void EspDiagnostics::watch_loop() {
	esp_err_t err = esp_task_wdt_add(nullptr);
	if (err != ESP_OK) {
		Log::error(kTag, "event loop NOT under the task watchdog: %s; a stuck loop will hang instead of resetting",
			esp_err_to_name(err));
		return;
	}
	watched_ = true;
#ifdef CONFIG_ESP_TASK_WDT_TIMEOUT_S
	Log::info(kTag, "event loop under the task watchdog (%d s, %s)", CONFIG_ESP_TASK_WDT_TIMEOUT_S,
#if CONFIG_ESP_TASK_WDT_PANIC
		"panics and resets"
#else
		"warns only"
#endif
	);
#endif
}

void EspDiagnostics::feed() {
	if (watched_) {
		esp_task_wdt_reset();
	}
}

void EspDiagnostics::visit_boot(DiagnosticsVisitor& visitor) const {
	if (record_ok_) {
		visitor.number("count", boots_);
		visitor.number("brownouts", brownouts_);
		visitor.number("previous_uptime_s", previous_uptime_s_);
	}
	visitor.flag("coredump", coredump_present());
	if (coredump_present() && summary_ok_) {
		char pc[12];
		std::snprintf(pc, sizeof(pc), "0x%08" PRIx32, coredump_pc_);
		visitor.text("coredump_task", coredump_task_);
		visitor.text("coredump_pc", pc);
	}
}

bool EspDiagnostics::coredump_read(size_t offset, void* buffer, size_t size) const {
	if (!coredump_present() || offset > coredump_size_ || size > coredump_size_ - offset) {
		return false;
	}
	return esp_flash_read(esp_flash_default_chip, buffer, static_cast<uint32_t>(coredump_address_ + offset),
		static_cast<uint32_t>(size)) == ESP_OK;
}

bool EspDiagnostics::coredump_clear() {
	if (!partition_) {
		Log::warn(kTag, "refusing coredump clear: there is no coredump partition");
		return false;
	}
	esp_err_t err = esp_core_dump_image_erase();
	if (err != ESP_OK) {
		Log::error(kTag, "cannot erase the core dump: %s", esp_err_to_name(err));
		return false;
	}
	Log::info(kTag, "core dump erased");
	coredump_size_ = 0;
	coredump_address_ = 0;
	summary_ok_ = false;
	panic_reason_[0] = '\0';
	return true;
}

void EspDiagnostics::print_coredump_status() const {
	if (!partition_) {
		std::printf("no coredump partition: the partition table predates the diagnostics; flash the full image\n");
		return;
	}
	if (!coredump_present()) {
		std::printf("no core dump stored\n");
		return;
	}
	std::printf("core dump %u B at 0x%06x (from %s)\n", static_cast<unsigned>(coredump_size_),
		static_cast<unsigned>(coredump_address_), reset_reason_is_crash(reason_) ? "this reset" : "an earlier crash");
	if (summary_ok_) {
		std::printf("task      %s\n", coredump_task_);
		std::printf("pc        0x%08" PRIx32 "\n", coredump_pc_);
	}
	if (panic_reason_[0] != '\0') {
		std::printf("reason    %s\n", panic_reason_);
	}
	std::printf("download  GET /api/coredump > core.bin; esp-coredump info_corefile -c core.bin -t raw build-esp32c6/homecontrol.elf\n");
}

} // namespace esp
} // namespace hc
