/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_CONSOLE_H
#define HC_ESP_CONSOLE_H

#include "hc/controller.h"
#include "loop_queue.h"
#include "net_services.h"
#include "wifi_driver.h"

namespace hc {
namespace esp {

/// What the console commands operate on. Must outlive the console.
struct ConsoleContext {
	Controller& controller;
	LoopQueue& queue;
	WifiDriver& wifi;
	NetServices& services;
};

/// Starts the line console (IDF console REPL, prompt "hc> ") on the USB Serial/JTAG port. The REPL
/// runs in its own task; every command body runs on the event loop (through the LoopQueue) while
/// the REPL waits, so commands see the core's state without locking. The command table is in
/// console.cpp. Logs and returns false if the console cannot start.
bool console_start(ConsoleContext& context);

} // namespace esp
} // namespace hc

#endif // HC_ESP_CONSOLE_H
