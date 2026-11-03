/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "wifi_driver.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wps.h"
#include "sdkconfig.h"
#include "hc/network_settings.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.wifi";

struct ReasonName {
	int32_t code;
	const char* name;
};

constexpr ReasonName kReasonNames[] = {
	{WIFI_REASON_UNSPECIFIED, "UNSPECIFIED"},
	{WIFI_REASON_AUTH_EXPIRE, "AUTH_EXPIRE"},
	{WIFI_REASON_AUTH_LEAVE, "AUTH_LEAVE"},
	{WIFI_REASON_ASSOC_EXPIRE, "ASSOC_EXPIRE"},
	{WIFI_REASON_ASSOC_TOOMANY, "ASSOC_TOOMANY"},
	{WIFI_REASON_NOT_AUTHED, "NOT_AUTHED"},
	{WIFI_REASON_NOT_ASSOCED, "NOT_ASSOCED"},
	{WIFI_REASON_ASSOC_LEAVE, "ASSOC_LEAVE"},
	{WIFI_REASON_ASSOC_NOT_AUTHED, "ASSOC_NOT_AUTHED"},
	{WIFI_REASON_MIC_FAILURE, "MIC_FAILURE"},
	{WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT, "4WAY_HANDSHAKE_TIMEOUT (wrong key?)"},
	{WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT, "GROUP_KEY_UPDATE_TIMEOUT"},
	{WIFI_REASON_802_1X_AUTH_FAILED, "802_1X_AUTH_FAILED"},
	{WIFI_REASON_STA_LEAVING, "STA_LEAVING"},
	{WIFI_REASON_TIMEOUT, "TIMEOUT"},
	{WIFI_REASON_BEACON_TIMEOUT, "BEACON_TIMEOUT"},
	{WIFI_REASON_NO_AP_FOUND, "NO_AP_FOUND"},
	{WIFI_REASON_AUTH_FAIL, "AUTH_FAIL"},
	{WIFI_REASON_ASSOC_FAIL, "ASSOC_FAIL"},
	{WIFI_REASON_HANDSHAKE_TIMEOUT, "HANDSHAKE_TIMEOUT (wrong key?)"},
	{WIFI_REASON_CONNECTION_FAIL, "CONNECTION_FAIL"},
	{WIFI_REASON_AP_TSF_RESET, "AP_TSF_RESET"},
	{WIFI_REASON_ROAMING, "ROAMING"},
	{WIFI_REASON_SA_QUERY_TIMEOUT, "SA_QUERY_TIMEOUT"},
	{WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY, "NO_AP_FOUND_W_COMPATIBLE_SECURITY"},
	{WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD, "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD"},
	{WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD, "NO_AP_FOUND_IN_RSSI_THRESHOLD"},
};

} // namespace

WifiDriver::WifiDriver(Settings& settings, LoopQueue& queue) : settings_(settings), queue_(queue) {}

const char* WifiDriver::reason_name(int32_t reason) {
	for (const ReasonName& entry : kReasonNames) {
		if (entry.code == reason) {
			return entry.name;
		}
	}
	return "unknown";
}

bool WifiDriver::init(Connectivity& connectivity) {
	connectivity_ = &connectivity;

	uint8_t mac[6] = {};
	esp_read_mac(mac, ESP_MAC_WIFI_STA);
	std::snprintf(hostname_, sizeof(hostname_), "homecontrol-%02x%02x%02x", mac[3], mac[4], mac[5]);

	esp_err_t err = esp_netif_init();
	if (err == ESP_OK) {
		err = esp_event_loop_create_default();
		if (err == ESP_ERR_INVALID_STATE) {
			err = ESP_OK; // Created by somebody else already.
		}
	}
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "cannot initialize the network stack: %s; WiFi disabled", esp_err_to_name(err));
		return false;
	}
	netif_ = esp_netif_create_default_wifi_sta();
	if (netif_ == nullptr) {
		ESP_LOGE(kTag, "cannot create the WiFi station interface; WiFi disabled");
		return false;
	}
	esp_netif_set_hostname(netif_, hostname_);

	wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
	err = esp_wifi_init(&config);
	if (err == ESP_OK) {
		// Credentials live in the settings, not in the WiFi driver's own NVS copy.
		err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
	}
	if (err == ESP_OK) {
		err = esp_wifi_set_mode(WIFI_MODE_STA);
	}
	if (err == ESP_OK) {
		err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, this, nullptr);
	}
	if (err == ESP_OK) {
		err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, this, nullptr);
	}
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "cannot initialize WiFi: %s; WiFi disabled", esp_err_to_name(err));
		return false;
	}
	initialized_ = true;
	ESP_LOGI(kTag, "WiFi station ready, host name %s", hostname_);
	return true;
}

