/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/zigbee/key_table.h"

namespace hc {
namespace zigbee {

ZigbeeDriver::Trust restore_key(KeyTable& table, Ieee ieee, int& stack_answer) {
	stack_answer = 0;
	if (table.has_key(ieee)) {
		return ZigbeeDriver::Trust::kept;
	}
	stack_answer = table.add_standard_key(ieee);
	// The answer does not tell (KeyTable): only the table does.
	return table.has_key(ieee) ? ZigbeeDriver::Trust::restored : ZigbeeDriver::Trust::refused;
}

bool forget_key(KeyTable& table, Ieee ieee) {
	table.delete_keys(ieee);
	return !table.has_key(ieee);
}

} // namespace zigbee
} // namespace hc
