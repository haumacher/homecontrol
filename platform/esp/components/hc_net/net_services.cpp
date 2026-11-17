/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "net_services.h"

#include <sys/time.h>

#include <cstdio>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "hc/version.h"
#include "mdns.h"
#include "wifi_driver.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.netsvc";

/// The clock the SNTP callback marks; the callback has no context argument.
EspClock* g_sntp_clock = nullptr;

} // namespace

NetServices::NetServices(Settings& settings, EspClock& clock) : settings_(settings), clock_(clock) {}

bool NetServices::init(Connectivity& connectivity, WifiDriver& wifi) {
	wifi_ = &wifi;
	g_sntp_clock = &clock_;
	return connectivity.add_listener(this);
}

void NetServices::connectivity_changed(const Connectivity& connectivity, LinkState) {
	// The mDNS announcement follows the core's NetworkName (apply_name()), which listens itself.
	if (connectivity.is_up() && !sntp_running_) {
		start_sntp();
	}
}

// ---- mDNS -------------------------------------------------------------------------------------

static_assert(MDNS_NAME_BUF_LEN <= NetServices::kMaxAnnounced, "the responder's names must fit announced_");

bool NetServices::ensure_mdns(const char** reason) {
	if (mdns_initialized_) {
		return true;
	}
	esp_err_t err = mdns_init();
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "mDNS not started: %s", esp_err_to_name(err));
		*reason = "mDNS cannot start (see the log)";
		return false;
	}
	mdns_initialized_ = true;
	err = mdns_register_hostname_changed_callback(&on_hostname_changed, this);
	if (err != ESP_OK) {
		ESP_LOGW(kTag, "mDNS renames are not logged: %s", esp_err_to_name(err));
	}
	return true;
}

void NetServices::on_hostname_changed(const char* hostname, void*) {
	// The mDNS task, during probing (no mDNS calls here): another host defended the name.
	ESP_LOGW(kTag, "mDNS: the name was taken on the network, the responder announces %s.local now",
		hostname != nullptr ? hostname : "(none)");
}

bool NetServices::add_services(const char** reason) {
	mdns_txt_item_t txt[] = {
		{"version", kVersion},
		{"build", build_id()},
	};
	esp_err_t err = mdns_service_add(nullptr, kServiceType, kServiceProtocol, kServicePort, txt,
		sizeof(txt) / sizeof(txt[0]));
	if (err == ESP_OK) {
		// The web UI as a standard HTTP service, so that routers list a link to the device (the
		// FRITZ!Box shows the web interface of a device that announces _http._tcp, as the phoneblock
		// dongle does); the path TXT names the page to open.
		mdns_txt_item_t http_txt[] = {{"path", "/"}};
		err = mdns_service_add(nullptr, "_http", "_tcp", kServicePort, http_txt, 1);
	}
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "mDNS services not announced: %s", esp_err_to_name(err));
		mdns_service_remove_all();
		*reason = "the mDNS services cannot be added (see the log)";
		return false;
	}
	return true;
}

bool NetServices::apply_name(const char* name, const char** reason) {
	if (!ensure_mdns(reason)) {
		return false;
	}
	esp_err_t err = mdns_hostname_set(name);
	if (err == ESP_OK) {
		// The instance name is the network name as well: E-Home's controller search lists the
		// instances it finds (haumacher/ehome #42), so it must be unique on the LAN like the host name.
		err = mdns_instance_name_set(name);
	}
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "mDNS name %s not set: %s", name, esp_err_to_name(err));
		*reason = "the mDNS responder refused the name (see the log)";
		return false;
	}
	if (!mdns_running_) {
		if (!add_services(reason)) {
			return false;
		}
		mdns_running_ = true;
		ESP_LOGI(kTag, "mDNS: %s.local, services %s.%s and _http._tcp port %u, instance %s, TXT version=%s build=%s",
			name, kServiceType, kServiceProtocol, static_cast<unsigned>(kServicePort), name, kVersion, build_id());
	} else {
		ESP_LOGI(kTag, "mDNS: now %s.local (instance %s)", name, name);
	}
	if (wifi_ != nullptr) {
		wifi_->set_dhcp_hostname(name);
	}
	return true;
}