bool WifiDriver::provisioned() const {
	char ssid[setting::kMaxSsidLength + 1];
	return settings_.get_string(setting::kWifiSsid, ssid, sizeof(ssid)) == SettingsStatus::ok && ssid[0] != '\0';
}

bool WifiDriver::rssi(int8_t& out) const {
	wifi_ap_record_t record;
	if (!associated_ || esp_wifi_sta_get_ap_info(&record) != ESP_OK) {
		return false;
	}
	out = record.rssi;
	return true;
}

bool WifiDriver::link_details(LinkDetails& out, const char** reason) const {
	if (!initialized_) {
		*reason = "WiFi is not initialized (see the log)";
		return false;
	}
	wifi_ap_record_t record;
	if (associated_ && esp_wifi_sta_get_ap_info(&record) == ESP_OK) {
		std::snprintf(out.ssid, sizeof(out.ssid), "%.32s", reinterpret_cast<const char*>(record.ssid));
		out.channel = record.primary;
		out.rssi = record.rssi;
		out.has_rssi = true;
	}
	int8_t power = 0;
	if (esp_wifi_get_max_tx_power(&power) == ESP_OK) {
		out.tx_power_dbm = power / 4.0; // the IDF counts quarter dBm
		out.has_tx_power = true;
	}
	wifi_ps_type_t ps;
	if (esp_wifi_get_ps(&ps) == ESP_OK) {
		out.power_save = ps == WIFI_PS_MIN_MODEM ? "min" : ps == WIFI_PS_MAX_MODEM ? "max" : "none";
	}
	uint8_t mac[6];
	if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
		std::snprintf(out.mac, sizeof(out.mac), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
			mac[4], mac[5]);
	}
	esp_netif_ip_info_t ip;
	if (has_ip_ && netif_ != nullptr && esp_netif_get_ip_info(netif_, &ip) == ESP_OK) {
		std::snprintf(out.gateway, sizeof(out.gateway), IPSTR, IP2STR(&ip.gw));
		std::snprintf(out.netmask, sizeof(out.netmask), IPSTR, IP2STR(&ip.netmask));
		esp_netif_dns_info_t dns;
		if (esp_netif_get_dns_info(netif_, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK && dns.ip.type == ESP_IPADDR_TYPE_V4) {
			std::snprintf(out.dns, sizeof(out.dns), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
		}
	}
	return true;
}

ConnectResult WifiDriver::connect() {
	if (!initialized_) {
		ESP_LOGE(kTag, "refusing connect: WiFi failed to initialize (see the error at boot)");
		return ConnectResult::refused;
	}
	char ssid[setting::kMaxSsidLength + 1];
	SettingsStatus status = settings_.get_string(setting::kWifiSsid, ssid, sizeof(ssid));
	if (status == SettingsStatus::not_found || (status == SettingsStatus::ok && ssid[0] == '\0')) {
		ESP_LOGW(kTag, "WiFi is not provisioned (no SSID stored): press the WPS button on the router, or type on "
			"the console: wifi set <ssid> <psk>");
		return ConnectResult::refused;
	}
	if (status != SettingsStatus::ok) {
		ESP_LOGE(kTag, "refusing connect: setting %s unreadable (%s); type: wifi set <ssid> <psk>",
			setting::kWifiSsid, settings_status_name(status));
		return ConnectResult::refused;
	}
	char psk[65];
	status = settings_.get_string(setting::kWifiPsk, psk, sizeof(psk));
	if (status == SettingsStatus::not_found) {
		psk[0] = '\0';
	} else if (status != SettingsStatus::ok) {
		ESP_LOGE(kTag, "refusing connect: setting %s unreadable (%s); type: wifi set <ssid> <psk>",
			setting::kWifiPsk, settings_status_name(status));
		return ConnectResult::refused;
	}

	bool open_network = psk[0] == '\0';
	if (wps_active_) {
		ESP_LOGE(kTag, "connect refused while WPS runs (the core stops pairing first)");
		return ConnectResult::failed;
	}

	wifi_config_t config = {};
	std::memcpy(config.sta.ssid, ssid, std::strlen(ssid)); // up to 32 bytes, NUL not required
	// up to 64 bytes (a 64-digit hex key fills the field without a NUL, as the IDF allows)
	std::memcpy(config.sta.password, psk, strnlen(psk, sizeof(config.sta.password)));
	config.sta.threshold.authmode = open_network ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
	config.sta.pmf_cfg.capable = true;
	config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
	// A full active scan finds a hidden SSID too (the default fast scan never does).
	config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
	std::memset(psk, 0, sizeof(psk));

	if (started_) {
		// Drop the current link or attempt first; its disconnect event is ours, not a failure.
		begin_disconnect();
	}
	esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
	std::memset(&config.sta.password, 0, sizeof(config.sta.password));
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "cannot configure the station: %s", esp_err_to_name(err));
		return ConnectResult::failed;
	}
	ESP_LOGI(kTag, "connecting to \"%s\"%s", ssid, open_network ? " (no key stored: open network)" : "");
	if (!started_) {
		connect_on_start_ = true;
		err = esp_wifi_start();
		if (err != ESP_OK) {
			connect_on_start_ = false;
			ESP_LOGE(kTag, "cannot start WiFi: %s", esp_err_to_name(err));
			return ConnectResult::failed;
		}
		started_ = true;
		apply_power_save();
		return ConnectResult::started; // esp_wifi_connect() follows on STA_START.
	}
	err = esp_wifi_connect();
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "esp_wifi_connect failed: %s", esp_err_to_name(err));
		return ConnectResult::failed;
	}
	return ConnectResult::started;
}

