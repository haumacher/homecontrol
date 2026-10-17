/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_DIAGNOSTICS_H
#define HC_ESP_DIAGNOSTICS_H

#include <cstddef>
#include <cstdint>

#include "hc/event_loop.h"
#include "hc/platform/clock.h"
#include "hc/platform/diagnostics.h"

namespace hc {
namespace esp {

/// The device's diagnostics (doc/diagnostics.md): the reset reason (esp_reset_reason()), the boot
/// record in the NVS namespace `diag` and the core dump in the `coredump` partition.
///
/// The boot record (frozen keys, all u32): `boots` (starts since the record began), `reason` (this
/// start's ResetReason), `uptime` (seconds of the current run, written every minute, so at the
/// next start it is the previous run's uptime to within a minute), `brownouts` (starts after a
/// brownout).
///
/// Order: init() in app_main after nvs_flash_init() and before the Controller logs (it reads and
/// updates the record, checks the core dump); report() on the event loop after Controller::start()
/// (logs through hc::Log, so the lines reach the ring and GET /api/log); start() schedules the
/// uptime writes; watch_loop() and feed() put the calling task under the task watchdog.
class EspDiagnostics : public Diagnostics {
public:
	/// Longest panic reason kept from the core dump (characters).
	static constexpr size_t kMaxPanicReason = 96;

	void init();
	void report();
	void start(EventLoop& loop, const Clock& clock);

	/// Subscribes the calling task (the event loop) to the task watchdog; logs the result.
	void watch_loop();
	/// Feeds the task watchdog for the calling task (every loop turn; cheap).
	void feed();

	ResetReason reset_reason() const override { return reason_; }
	void visit_boot(DiagnosticsVisitor& visitor) const override;

	uint32_t boot_count() const { return boots_; }

	/// The coredump partition exists (the table of issue #4 is flashed).
	bool coredump_partition() const { return partition_; }
	/// A valid core dump is stored.
	bool coredump_present() const { return coredump_size_ != 0; }
	size_t coredump_size() const override { return coredump_size_; }
	/// Copies `size` bytes of the stored dump from `offset` (the raw image esp-coredump reads).
	/// False past the end or on a flash error. Any task.
	bool coredump_read(size_t offset, void* buffer, size_t size) const override;
	/// Erases the stored dump. False (logged) on failure.
	bool coredump_clear();
	/// Prints the core dump's state and summary to stdout (the console's `coredump status`).
	void print_coredump_status() const;

private:
	void read_record();
	void check_coredump();
	void write_uptime();

	ResetReason reason_ = ResetReason::unknown;
	uint32_t boots_ = 0;
	uint32_t brownouts_ = 0;
	uint32_t previous_uptime_s_ = 0;
	bool record_ok_ = false;
	const char* record_error_ = nullptr;
	bool partition_ = false;
	size_t coredump_address_ = 0;
	size_t coredump_size_ = 0;
	bool summary_ok_ = false;
	char coredump_task_[17] = {};
	uint32_t coredump_pc_ = 0;
	char panic_reason_[kMaxPanicReason + 1] = {};
	uint32_t last_written_uptime_s_ = UINT32_MAX;
	TimerHandle uptime_timer_;
	bool watched_ = false;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_DIAGNOSTICS_H