const char* NetServices::announced_name() const {
	if (!mdns_running_ || mdns_hostname_get(announced_) != ESP_OK) {
		announced_[0] = '\0';
	}
	return announced_;
}

bool NetServices::collect_abandoned() {
	if (search_ == nullptr) {
		return true;
	}
	if (!abandoned_) {
		return false;
	}
	mdns_result_t* results = nullptr;
	if (!mdns_query_async_get_results(search_, 0, &results, nullptr)) {
		return false;
	}
	mdns_query_results_free(results);
	mdns_query_async_delete(search_);
	search_ = nullptr;
	abandoned_ = false;
	return true;
}

bool NetServices::start_lookup(const char* name, uint32_t timeout_ms, const char** reason) {
	if (!collect_abandoned()) {
		*reason = "a lookup is still running";
		return false;
	}
	if (!ensure_mdns(reason)) {
		return false;
	}
	// The A record of <name>.local: a host that uses the name answers for it. One answer is enough.
	search_ = mdns_query_async_new(name, nullptr, nullptr, MDNS_TYPE_A, timeout_ms, 1, nullptr);
	if (search_ == nullptr) {
		*reason = "the mDNS query cannot start (out of memory?)";
		return false;
	}
	std::snprintf(lookup_name_, sizeof(lookup_name_), "%s", name);
	ESP_LOGI(kTag, "mDNS: asking whether a host answers for %s.local (%u ms)", name, static_cast<unsigned>(timeout_ms));
	return true;
}

NameLookup NetServices::poll_lookup() {
	if (search_ == nullptr || abandoned_) {
		return NameLookup::failed;
	}
	mdns_result_t* results = nullptr;
	uint8_t count = 0;
	if (!mdns_query_async_get_results(search_, 0, &results, &count)) {
		return NameLookup::running;
	}
	bool taken = results != nullptr;
	if (taken) {
		char address[16] = "?";
		for (const mdns_ip_addr_t* a = results->addr; a != nullptr; a = a->next) {
			if (a->addr.type == ESP_IPADDR_TYPE_V4) {
				esp_ip4addr_ntoa(&a->addr.u_addr.ip4, address, sizeof(address));
				break;
			}
		}
		ESP_LOGI(kTag, "mDNS: %s.local is answered by %s", lookup_name_, address);
	} else {
		ESP_LOGI(kTag, "mDNS: no host answers for %s.local", lookup_name_);
	}
	mdns_query_results_free(results);
	mdns_query_async_delete(search_);
	search_ = nullptr;
	return taken ? NameLookup::taken : NameLookup::free;
}

void NetServices::cancel_lookup() {
	// A running search cannot be deleted (ESP_ERR_INVALID_STATE): it is freed once it ends, at the
	// next start_lookup().
	if (search_ != nullptr) {
		abandoned_ = true;
		collect_abandoned();
	}
}

void NetServices::start_sntp() {
	read_string_setting(settings_, setting::kNtpServer, setting::kDefaultNtpServer, ntp_server_,
		sizeof(ntp_server_), kTag, &host_name_valid);
	esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(ntp_server_);
	config.sync_cb = &on_time_sync;
	esp_err_t err = esp_netif_sntp_init(&config);
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "SNTP not started: %s (retried at the next network up)", esp_err_to_name(err));
		return;
	}
	sntp_running_ = true;
	ESP_LOGI(kTag, "SNTP started against %s", ntp_server_);
}

void NetServices::restart_sntp() {
	if (!sntp_running_) {
		return; // Starts with the new server when the network comes up.
	}
	esp_netif_sntp_deinit();
	sntp_running_ = false;
	start_sntp();
}

void NetServices::on_time_sync(struct timeval*) {
	// lwIP task: only the atomic flag; the Controller's clock watch logs the local time.
	if (g_sntp_clock != nullptr) {
		g_sntp_clock->set_synchronized();
	}
}

} // namespace esp
} // namespace hc
