/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_PLANNED_RESTART_H
#define HC_ESP_PLANNED_RESTART_H

#include "hc/controller.h"
#include "hc/event_loop.h"

namespace hc {
namespace esp {

/// Every restart the application decides on (homecontrol #103, doc/core.md "Planned restart"): the
/// API's restart and factory reset, the console's `restart` and `factory reset`, the switch to the
/// OTA updater for an update or a firmware upload. Controller::shutdown() first closes the
/// connections (the KNX tunnel's DISCONNECT_REQUEST, so the gateway frees its slot at once; at most
/// Shutdown::kMaxWaitMs), then `before` runs if given (the factory reset's erase; it may restart
/// itself) and the device restarts (the console output drained, esp_restart()).
///
/// Called on the event loop; returns at once, the restart follows from the loop. A second call
/// while one is under way is refused and logged.
void planned_restart(Controller& controller, const char* reason, Callback before = Callback());

} // namespace esp
} // namespace hc

#endif // HC_ESP_PLANNED_RESTART_H
