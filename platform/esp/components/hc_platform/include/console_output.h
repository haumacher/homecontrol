/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_CONSOLE_OUTPUT_H
#define HC_ESP_CONSOLE_OUTPUT_H

#include <cstddef>
#include <cstdint>

#include "hc/platform/system_info.h"

namespace hc {
namespace esp {

/// The console's output path on the USB Serial/JTAG port (issue #46): no task ever blocks on it.
///
/// start() installs the USB Serial/JTAG driver, creates the writer task and points stdout and stderr
/// at the device kOutputDevice, whose write() appends to an hc::OutputRing of kCapacity bytes under a
/// short critical section and wakes the writer. Everything printed goes this way: the ESP log sink
/// of hc::Log and the esp_log output of the IDF components (esp_log prints through vprintf to
/// stdout), the console's command output, its prompt and line editor, and the Improv frames
/// (append_frame(), whole records, never inside a line). The writer task, below the event loop and
/// the serial reader, takes the ring's contents and does the blocking writes to the driver; it is
/// the only one that waits for the host. A full ring drops whole lines and counts them, and the
/// stream carries a marker line "… N line(s) dropped" where output resumed (hc::OutputRing).
/// Drops while nobody reads the port (no USB host, or the port not opened: the writer made no
/// progress for a second) are counted apart (ConsoleOutputInfo::dropped_unread).
///
/// Output before start() (the bootloader, the IDF start-up before app_main) uses the IDF's own
/// synchronous path; start() is the first thing app_main does. The panic handler prints through
/// the IDF's own path, past the ring.
class ConsoleOutput {
public:
	/// The ring: a few log lines' bursts, the output of one console command (`status`).
	static constexpr size_t kCapacity = 4096;
	static constexpr const char* kOutputDevice = "/dev/hcout";

	/// Installs the driver, starts the writer, redirects stdout and stderr. Once, first thing in
	/// app_main. False (logged) when the driver could not be installed; the console then has no
	/// input either. A failed redirection (logged) leaves stdout on the IDF's path.
	bool start();

	/// The driver is installed (the serial reader may read it).
	bool driver_installed() const;

	/// Queues one Improv frame as a whole (binary, no line-ending translation). False when it was
	/// dropped (counted like a line) or the path is not running.
	bool append_frame(const uint8_t* frame, size_t length);

	/// The queue's figures (the System page and the console's `status`). Any task.
	ConsoleOutputInfo info() const;

	/// Waits up to `timeout_ms` for the queue to drain while a host is attached (before a restart,
	/// so that "restarting" reaches the terminal). Not for the event loop's steady state.
	void drain(uint32_t timeout_ms);
};

/// The one console output path.
ConsoleOutput& console_output();

} // namespace esp
} // namespace hc

#endif // HC_ESP_CONSOLE_OUTPUT_H
