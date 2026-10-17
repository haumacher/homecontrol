/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_IMPROV_H
#define HC_IMPROV_H

#include <cstddef>
#include <cstdint>

#include "hc/connectivity.h"
#include "hc/event_loop.h"
#include "hc/improv_protocol.h"
#include "hc/network_settings.h"
#include "hc/platform/clock.h"
#include "hc/platform/improv_port.h"
#include "hc/platform/settings.h"
#include "hc/platform/wifi_scanner.h"

namespace hc {

/// The Improv Wi-Fi serial service (ported from the phoneblock dongle's improv.c): lets a browser
/// installer (ESP Web Tools) hand the device its WiFi credentials over the serial port the
/// firmware was just flashed through. The platform delivers parsed frames (handle_frame(),
/// bad_frame()); this class answers through the ImprovPort:
///
/// - `wifi settings`: stores the credentials with store_wifi_credentials() (the one way every
///   provisioning path stores them) and restarts the connection workflow
///   (Connectivity::request_connect()). The state is `provisioning` (the status LED shows pairing)
///   until the link comes up - then `provisioned` and the RPC result carries the device URL
///   `http://<device name>.local/` - or until kProvisionFailures attempts failed or
///   kProvisionTimeoutMs passed: error `unable to connect`. The credentials stay stored either way
///   (a corrected retry overwrites them; WPS and the failsafe remain).
/// - `get current state`: the state; when provisioned also the URL as an RPC result.
/// - `get device info`: "HomeControl", "<version> (<build>)", the chip family, the device name.
/// - `get wifi networks`: pauses pairing (Connectivity::pause_pairing(): the driver refuses to
///   scan while WPS owns the station), scans, resumes, and streams one result per SSID (hidden
///   networks left out, the strongest access point of a mesh kept, strongest first), then the
///   empty result that ends the list.
///
/// Every RPC first clears the error state (error `none`); a bad checksum or malformed RPC is
/// answered with `invalid RPC`, an unknown command with `unknown RPC`. Nothing is sent before a
/// client has sent a frame: on a port nobody uses for Improv the service is silent.
///
/// Lifetime: Improv is active from start() for kWindowMs, and ends earlier kGraceMs after the link
/// first came up - but never sooner than kGraceMs after the last frame from a client or the outcome
/// of a `wifi settings` exchange (which therefore always completes). Then it logs that it ended, tells the platform
/// (ImprovPort::improv_ended()) and ignores further frames: the port belongs to the console.
///
/// Threading: every method is called on the event loop's thread.
class Improv : public ConnectivityListener, public WifiScanSink {
public:
	/// How long Improv is available after boot at most (absent client activity).
	static constexpr uint32_t kWindowMs = 5 * 60 * 1000;

	/// How long Improv stays available after the link first came up, and after a client's frame.
	static constexpr uint32_t kGraceMs = 60 * 1000;

	/// A `wifi settings` exchange without a link after this long fails (ESP Web Tools waits 30 s).
	static constexpr uint32_t kProvisionTimeoutMs = 25 * 1000;

	/// Failed attempts with the new credentials that end a `wifi settings` exchange early.
	static constexpr uint32_t kProvisionFailures = 3;

	/// A scan without a result after this long is given up (an empty list is sent).
	static constexpr uint32_t kScanTimeoutMs = 15 * 1000;

	// A `wifi settings` exchange always ends before Improv does: its frame keeps Improv for kGraceMs.
	static_assert(kGraceMs > kProvisionTimeoutMs, "Improv must outlive every wifi settings exchange");

	// A scan, too, is over before Improv ends (its frame keeps Improv for kGraceMs).
	static_assert(kGraceMs > kScanTimeoutMs + Connectivity::kSettleMs, "Improv must outlive every scan");

	/// Networks kept from one scan (the strongest ones).
	static constexpr size_t kMaxNetworks = 20;

	/// Firmware name reported in the device info, also the web flasher manifest's name.
	static constexpr const char* kFirmwareName = "HomeControl";

	/// Longest device URL including the NUL.
	static constexpr size_t kMaxUrl = 64;

	/// `port` null: the platform has no Improv port; the service stays off. `scanner` null: network
	/// lists are answered empty.
	Improv(EventLoop& loop, const Clock& clock, Settings& settings, Connectivity& connectivity, ImprovPort* port,
		WifiScanner* scanner);
	~Improv();

	Improv(const Improv&) = delete;
	Improv& operator=(const Improv&) = delete;

	/// Opens the window (logged). Registers with the Connectivity. A second call is refused.
	void start();

	/// A complete frame with a valid checksum, from the platform's reader.
	void handle_frame(uint8_t type, const uint8_t* data, size_t length);

	/// A frame the reader dropped (bad checksum, incomplete), for `reason`.
	void bad_frame(const char* reason);

	/// Whether frames are accepted.
	bool active() const { return active_; }

	/// Whether a `wifi settings` exchange is waiting for the link (the status LED shows pairing).
	bool provisioning() const { return provisioning_; }

	/// Monotonic time at which Improv ends (EventLoop::kNever when it is not active).
	uint64_t ends_at() const { return active_ ? end_at_ : EventLoop::kNever; }

	improv::State state() const;

	// ConnectivityListener
	void connectivity_changed(const Connectivity& connectivity, LinkState previous) override;

	// WifiScanSink
	void scan_network(const char* ssid, int8_t rssi, bool secured) override;
	void scan_finished(bool ok, const char* reason) override;

private:
	struct Network {
		char ssid[setting::kMaxSsidLength + 1];
		int8_t rssi;
		bool secured;
	};

	void handle_rpc(const uint8_t* data, size_t length);
	void rpc_wifi_settings(const uint8_t* payload, size_t length);
	void rpc_get_state();
	void rpc_get_info();
	void rpc_get_networks();
	void provisioning_done(bool success, const char* why);
	void provision_timeout_due();
	void scan_start_due();
	void scan_timeout_due();
	void finish_scan(bool ok, const char* reason);
	void send_state();
	void send_error(improv::Error error);
	void send_rpc_result(uint8_t command, const char* const* strings, size_t count);
	void send(size_t length);
	void format_url(char* buffer, size_t size) const;
	void set_end(uint64_t at, const char* reason);
	void end_due();
	void end(const char* reason);
	void cancel(TimerHandle& timer);

	EventLoop& loop_;
	const Clock& clock_;
	Settings& settings_;
	Connectivity& connectivity_;
	ImprovPort* port_;
	WifiScanner* scanner_;
	uint8_t frame_[improv::kMaxFrame] = {};
	Network networks_[kMaxNetworks] = {};
	size_t network_count_ = 0;
	TimerHandle end_timer_;
	TimerHandle provision_timer_;
	TimerHandle scan_timer_;
	TimerHandle scan_timeout_;
	uint64_t end_at_ = 0;
	const char* end_reason_ = "";
	bool started_ = false;
	bool active_ = false;
	bool link_was_up_ = false;
	bool provisioning_ = false;
	bool scanning_ = false;
};

} // namespace hc

#endif // HC_IMPROV_H
