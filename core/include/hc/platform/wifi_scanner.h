/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_WIFI_SCANNER_H
#define HC_PLATFORM_WIFI_SCANNER_H

#include <cstdint>

namespace hc {

/// Receives the results of a WiFi scan, on the event loop's thread.
class WifiScanSink {
public:
	/// One access point seen; `secured` is false for an open network. The same SSID may be
	/// reported several times (one per access point of a mesh).
	virtual void scan_network(const char* ssid, int8_t rssi, bool secured) = 0;

	/// The scan is over (`ok`) or failed for `reason`. Exactly one call per accepted scan.
	virtual void scan_finished(bool ok, const char* reason) = 0;

protected:
	~WifiScanSink() = default;
};

/// The platform's WiFi scan (the ESP station). The caller makes sure the station is free (the
/// driver refuses to scan while WPS or an attempt owns it: Connectivity::pause_pairing()).
class WifiScanner {
public:
	/// Starts a scan whose results go to `sink` - always on the event loop's thread and never from
	/// within this call. Returns false (logged) if the scan cannot start; `sink` is then not called.
	virtual bool start_scan(WifiScanSink& sink) = 0;

protected:
	~WifiScanner() = default;
};

} // namespace hc

#endif // HC_PLATFORM_WIFI_SCANNER_H
