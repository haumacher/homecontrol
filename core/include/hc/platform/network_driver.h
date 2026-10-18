/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_NETWORK_DRIVER_H
#define HC_PLATFORM_NETWORK_DRIVER_H

#include <cstdint>

namespace hc {

/// The radio link's facts for the System page (NetworkDriver::link_details(), spec/api.md "System
/// diagnostics"). Empty strings and the "unknown" values mark what the driver does not know.
struct LinkDetails {
	char ssid[33] = {};
	/// The primary channel; 0: unknown.
	uint8_t channel = 0;
	/// The access point's signal in dBm while associated; `has_rssi` false otherwise.
	int8_t rssi = 0;
	bool has_rssi = false;
	/// The transmit power limit in dBm (quarter dB steps); `has_tx_power` false when unknown.
	double tx_power_dbm = 0;
	bool has_tx_power = false;
	/// The power-save mode in effect ("none", "min", "max"); "" when unknown.
	const char* power_save = "";
	/// "aa:bb:cc:dd:ee:ff"; "" when unknown.
	char mac[18] = {};
	/// The interface's IPv4 gateway, netmask and first DNS server in dotted form; "" when unknown.
	char gateway[16] = {};
	char netmask[16] = {};
	char dns[16] = {};
};

/// Result of NetworkDriver::connect().
enum class ConnectResult {
	/// An attempt is under way; the driver reports its outcome later through
	/// Connectivity::link_up() or link_down().
	started,
	/// The attempt could not be started now (a transient driver error, logged by the driver). The
	/// core retries after the backoff delay.
	failed,
	/// The driver has no usable credentials (logged by the driver). The core falls back to
	/// pairing.
	refused,
};

/// The platform's network link (the WiFi station on the ESP, the host's network on POSIX). The
/// core's Connectivity decides *when* to connect, when to pair (WPS push-button on the ESP) and
/// when to give up on the stored credentials; the driver only executes and reports what happens
/// through Connectivity's link_*() and pairing_*() methods - always on the event loop's thread and
/// never from within one of the calls below (post the report instead).
class NetworkDriver {
public:
	/// Whether credentials are stored.
	virtual bool provisioned() const = 0;

	/// Starts a connection attempt with the stored credentials (dropping an existing link first).
	virtual ConnectResult connect() = 0;

	/// Drops the link or aborts an attempt. The resulting down report, if any, is ignored by the core
	/// when it asked for the disconnect.
	virtual void disconnect() = 0;

	/// Starts listening for a pairing partner (WPS push-button). An existing link stays up as long
	/// as the platform can keep it. Returns false (logged) if pairing cannot start now or is not
	/// supported; the core retries after the backoff delay where pairing is required.
	virtual bool start_pairing() = 0;

	/// Ends pairing (and the association it may have made).
	virtual void stop_pairing() = 0;

	/// The setting wifi.ps changed (web API, console): a driver with a power-save mode applies it to
	/// the running link. Default: nothing (the POSIX driver has no radio).
	virtual void power_save_changed() {}

	/// Fills the link's facts for the System page. False with the reason when the driver has none
	/// (the POSIX driver has no radio). Called on the event loop's thread.
	virtual bool link_details(LinkDetails& out, const char** reason) const {
		(void)out;
		*reason = "this network driver reports no link details";
		return false;
	}

protected:
	~NetworkDriver() = default;
};

} // namespace hc

#endif // HC_PLATFORM_NETWORK_DRIVER_H
