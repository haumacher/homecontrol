/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_POSIX_DIAGNOSTICS_H
#define HC_POSIX_DIAGNOSTICS_H

#include <cstdint>
#include <string>

#include "hc/platform/diagnostics.h"

namespace hc {
namespace posix {

/// The POSIX build's diagnostics (doc/diagnostics.md): the process ID, the start time and the
/// signal that stopped the previous run, from a marker file beside the settings
/// (`homecontrol.settings` -> `homecontrol.run`).
///
/// start() reads the previous run's marker and writes this run's ("pid <pid> started <unix s>");
/// the run appends "signal <n>" when it stops: stopped() for SIGINT/SIGTERM (called by main), and an
/// async-signal-safe handler for the fatal signals (SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT), which
/// then lets the signal take its default course (a core file if the shell allows one). The reset
/// reason follows: no marker - power-on (first start); SIGINT, SIGTERM or SIGHUP - software
/// restart; a fatal signal - panic; a marker without a signal (SIGKILL, power loss, an error exit
/// during start-up) - unknown.
class PosixDiagnostics : public Diagnostics {
public:
	explicit PosixDiagnostics(std::string marker_path);

	/// The marker for a settings file: `.settings` replaced by `.run`, or `.run` appended.
	static std::string marker_for_settings(const std::string& settings_path);

	/// Reads the previous marker, writes this run's and installs the fatal-signal handlers. A marker
	/// that cannot be written is logged; the run continues without a record.
	void start();

	/// Records the signal that stops the run (SIGINT, SIGTERM) in the marker.
	void stopped(int signal);

	ResetReason reset_reason() const override { return reason_; }
	void visit_boot(DiagnosticsVisitor& visitor) const override;

	/// "SIGTERM", "SIGSEGV", ... or "signal <n>".
	static std::string signal_name(int signal);

private:
	std::string path_;
	ResetReason reason_ = ResetReason::power_on;
	/// The previous run's stop signal: 0 none recorded, -1 no previous marker.
	int previous_signal_ = -1;
	int64_t started_ = 0;
	long pid_ = 0;
};

} // namespace posix
} // namespace hc

#endif // HC_POSIX_DIAGNOSTICS_H
