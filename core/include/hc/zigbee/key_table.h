/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ZIGBEE_KEY_TABLE_H
#define HC_ZIGBEE_KEY_TABLE_H

#include "hc/platform/zigbee_driver.h"
#include "hc/zigbee/zcl.h"

namespace hc {
namespace zigbee {

/// The trust center's link-key table as a platform's stack offers it (homecontrol #99, doc/zigbee.md
/// "Rejoin policy"): the three operations ZigbeeDriver::trust_device() and forget_device() are made of.
/// The outcome is judged by has_key() alone, never by the answer of add_standard_key(): the
/// esp-zigbee-lib 1.6.8 answers ESP_FAIL when the key was written and ESP_OK when it was not.
class KeyTable {
public:
	/// Whether the trust center holds a verified or provisional key for the device: what it looks for
	/// when the device rejoins by a trust-center rejoin.
	virtual bool has_key(Ieee ieee) = 0;
	/// Writes the trust center's standard key (global, provisional) for the device, as the stack does
	/// for a joining device; the stack's answer, for the log only.
	virtual int add_standard_key(Ieee ieee) = 0;
	/// Deletes every key the trust center holds for the device.
	virtual void delete_keys(Ieee ieee) = 0;

protected:
	~KeyTable() = default;
};

/// ZigbeeDriver::trust_device() over a key table: kept when the device has a key, restored when it has
/// one after add_standard_key(), refused otherwise; `stack_answer` the answer of add_standard_key() (0
/// when it was not called).
ZigbeeDriver::Trust restore_key(KeyTable& table, Ieee ieee, int& stack_answer);

/// ZigbeeDriver::forget_device() over a key table: true when no key is left after delete_keys().
bool forget_key(KeyTable& table, Ieee ieee);

} // namespace zigbee
} // namespace hc

#endif // HC_ZIGBEE_KEY_TABLE_H
