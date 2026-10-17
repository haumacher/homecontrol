/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_WIFI_DRIVER_H
#define HC_ESP_WIFI_DRIVER_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "esp_event.h"
#include "esp_netif.h"
#include "hc/connectivity.h"
#include "hc/platform/network_driver.h"
#include "hc/platform/settings.h"
#include "hc/platform/wifi_scanner.h"
#include "loop_queue.h"

namespace hc {
namespace esp {

/// The WiFi station on esp_wifi, with the credentials from the settings wifi.ssid and wifi.psk.
/// The core's Connectivity decides when to connect; this driver performs the attempt and feeds
/// the IDF events back: STA_START -> link_connecting(), GOT_IP -> link_up(ip), DISCONNECTED ->
/// link_down("wifi reason <code> (<name>)"), LOST_IP -> link_down. The IDF event handlers run in
/// the IDF event task and only post to the LoopQueue; everything else runs on the event loop.
///
/// Without a stored SSID connect() refuses and logs what to type on the console. The key is never
/// logged.
///
/// Pairing is WPS push-button (esp_wps, WPS_TYPE_PBC). The scan for a router in WPS mode does not
/// touch the current link; only when the router's button has been pressed does the IDF leave the
/// access point for the few-second WPS exchange. Received credentials (the first set if several
/// access points answer) are stored with hc::store_wifi_credentials(), exactly as `wifi set`
/// stores them - the Settings stay authoritative, the IDF keeps nothing (WIFI_STORAGE_RAM) - and
/// reported as pairing_succeeded(); failures, timeouts, PIN requests and PBC overlaps as
/// pairing_failed(). When to pair, restart or fall back is the core's decision (Connectivity).
///
/// The scan for Improv (WifiScanner) is an asynchronous esp_wifi_scan_start(); SCAN_DONE hands the
/// records to the sink on the event loop, one esp_wifi_scan_get_ap_record() at a time (no array).
/// It is refused while WPS runs: the core pauses pairing first (Connectivity::pause_pairing()).
class WifiDriver : public NetworkDriver, public WifiScanner {
public:
	/// Longest host name including the NUL ("homecontrol-" + 6 hex digits).
	static constexpr size_t kMaxHostname = 24;

	WifiDriver(Settings& settings, LoopQueue& queue);

	/// Creates the station interface, initializes esp_wifi and registers the event handlers. Must
	/// run before the Controller starts (it calls connect()). Logs and returns false on failure;
	/// connect() then refuses.
	bool init(Connectivity& connectivity);

	bool provisioned() const override;
	ConnectResult connect() override;
	void disconnect() override;
	bool start_pairing() override;
	void stop_pairing() override;

	bool start_scan(WifiScanSink& sink) override;

	/// "homecontrol-<last three MAC bytes in lower-case hex>", also the DHCP host name.
	const char* hostname() const { return hostname_; }

	/// RSSI of the access point in dBm while associated; false otherwise.
	bool rssi(int8_t& out) const;

	/// The WiFi reason code of the last disconnect (0: none yet).
	int32_t last_reason() const { return last_reason_; }

	/// Name of a WiFi disconnect reason code ("NO_AP_FOUND"), "unknown" if not known.
	static const char* reason_name(int32_t reason);

private:
	static void on_wifi_event(void* arg, esp_event_base_t base, int32_t id, void* data);
	static void on_ip_event(void* arg, esp_event_base_t base, int32_t id, void* data);

	static void handle_started(void* context, const LoopMessage& message);
	static void handle_associated(void* context, const LoopMessage& message);
	static void handle_disconnected(void* context, const LoopMessage& message);
	static void handle_got_ip(void* context, const LoopMessage& message);
	static void handle_lost_ip(void* context, const LoopMessage& message);

	static void handle_wps_success(void* context, const LoopMessage& message);
	static void handle_wps_failed(void* context, const LoopMessage& message);
	static void handle_scan_done(void* context, const LoopMessage& message);

	void begin_disconnect();
	void disable_power_save();
	bool ensure_started(const char* what);

	/// Credentials of a WPS success, handed from the IDF event task to the loop. Written only while
	/// `wps_mailbox_full_` is false, read only while it is true.
	struct WpsCredentials {
		char ssid[33];
		char psk[65];
		bool from_event; ///< false: the IDF put them into the station config instead (ap_cred_cnt 0)
	};

	Settings& settings_;
	LoopQueue& queue_;
	Connectivity* connectivity_ = nullptr;
	/// The sink of the scan in progress; nullptr when none runs.
	WifiScanSink* scan_sink_ = nullptr;
	esp_netif_t* netif_ = nullptr;
	char hostname_[kMaxHostname] = {};
	int32_t last_reason_ = 0;
	WpsCredentials wps_mailbox_ = {};
	std::atomic<bool> wps_mailbox_full_{false};
	bool wps_active_ = false;
	bool initialized_ = false;
	bool started_ = false;
	bool connect_on_start_ = false;
	bool associated_ = false;
	bool has_ip_ = false;
	/// esp_wifi_disconnect() was called: the next ASSOC_LEAVE disconnect is ours, not a failure.
	bool own_disconnect_pending_ = false;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_WIFI_DRIVER_H
