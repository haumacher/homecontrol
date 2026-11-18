/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_NET_SERVICES_H
#define HC_ESP_NET_SERVICES_H

#include "esp_platform.h"
#include "hc/connectivity.h"
#include "hc/network_settings.h"
#include "hc/platform/name_service.h"
#include "hc/platform/settings.h"

struct mdns_search_once_s;

namespace hc {
namespace esp {

class WifiDriver;

/// The network services of the platform: the mDNS announcement and SNTP.
///
/// mDNS is the hc::NameService of the core's NetworkName (doc/network.md "Network name"): the core
/// decides which name to announce; apply_name() starts the responder with its first call (host
/// name and instance name = the network name, service _homecontrol._tcp on port 80 with TXT version
/// and build, _http._tcp with path=/) and renames it afterwards, and sets the WiFi station's DHCP
/// host name. A lookup is an asynchronous mDNS query for the A record of `<name>.local`
/// (mdns_query_async_new, one result is enough), polled on the loop. The responder renames a name
/// another host defends while probing (`name-2`, ...; logged from the mDNS task), announced_name()
/// reads it (mdns_hostname_get).
///
/// SNTP starts when the network first comes up, against the setting ntp.server (default
/// pool.ntp.org); its first delivery marks the EspClock synchronized. Both keep running across later
/// link losses (they follow the interface by themselves). Runs on the event loop.
class NetServices : public ConnectivityListener, public NameService {
public:
	static constexpr const char* kServiceType = "_homecontrol";
	static constexpr const char* kServiceProtocol = "_tcp";
	static constexpr uint16_t kServicePort = 80;
	/// The responder's name buffer (MDNS_NAME_BUF_LEN of the mDNS component).
	static constexpr size_t kMaxAnnounced = 65;
	/// The answers a lookup collects (the controller's own echo and another host's).
	static constexpr size_t kMaxLookupAnswers = 4;

	NetServices(Settings& settings, EspClock& clock);

	/// Registers with the connectivity (SNTP); `wifi` takes the DHCP host name. `wifi` must outlive
	/// this object.
	bool init(Connectivity& connectivity, WifiDriver& wifi);

	void connectivity_changed(const Connectivity& connectivity, LinkState previous) override;

	// NameService
	bool start_lookup(const char* name, const char* own_address, uint32_t timeout_ms, const char** reason) override;
	NameLookup poll_lookup() override;
	void cancel_lookup() override;
	bool apply_name(const char* name, const char** reason) override;
	const char* announced_name() const override;

	/// Re-reads ntp.server and restarts SNTP if it runs (after `ntp set`).
	void restart_sntp();

	bool sntp_running() const { return sntp_running_; }
	/// Whether the mDNS announcement is up (the first apply_name() succeeded).
	bool mdns_running() const { return mdns_running_; }
	const char* ntp_server() const { return ntp_server_; }

private:
	bool ensure_mdns(const char** reason);
	bool add_services(const char** reason);
	/// Frees an abandoned lookup that has finished meanwhile; false while it still runs.
	bool collect_abandoned();
	void start_sntp();
	static void on_time_sync(struct timeval* tv);
	static void on_hostname_changed(const char* hostname, void* arg);

	Settings& settings_;
	EspClock& clock_;
	WifiDriver* wifi_ = nullptr;
	char ntp_server_[Settings::kMaxStringLength + 1] = {};
	mutable char announced_[kMaxAnnounced] = {};
	/// The name of the last apply_name(): the rename callback tells its own change from probing's.
	char requested_[kMaxAnnounced] = {};
	/// The running (or abandoned, not yet finished) lookup.
	mdns_search_once_s* search_ = nullptr;
	char lookup_name_[setting::kMaxNetHostnameLength + 1] = {};
	/// The controller's own address of the running lookup: an answer with it is the controller itself.
	char lookup_own_[16] = {};
	bool abandoned_ = false;
	bool mdns_initialized_ = false;
	bool mdns_running_ = false;
	bool sntp_running_ = false;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_NET_SERVICES_H
