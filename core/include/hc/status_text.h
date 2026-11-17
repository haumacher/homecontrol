/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_STATUS_TEXT_H
#define HC_STATUS_TEXT_H

#include <cstddef>

namespace hc {

/// The limits of the status's texts (homecontrol #92): the WebSocket's `status` message fits
/// LiveFeed::kMessageSize by construction. Each limit is the bytes of the text's JSON form, quotes
/// and escapes included (JsonWriter::bounded(): a longer text is cut and ends in "…"); the status
/// test runs WebApi::write_status() through JsonWriter::bound() and requires the sum to fit.
namespace status_text {

/// A state or code from one of the firmware's tables (the status test checks that no name of the
/// tables it writes is cut).
constexpr size_t kCode = 32;
/// A format version, a CRC.
constexpr size_t kTiny = 13;
/// A fingerprint, a version, a schema version, a KNX address.
constexpr size_t kShort = 22;
/// The build (`git describe`).
constexpr size_t kBuild = 40;
/// A host name with `.local`, the device's name, the local time.
constexpr size_t kHost = 42;
/// An IP address (IPv6 included).
constexpr size_t kAddress = 47;
/// A name or an id (the image's project and name, the E-Home installation), a server, the time
/// zone rule.
constexpr size_t kName = 66;
/// A URL (the update source).
constexpr size_t kUrl = 82;
/// A reason or an error for people.
constexpr size_t kText = 100;

/// The budgets of the platform's members, whose writers the core does not know (WebHost's update
/// state, the Diagnostics' boot facts): the bytes of the member's object. A platform writer that
/// exceeds its budget is replaced by `{"error": "..."}` (WebApi::write_status()).
constexpr size_t kUpdateBudget = 380;
constexpr size_t kBootBudget = 230;
/// A text of the platform's boot facts.
constexpr size_t kFact = 20;

} // namespace status_text

} // namespace hc

#endif // HC_STATUS_TEXT_H