void WifiDriver::power_save_changed() {
	if (started_) {
		apply_power_save();
	}
}

void WifiDriver::apply_power_save() {
	// The setting wifi.ps (doc/network.md): minimum modem sleep is the default, because it gives the
	// 802.15.4 radio the air while WiFi sleeps (doc/zigbee.md, coexistence: with power save off a
	// Zigbee battery device lost its parent within seconds of joining); off is the phoneblock
	// dongle's choice (mains powered; modem sleep between beacons costs lost ACKs, which some access
	// points answer with a disassociation, reason 34) and stays a per-household choice.
	setting::WifiPowerSave mode = read_wifi_power_save(settings_, kTag);
	wifi_ps_type_t ps = mode == setting::WifiPowerSave::min ? WIFI_PS_MIN_MODEM
		: mode == setting::WifiPowerSave::max ? WIFI_PS_MAX_MODEM : WIFI_PS_NONE;
	esp_err_t err = esp_wifi_set_ps(ps);
	if (err != ESP_OK) {
		ESP_LOGW(kTag, "cannot set WiFi power save %s: %s", wifi_power_save_name(mode), esp_err_to_name(err));
	} else {
		ESP_LOGI(kTag, "WiFi power save %s", wifi_power_save_name(mode));
	}
	// The transmit power limit (quarter dBm) is a board decision: a weak supply browns out at the
	// driver's maximum of 20 dBm.
	err = esp_wifi_set_max_tx_power(CONFIG_HC_WIFI_MAX_TX_POWER);
	if (err != ESP_OK) {
		ESP_LOGW(kTag, "cannot set the WiFi transmit power limit: %s", esp_err_to_name(err));
	} else {
		int8_t power = 0;
		esp_wifi_get_max_tx_power(&power);
		ESP_LOGI(kTag, "WiFi transmit power limit %d.%02d dBm", power / 4, (power % 4) * 25);
	}
}

void WifiDriver::disconnect() {
	connect_on_start_ = false;
	if (started_) {
		begin_disconnect();
	}
}

