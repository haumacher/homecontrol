/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_DIAGNOSTICS_H
#define HC_DIAGNOSTICS_H

#include <cstddef>

#include "hc/json.h"
#include "hc/platform/diagnostics.h"

namespace hc {

/// Writes the member `boot` of `/api/status` (spec/api.md): `{"reason": "<reset_reason_name>",
/// <the platform's facts>}`; without diagnostics (nullptr) `{"reason": "unknown"}`. Called inside
/// the status object.
void write_boot_status(JsonWriter& json, const Diagnostics* diagnostics);

/// Formats the platform's facts as "key value, key value, ..." (texts unquoted, flags as yes/no)
/// into `buffer`; an empty string if there are none. Returns the length (truncated to fit).
size_t format_boot_facts(const Diagnostics& diagnostics, char* buffer, size_t size);

} // namespace hc

#endif // HC_DIAGNOSTICS_H
