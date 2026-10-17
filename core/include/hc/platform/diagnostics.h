/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_DIAGNOSTICS_H
#define HC_PLATFORM_DIAGNOSTICS_H

#include <cstddef>
#include <cstdint>

namespace hc {

/// Why the current run started (doc/diagnostics.md). The device maps esp_reset_reason(); the POSIX
/// build derives it from the marker file of the previous run. The numbers are stored in the
/// device's boot record (NVS `diag/reason`) and are therefore frozen: append, never renumber.
enum class ResetReason : uint8_t {
	unknown = 0,          ///< Cannot be determined (POSIX: the previous run left no stop record).
	power_on = 1,         ///< Power-on (POSIX: first start, no marker of a previous run).
	external = 2,         ///< External reset pin.
	software = 3,         ///< esp_restart() (POSIX: the previous run stopped on SIGINT/SIGTERM).
	panic = 4,            ///< Exception or abort (POSIX: the previous run died on a fatal signal).
	interrupt_watchdog = 5,
	task_watchdog = 6,
	other_watchdog = 7,
	deep_sleep = 8,       ///< Wake-up from deep sleep.
	brownout = 9,
	sdio = 10,
	usb = 11,             ///< Reset by the USB peripheral (USB Serial/JTAG: esptool, the monitor).
	jtag = 12,
	efuse = 13,
	power_glitch = 14,
	cpu_lockup = 15,
};

/// The reason in words ("power-on", "software restart", "brownout", "task watchdog", ...).
const char* reset_reason_name(ResetReason reason);

/// True for the reasons that mean the previous run crashed or hung (panic, the watchdogs, a CPU
/// lock-up): the ones after which a core dump is expected.
bool reset_reason_is_crash(ResetReason reason);

/// Receives the platform's boot facts (Diagnostics::visit_boot()). Keys are short lower-case
/// identifiers, stable because the web API reports them (`/api/status` member `boot`).
class DiagnosticsVisitor {
public:
	virtual void number(const char* key, uint64_t value) = 0;
	virtual void text(const char* key, const char* value) = 0;
	virtual void flag(const char* key, bool value) = 0;

protected:
	~DiagnosticsVisitor() = default;
};

/// What the platform knows about this run and the one before (doc/diagnostics.md): the reset
/// reason, which the controller logs in its first line, and further facts in the platform's own
/// terms - the device's boot record (boot count, uptime of the previous run, brownouts, a stored
/// core dump), the POSIX process (PID, start time, the signal that stopped the previous run). The
/// core reports them without knowing them (hc::write_boot_status(), the console's `status`).
///
/// Threading: called on the event loop's thread.
class Diagnostics {
public:
	virtual ResetReason reset_reason() const = 0;

	/// Calls `visitor` for every fact, in a fixed order.
	virtual void visit_boot(DiagnosticsVisitor& visitor) const = 0;

	/// Size in bytes of the stored crash dump (the device's ELF core dump, raw as esp-coredump reads
	/// it; GET /api/coredump), 0 if none is stored or the platform keeps none.
	virtual size_t coredump_size() const { return 0; }

	/// Copies `size` bytes of the stored dump from `offset`. False past the end or on a read error.
	virtual bool coredump_read(size_t offset, void* buffer, size_t size) const {
		(void)offset;
		(void)buffer;
		(void)size;
		return false;
	}

protected:
	~Diagnostics() = default;
};

} // namespace hc

#endif // HC_PLATFORM_DIAGNOSTICS_H
