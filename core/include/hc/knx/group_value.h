/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_KNX_GROUP_VALUE_H
#define HC_KNX_GROUP_VALUE_H

#include <cstddef>
#include <cstdint>

#include "hc/datapoint.h"
#include "hc/dpt.h"
#include "hc/knx/address.h"
#include "hc/knx/cemi.h"

namespace hc {
namespace knx {

/// Builds the L_Data.req of a group write or response of `value` as `dpt` to `destination`: the
/// DPT codec encodes, an embedded type (Dpt::embedded()) travels in the APCI octet. False with the
/// codec's or the frame's reason in `error`.
bool make_group_value(LData& frame, GroupAddress destination, GroupService service, Dpt dpt, const Value& value,
	const char** error);

/// Builds the L_Data.req of a group read of `destination`.
void make_group_read(LData& frame, GroupAddress destination);

/// Decodes the data of a group write or response as `dpt`. An embedded type needs the short form
/// (value in the APCI octet), every other type the long form; a mismatch is refused as
/// wrong_length. The codec's refusals pass through.
DptResult decode_group_value(Dpt dpt, const GroupData& data, Value& value);

/// Reads the text form of a value of `dpt` as the console and knxtool take it, by the DPT's value
/// type: boolean "on"/"off"/"true"/"false"/"1"/"0"; numbers in decimal ("21.5", "-3", "1e3");
/// a string as it is; controlled "<control>,<value>" ("1,5"); time of day "HH:MM[:SS]"; date
/// "YYYY-MM-DD"; date-time "YYYY-MM-DDTHH:MM:SS"; color "R,G,B" or "R,G,B,W" (levels 0..255). False
/// with `error` naming the expected form. The range of the type is checked by the codec later.
bool parse_value_text(Dpt dpt, const char* text, Value& value, const char** error);

/// Writes the data octets as "0C 1A" ("" for none). Returns `buffer`.
const char* format_hex(const uint8_t* data, size_t length, char* buffer, size_t size);

} // namespace knx
} // namespace hc

#endif // HC_KNX_GROUP_VALUE_H
