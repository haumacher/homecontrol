/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_NET_SERVICES_H
#define HC_ESP_NET_SERVICES_H

#include "esp_platform.h"
#include "hc/connectivity.h"
#include "hc/network_settings.h"
#include "hc/platform/settings.h"

namespace hc {
namespace esp {

/// The services that start when the network first comes up: the mDNS announcement
/// (<hostname>.local, service _homecontrol._tcp on port 80 with TXT version and build) and SNTP
/// against the setting ntp.server (default pool.ntp.org), whose first delivery marks the
/// EspClock synchronized. Both keep running across later link losses (they follow the interface
/// by themselves). Runs on the event loop.
class NetServices : public ConnectivityListener {
public:
	static constexpr const char* kServiceType = "_homecontrol";
	static constexpr const char* kServiceProtocol = "_tcp";
	static constexpr uint16_t kServicePort = 80;

	NetServices(Settings& settings, EspClock& clock);

	/// Registers with the connectivity. `hostname` must outlive this object.
	bool init(Connectivity& connectivity, const char* hostname);

	void connectivity_changed(const Connectivity& connectivity, LinkState previous) override;

	/// Re-reads ntp.server and restarts SNTP if it runs (after `ntp set`).
	void restart_sntp();

	bool sntp_running() const { return sntp_running_; }
	bool mdns_running() const { return mdns_running_; }
	const char* ntp_server() const { return ntp_server_; }

private:
	void start_mdns();
	void start_sntp();
	static void on_time_sync(struct timeval* tv);

	Settings& settings_;
	EspClock& clock_;
	const char* hostname_ = "";
	char ntp_server_[Settings::kMaxStringLength + 1] = {};
	bool mdns_running_ = false;
	bool sntp_running_ = false;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_NET_SERVICES_H
