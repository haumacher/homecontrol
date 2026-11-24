/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_RADIO_TAP_H
#define HC_ESP_RADIO_TAP_H

#include <cstdint>

#include "hc/platform/zigbee_driver.h"

namespace hc {
namespace esp {

/// Counts, below the Zigbee stack, what the 802.15.4 radio hears and sends (homecontrol #117): the MAC
/// Data Requests (polls) of each short address, whether their acknowledgement said a frame is pending,
/// the gaps between them, and the outcome of every frame sent. It registers its own callbacks with the
/// driver, which hand every event on to the stack's (ZBOSS's esp_ieee802154_receive_done() and
/// friends) unchanged.
///
/// Installs the callbacks; must run before the stack initializes the radio. False when the driver
/// refused them (the stack still gets every event then, nothing is counted).
bool radio_tap_install();

/// What the radio heard of `short_addr` and sent to it (ZigbeeDriver::read_polls()). False when the
/// tap is not installed.
bool radio_tap_read(uint16_t short_addr, bool reset, ZigbeePolls& out);

} // namespace esp
} // namespace hc

#endif // HC_ESP_RADIO_TAP_H