void WifiDriver::begin_disconnect() {
	own_disconnect_pending_ = true;
	esp_err_t err = esp_wifi_disconnect();
	if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
		ESP_LOGW(kTag, "esp_wifi_disconnect: %s", esp_err_to_name(err));
	}
	associated_ = false;
}

// IDF event task: copy what is needed and hand over to the event loop.

void WifiDriver::on_wifi_event(void* arg, esp_event_base_t, int32_t id, void* data) {
	WifiDriver* self = static_cast<WifiDriver*>(arg);
	switch (id) {
	case WIFI_EVENT_STA_START:
		self->queue_.post(&handle_started, self, 0, nullptr, "WiFi STA_START");
		break;
	case WIFI_EVENT_STA_CONNECTED:
		self->queue_.post(&handle_associated, self,
			static_cast<const wifi_event_sta_connected_t*>(data)->channel, nullptr, "WiFi STA_CONNECTED");
		break;
	case WIFI_EVENT_STA_DISCONNECTED:
		self->queue_.post(&handle_disconnected, self,
			static_cast<const wifi_event_sta_disconnected_t*>(data)->reason, nullptr, "WiFi STA_DISCONNECTED");
		break;
	case WIFI_EVENT_STA_WPS_ER_SUCCESS: {
		const wifi_event_sta_wps_er_success_t* event = static_cast<const wifi_event_sta_wps_er_success_t*>(data);
		if (self->wps_mailbox_full_.load()) {
			ESP_LOGE(kTag, "WPS success while the previous one is unprocessed, dropped");
			break;
		}
		WpsCredentials& box = self->wps_mailbox_;
		std::memset(&box, 0, sizeof(box));
		box.from_event = event != nullptr && event->ap_cred_cnt > 0;
		if (box.from_event) {
			// Several access points may answer; the first one is taken (doc/network.md).
			std::memcpy(box.ssid, event->ap_cred[0].ssid, sizeof(event->ap_cred[0].ssid));
			std::memcpy(box.psk, event->ap_cred[0].passphrase, sizeof(event->ap_cred[0].passphrase));
		}
		self->wps_mailbox_full_.store(true);
		self->queue_.post(&handle_wps_success, self, 0, nullptr, "WPS success");
		break;
	}
	case WIFI_EVENT_SCAN_DONE: {
		const wifi_event_sta_scan_done_t* event = static_cast<const wifi_event_sta_scan_done_t*>(data);
		self->queue_.post(&handle_scan_done, self, event != nullptr ? static_cast<int32_t>(event->status) : 1, nullptr,
			"WiFi SCAN_DONE");
		break;
	}
	case WIFI_EVENT_STA_WPS_ER_FAILED: {
		int32_t reason = data != nullptr ? *static_cast<const wifi_event_sta_wps_fail_reason_t*>(data) : 0;
		self->queue_.post(&handle_wps_failed, self, reason, "failed", "WPS failed");
		break;
	}
	case WIFI_EVENT_STA_WPS_ER_TIMEOUT:
		self->queue_.post(&handle_wps_failed, self, 0, "timed out (no router in WPS mode found)", "WPS timeout");
		break;
	case WIFI_EVENT_STA_WPS_ER_PIN:
		self->queue_.post(&handle_wps_failed, self, 0, "the router asked for a PIN", "WPS PIN");
		break;
	case WIFI_EVENT_STA_WPS_ER_PBC_OVERLAP:
		self->queue_.post(&handle_wps_failed, self, 0, "two routers in WPS mode (overlap)", "WPS overlap");
		break;
	default:
		break;
	}
}

void WifiDriver::on_ip_event(void* arg, esp_event_base_t, int32_t id, void* data) {
	WifiDriver* self = static_cast<WifiDriver*>(arg);
	if (id == IP_EVENT_STA_GOT_IP) {
		const ip_event_got_ip_t* event = static_cast<const ip_event_got_ip_t*>(data);
		// lwIP's formatter, not printf: this runs on the IDF's sys_evt task (#48).
		char ip[16];
		esp_ip4addr_ntoa(&event->ip_info.ip, ip, sizeof(ip));
		self->queue_.post(&handle_got_ip, self, 0, ip, "IP GOT_IP");
	} else if (id == IP_EVENT_STA_LOST_IP) {
		self->queue_.post(&handle_lost_ip, self, 0, nullptr, "IP LOST_IP");
	}
}

