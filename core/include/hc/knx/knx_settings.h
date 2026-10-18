/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_KNX_KNX_SETTINGS_H
#define HC_KNX_KNX_SETTINGS_H

#include <cstddef>
#include <cstdint>
#include <optional>

#include "hc/platform/settings.h"

namespace hc {
namespace knx {

class KnxTunnel;

/// The device settings of the KNX tunnel (doc/knx.md). Persisted names: never rename.
namespace setting {

/// The gateway, "host[:port]" (a host name or IPv4 literal, port 3671 by default). Without it the
/// tunnel stays down.
constexpr const char* kGateway = "knx.gateway";

/// Boolean: connect in NAT mode (endpoints 0.0.0.0:0, the gateway answers to where the request came
/// from). Default false.
constexpr const char* kNat = "knx.nat";

} // namespace setting

/// Where the KNXnet/IP gateway is.
struct GatewayAddress {
	static constexpr size_t kMaxHost = Settings::kMaxStringLength;

	/// Longest text format() writes, including the NUL.
	static constexpr size_t kMaxText = kMaxHost + 7;

	char host[kMaxHost + 1] = {};
	uint16_t port = 3671;

	bool empty() const { return host[0] == '\0'; }

	/// Reads "host" or "host:port": the host a name or IPv4 literal (hc::host_name_valid()), the port
	/// 1..65535 (default 3671). The whole text has at most Settings::kMaxStringLength bytes. IPv6 is
	/// refused. Empty with `error` naming the problem.
	static std::optional<GatewayAddress> parse(const char* text, const char** error = nullptr);

	/// Writes "host:port". Returns `buffer`.
	const char* format(char* buffer, size_t size) const;
};

/// The gateway and the NAT flag as stored. `gateway` is empty when knx.gateway is not set; a stored
/// value that does not parse is logged (tag "hc.knx") and treated as not set. A missing or
/// unreadable knx.nat is false.
void read_gateway_settings(const Settings& settings, GatewayAddress& gateway, bool& nat);

/// Validates and stores knx.gateway. False with `error` (nothing stored when invalid).
bool store_gateway_setting(Settings& settings, const char* text, const char** error);

/// knx.gateway as the console's `knx gateway set|clear` and the settings API change it: validates and
/// stores `text` ("" erases the setting) and hands the gateway to the running `tunnel` (nullptr: a
/// build without one; only stored), which reconnects to it (or closes, when cleared). False with
/// `error` (nothing changed).
bool apply_gateway_setting(Settings& settings, KnxTunnel* tunnel, const char* text, const char** error);

/// knx.nat as the console's `knx nat on|off` and the settings API change it: stores it and reconnects
/// the running `tunnel` (nullptr: only stored) in that mode. False with `error`.
bool apply_nat_setting(Settings& settings, KnxTunnel* tunnel, bool nat, const char** error);

} // namespace knx
} // namespace hc

#endif // HC_KNX_KNX_SETTINGS_H
