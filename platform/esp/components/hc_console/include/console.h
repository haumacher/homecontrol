/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_CONSOLE_H
#define HC_ESP_CONSOLE_H

#include "esp_diagnostics.h"
#include "hc/controller.h"
#include "hc/platform/blob_store.h"
#include "loop_queue.h"
#include "net_services.h"
#include "ota_service.h"
#include "wifi_driver.h"

namespace hc {
namespace esp {

/// A console command contributed by another component (the web server's `web`, doc/web.md). Its
/// body runs on the event loop like the built-in commands; it prints to stdout and returns 0 on
/// success.
struct ConsoleCommand {
	const char* name;
	const char* hint; ///< Arguments shown by `help`, nullptr for none.
	const char* help;
	int (*run)(void* context, int argc, char** argv);
	void* context;
};

/// What the console commands operate on. Must outlive the console.
struct ConsoleContext {
	Controller& controller;
	LoopQueue& queue;
	WifiDriver& wifi;
	NetServices& services;
	OtaService& ota;
	/// The device the REPL reads its input from (the serial port shared with Improv,
	/// SerialMux::kConsoleInput); its output goes to stdout.
	const char* input_device;
	/// Further commands registered next to the built-in ones (may be nullptr); the array must
	/// outlive the console.
	const ConsoleCommand* extensions = nullptr;
	size_t extension_count = 0;
	/// The diagnostics behind `status`, `uptime`, `coredump` (may be nullptr: those refuse).
	EspDiagnostics* diagnostics = nullptr;
	/// The blob store `factory reset` empties (may be nullptr: the reset then keeps the state
	/// directory and says so).
	BlobStore* blobs = nullptr;
};

/// Starts the line console (linenoise and the IDF command registry, prompt "hc> ") on the USB
/// Serial/JTAG port (doc/diagnostics.md has the command reference). The REPL runs in its own task,
/// with `input_device` as its stdin (the IDF REPL reads the port itself and so cannot share it with
/// Improv); every command body runs on the event loop (through the LoopQueue) while the REPL waits,
/// so commands see the core's state without locking. The built-in command table is in console.cpp;
/// other components add theirs through ConsoleContext::extensions.
///
/// Only the task is created on the caller's thread (the event loop): the registry, the line
/// editor's set-up and its terminal probe (up to 500 ms waiting for an answer) run in the REPL
/// task, so the loop's timers keep their periods while the console starts. Returns false (logged)
/// if the task cannot be created; a failure inside the task is logged there.
bool console_start(ConsoleContext& context);

} // namespace esp
} // namespace hc

#endif // HC_ESP_CONSOLE_H