// Event loop.

void WifiDriver::handle_started(void* context, const LoopMessage&) {
	WifiDriver* self = static_cast<WifiDriver*>(context);
	if (!self->connect_on_start_) {
		return;
	}
	self->connect_on_start_ = false;
	self->connectivity_->link_connecting();
	esp_err_t err = esp_wifi_connect();
	if (err != ESP_OK) {
		char reason[Connectivity::kMaxReason];
		std::snprintf(reason, sizeof(reason), "esp_wifi_connect failed: %s", esp_err_to_name(err));
		self->connectivity_->link_down(reason);
	}
}

void WifiDriver::handle_associated(void* context, const LoopMessage& message) {
	WifiDriver* self = static_cast<WifiDriver*>(context);
	self->associated_ = true;
	self->own_disconnect_pending_ = false;
	ESP_LOGI(kTag, "associated (channel %d), waiting for an address", static_cast<int>(message.number));
}

void WifiDriver::handle_disconnected(void* context, const LoopMessage& message) {
	WifiDriver* self = static_cast<WifiDriver*>(context);
	self->associated_ = false;
	self->has_ip_ = false;
	if (self->wps_active_) {
		// The WPS exchange left the access point; the core ignores the report as a failure.
		self->connectivity_->link_down("left the access point for the WPS exchange");
		return;
	}
	if (self->own_disconnect_pending_) {
		self->own_disconnect_pending_ = false;
		if (message.number == WIFI_REASON_ASSOC_LEAVE) {
			ESP_LOGI(kTag, "disconnected as requested");
			return;
		}
	}
	self->last_reason_ = message.number;
	char reason[Connectivity::kMaxReason];
	std::snprintf(reason, sizeof(reason), "wifi reason %d (%s)", static_cast<int>(message.number),
		reason_name(message.number));
	self->connectivity_->link_down(reason);
}

bool WifiDriver::ensure_started(const char* what) {
	if (!initialized_) {
		ESP_LOGE(kTag, "refusing %s: WiFi failed to initialize (see the error at boot)", what);
		return false;
	}
	if (!started_) {
		esp_err_t err = esp_wifi_start();
		if (err != ESP_OK) {
			ESP_LOGE(kTag, "refusing %s: cannot start WiFi: %s", what, esp_err_to_name(err));
			return false;
		}
		started_ = true;
		apply_power_save();
	}
	return true;
}

bool WifiDriver::start_scan(WifiScanSink& sink) {
	if (scan_sink_ != nullptr) {
		ESP_LOGW(kTag, "refusing scan: a scan is already running");
		return false;
	}
	if (wps_active_) {
		ESP_LOGE(kTag, "refusing scan while WPS runs (the core pauses pairing first)");
		return false;
	}
	if (!ensure_started("scan")) {
		return false;
	}
	esp_err_t err = esp_wifi_scan_start(nullptr, false);
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "WiFi scan not started: %s", esp_err_to_name(err));
		return false;
	}
	scan_sink_ = &sink;
	ESP_LOGI(kTag, "WiFi scan started");
	return true;
}

void WifiDriver::handle_scan_done(void* context, const LoopMessage& message) {
	WifiDriver* self = static_cast<WifiDriver*>(context);
	WifiScanSink* sink = self->scan_sink_;
	if (sink == nullptr) {
		// A scan nobody here asked for (WPS scans for itself): leave its records alone.
		return;
	}
	self->scan_sink_ = nullptr;
	if (message.number != 0) {
		esp_wifi_clear_ap_list();
		sink->scan_finished(false, "the WiFi driver reports the scan failed");
		return;
	}
	uint16_t count = 0;
	esp_wifi_scan_get_ap_num(&count);
	wifi_ap_record_t record;
	// Each call hands out (and frees) one record of the driver's list.
	while (esp_wifi_scan_get_ap_record(&record) == ESP_OK) {
		record.ssid[sizeof(record.ssid) - 1] = '\0';
		sink->scan_network(reinterpret_cast<const char*>(record.ssid), record.rssi, record.authmode != WIFI_AUTH_OPEN);
	}
	esp_wifi_clear_ap_list();
	ESP_LOGI(kTag, "WiFi scan done: %u access point(s)", static_cast<unsigned>(count));
	sink->scan_finished(true, "");
}

