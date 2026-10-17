/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_NETWORK_DRIVER_H
#define HC_PLATFORM_NETWORK_DRIVER_H

namespace hc {

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

protected:
	~NetworkDriver() = default;
};

} // namespace hc

#endif // HC_PLATFORM_NETWORK_DRIVER_H
