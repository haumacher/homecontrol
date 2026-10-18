/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "net_services.h"

#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "hc/version.h"
#include "mdns.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.netsvc";

/// The clock the SNTP callback marks; the callback has no context argument.
EspClock* g_sntp_clock = nullptr;

} // namespace

NetServices::NetServices(Settings& settings, EspClock& clock) : settings_(settings), clock_(clock) {}

bool NetServices::init(Connectivity& connectivity, const char* hostname) {
	hostname_ = hostname;
	g_sntp_clock = &clock_;
	return connectivity.add_listener(this);
}

void NetServices::connectivity_changed(const Connectivity& connectivity, LinkState) {
	if (!connectivity.is_up()) {
		return;
	}
	if (!mdns_running_) {
		start_mdns();
	}
	if (!sntp_running_) {
		start_sntp();
	}
}

void NetServices::start_mdns() {
	esp_err_t err = mdns_init();
	if (err == ESP_OK) {
		err = mdns_hostname_set(hostname_);
	}
	if (err == ESP_OK) {
		// The instance name must be unique per device on the LAN (E-Home lists every controller it
		// finds, haumacher/ehome #42): the host label carries the MAC's last three bytes.
		err = mdns_instance_name_set(hostname_);
	}
	mdns_txt_item_t txt[] = {
		{"version", kVersion},
		{"build", build_id()},
	};
	if (err == ESP_OK) {
		err = mdns_service_add(nullptr, kServiceType, kServiceProtocol, kServicePort, txt,
			sizeof(txt) / sizeof(txt[0]));
	}
	if (err == ESP_OK) {
		// The web UI as a standard HTTP service, so that routers list a link to the device (the
		// FRITZ!Box shows the web interface of a device that announces _http._tcp, as the phoneblock
		// dongle does); the path TXT names the page to open.
		mdns_txt_item_t http_txt[] = {{"path", "/"}};
		err = mdns_service_add(nullptr, "_http", "_tcp", kServicePort, http_txt, 1);
	}
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "mDNS not announced: %s (retried at the next network up)", esp_err_to_name(err));
		mdns_free();
		return;
	}
	mdns_running_ = true;
	ESP_LOGI(kTag, "mDNS: %s.local, services %s.%s and _http._tcp port %u, instance %s, TXT version=%s build=%s",
		hostname_, kServiceType, kServiceProtocol, static_cast<unsigned>(kServicePort), hostname_, kVersion, build_id());
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