bool WifiDriver::start_pairing() {
	if (!ensure_started("WPS")) {
		return false;
	}
	esp_err_t err = ESP_OK;
	esp_wps_config_t config = WPS_CONFIG_INIT_DEFAULT(WPS_TYPE_PBC);
	err = esp_wifi_wps_enable(&config);
	if (err == ESP_OK) {
		err = esp_wifi_wps_start(0);
		if (err != ESP_OK) {
			esp_wifi_wps_disable();
		}
	}
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "WPS not started: %s", esp_err_to_name(err));
		return false;
	}
	wps_active_ = true;
	ESP_LOGI(kTag, "WPS push-button mode active: press the WPS button on the router");
	return true;
}

void WifiDriver::stop_pairing() {
	if (wps_active_) {
		// Also ends an association the WPS exchange made (the IDF disconnects it itself).
		esp_err_t err = esp_wifi_wps_disable();
		if (err != ESP_OK) {
			ESP_LOGW(kTag, "esp_wifi_wps_disable: %s", esp_err_to_name(err));
		}
		wps_active_ = false;
	}
	if (started_ && !has_ip_) {
		// Settle the station before the next step (the dongle's "STA is connecting" lesson); a
		// working link is left alone.
		begin_disconnect();
	}
}

void WifiDriver::handle_wps_success(void* context, const LoopMessage&) {
	WifiDriver* self = static_cast<WifiDriver*>(context);
	WpsCredentials& box = self->wps_mailbox_;
	if (!box.from_event) {
		// A single credential set: the IDF wrote it into the station config instead.
		wifi_config_t config = {};
		esp_wifi_get_config(WIFI_IF_STA, &config);
		std::memcpy(box.ssid, config.sta.ssid, sizeof(config.sta.ssid));
		std::memcpy(box.psk, config.sta.password, sizeof(config.sta.password));
		std::memset(&config.sta.password, 0, sizeof(config.sta.password));
	}
	box.ssid[sizeof(box.ssid) - 1] = '\0';
	box.psk[sizeof(box.psk) - 1] = '\0';
	const char* error = nullptr;
	bool stored = store_wifi_credentials(self->settings_, box.ssid, box.psk, &error);
	char detail[Connectivity::kMaxReason];
	if (stored) {
		ESP_LOGI(kTag, "WPS: credentials received for %s", box.ssid);
		std::snprintf(detail, sizeof(detail), "credentials received for %s", box.ssid);
	} else {
		ESP_LOGE(kTag, "WPS: credentials received for %s but refused: %s", box.ssid, error);
		std::snprintf(detail, sizeof(detail), "credentials refused: %s", error);
	}
	std::memset(&box, 0, sizeof(box));
	self->wps_mailbox_full_.store(false);
	if (stored) {
		self->connectivity_->pairing_succeeded(detail);
	} else {
		self->connectivity_->pairing_failed(detail);
	}
}

void WifiDriver::handle_wps_failed(void* context, const LoopMessage& message) {
	WifiDriver* self = static_cast<WifiDriver*>(context);
	char reason[Connectivity::kMaxReason];
	if (message.number != 0) {
		std::snprintf(reason, sizeof(reason), "WPS %s (reason %d)", message.text, static_cast<int>(message.number));
	} else {
		std::snprintf(reason, sizeof(reason), "WPS %s", message.text);
	}
	self->connectivity_->pairing_failed(reason);
}

void WifiDriver::handle_got_ip(void* context, const LoopMessage& message) {
	WifiDriver* self = static_cast<WifiDriver*>(context);
	self->has_ip_ = true;
	self->connectivity_->link_up(message.text);
}

void WifiDriver::handle_lost_ip(void* context, const LoopMessage&) {
	WifiDriver* self = static_cast<WifiDriver*>(context);
	self->has_ip_ = false;
	self->connectivity_->link_down("IP address lost");
}

} // namespace esp
} // namespace hc
