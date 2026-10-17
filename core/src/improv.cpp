/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/improv.h"

#include <cstdio>
#include <cstring>

#include "hc/log.h"
#include "hc/version.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.improv";

} // namespace

Improv::Improv(EventLoop& loop, const Clock& clock, Settings& settings, Connectivity& connectivity, ImprovPort* port,
	WifiScanner* scanner)
	: loop_(loop), clock_(clock), settings_(settings), connectivity_(connectivity), port_(port), scanner_(scanner) {}

Improv::~Improv() {
	cancel(end_timer_);
	cancel(provision_timer_);
	cancel(scan_timer_);
	cancel(scan_timeout_);
}

void Improv::start() {
	if (started_) {
		Log::error(kTag, "refusing start: Improv already started");
		return;
	}
	started_ = true;
	if (port_ == nullptr) {
		Log::info(kTag, "Improv Wi-Fi serial not offered: no serial port for it on this platform");
		return;
	}
	if (!connectivity_.add_listener(this)) {
		Log::error(kTag, "Improv Wi-Fi serial not offered: cannot observe the network");
		return;
	}
	active_ = true;
	set_end(clock_.monotonic_ms() + kWindowMs, "the window after boot is over");
	Log::info(kTag, "Improv Wi-Fi serial active for %u s (ends %u s after the network is first up)",
		static_cast<unsigned>(kWindowMs / 1000), static_cast<unsigned>(kGraceMs / 1000));
	if (connectivity_.is_up()) {
		connectivity_changed(connectivity_, LinkState::down);
	}
}

improv::State Improv::state() const {
	if (provisioning_) {
		return improv::State::provisioning;
	}
	return connectivity_.is_up() ? improv::State::provisioned : improv::State::authorized;
}

// ---- Frames -----------------------------------------------------------------------------------

void Improv::handle_frame(uint8_t type, const uint8_t* data, size_t length) {
	if (!active_) {
		Log::info(kTag, "Improv frame ignored: Improv is %s", started_ ? "over" : "not started");
		return;
	}
	uint64_t keep_until = clock_.monotonic_ms() + kGraceMs;
	if (end_at_ < keep_until) {
		set_end(keep_until, "no client frame for the grace period");
	}
	if (type != improv::kTypeRpcCommand) {
		Log::warn(kTag, "Improv frame of type 0x%02x refused: a client sends RPC commands only",
			static_cast<unsigned>(type));
		send_error(improv::Error::invalid_rpc);
		return;
	}
	handle_rpc(data, length);
}

void Improv::bad_frame(const char* reason) {
	if (!active_) {
		Log::info(kTag, "bad Improv frame (%s) ignored: Improv is %s", reason, started_ ? "over" : "not started");
		return;
	}
	uint64_t keep_until = clock_.monotonic_ms() + kGraceMs;
	if (end_at_ < keep_until) {
		set_end(keep_until, "no client frame for the grace period");
	}
	Log::warn(kTag, "Improv frame refused: %s", reason);
	send_error(improv::Error::invalid_rpc);
}

void Improv::handle_rpc(const uint8_t* data, size_t length) {
	// RPC: [command, payload length, payload]
	if (length < 2 || data[1] != length - 2) {
		Log::warn(kTag, "Improv RPC refused: malformed (frame data %u bytes, RPC declares %u)",
			static_cast<unsigned>(length), length < 2 ? 0u : static_cast<unsigned>(data[1]));
		send_error(improv::Error::invalid_rpc);
		return;
	}
	// The spec: an RPC command resets the error state.
	send_error(improv::Error::none);
	switch (data[0]) {
	case improv::kRpcWifiSettings:
		rpc_wifi_settings(data + 2, length - 2);
		break;
	case improv::kRpcGetCurrentState:
		rpc_get_state();
		break;
	case improv::kRpcGetDeviceInfo:
		rpc_get_info();
		break;
	case improv::kRpcGetWifiNetworks:
		rpc_get_networks();
		break;
	default:
		Log::warn(kTag, "Improv RPC 0x%02x refused: unknown command", static_cast<unsigned>(data[0]));
		send_error(improv::Error::unknown_rpc);
		break;
	}
}

// ---- RPCs -------------------------------------------------------------------------------------

void Improv::rpc_wifi_settings(const uint8_t* payload, size_t length) {
	char ssid[setting::kMaxSsidLength + 1];
	char psk[65];
	if (!improv::parse_wifi_settings(payload, length, ssid, sizeof(ssid), psk, sizeof(psk))) {
		Log::warn(kTag, "Improv wifi settings refused: malformed (lengths do not add up, empty SSID, or a value "
			"too long)");
		send_error(improv::Error::invalid_rpc);
		return;
	}
	const char* error = nullptr;
	bool stored = store_wifi_credentials(settings_, ssid, psk[0] != '\0' ? psk : nullptr, &error);
	std::memset(psk, 0, sizeof(psk));
	if (!stored) {
		Log::warn(kTag, "Improv: credentials for %s refused: %s", ssid, error);
		send_error(improv::Error::unable_to_connect);
		return;
	}
	Log::info(kTag, "Improv: credentials received for %s, connecting", ssid);
	provisioning_ = true;
	cancel(provision_timer_);
	provision_timer_ = loop_.schedule_at(clock_.monotonic_ms() + kProvisionTimeoutMs,
		Callback::member<Improv, &Improv::provision_timeout_due>(this));
	send_state();
	connectivity_.request_connect("Improv: new WiFi credentials");
}

void Improv::rpc_get_state() {
	send_state();
	if (state() == improv::State::provisioned) {
		// The clients await the device URL after a "provisioned" state.
		char url[kMaxUrl];
		format_url(url, sizeof(url));
		const char* strings[] = {url};
		send_rpc_result(improv::kRpcGetCurrentState, strings, 1);
	}
}

void Improv::rpc_get_info() {
	char version[64];
	std::snprintf(version, sizeof(version), "%s (%s)", kVersion, build_id());
	const char* strings[] = {kFirmwareName, version, port_->chip_family(), port_->device_name()};
	send_rpc_result(improv::kRpcGetDeviceInfo, strings, 4);
}

void Improv::rpc_get_networks() {
	if (scanning_) {
		Log::info(kTag, "Improv: a WiFi scan is already running; its list answers this request too");
		return;
	}
	if (scanner_ == nullptr) {
		Log::warn(kTag, "Improv: no WiFi scan on this platform; sending an empty network list");
		send_rpc_result(improv::kRpcGetWifiNetworks, nullptr, 0);
		return;
	}
	if (!connectivity_.pause_pairing("Improv WiFi scan")) {
		Log::warn(kTag, "Improv: the station cannot be freed for a scan; sending an empty network list");
		send_rpc_result(improv::kRpcGetWifiNetworks, nullptr, 0);
		return;
	}
	scanning_ = true;
	network_count_ = 0;
	uint64_t now = clock_.monotonic_ms();
	scan_timeout_ = loop_.schedule_at(now + kScanTimeoutMs, Callback::member<Improv, &Improv::scan_timeout_due>(this));
	uint64_t at = connectivity_.station_free_at();
	scan_timer_ = loop_.schedule_at(at > now ? at : now, Callback::member<Improv, &Improv::scan_start_due>(this));
	if (!scan_timer_.valid() || !scan_timeout_.valid()) {
		finish_scan(false, "no timer for the scan");
	}
}

// ---- Provisioning outcome ---------------------------------------------------------------------

void Improv::connectivity_changed(const Connectivity& connectivity, LinkState previous) {
	if (!active_) {
		return;
	}
	if (!link_was_up_ && connectivity.is_up()) {
		link_was_up_ = true;
		uint64_t at = clock_.monotonic_ms() + kGraceMs;
		if (at < end_at_) {
			set_end(at, "the network is up");
			Log::info(kTag, "network up: Improv ends in %u s", static_cast<unsigned>(kGraceMs / 1000));
		}
	}
	if (!provisioning_) {
		return;
	}
	if (connectivity.is_up() && previous != LinkState::up) {
		provisioning_done(true, "link up");
	} else if (connectivity.failures() >= kProvisionFailures) {
		char why[Connectivity::kMaxReason + 32];
		std::snprintf(why, sizeof(why), "%u attempts failed (%s)", static_cast<unsigned>(connectivity.failures()),
			connectivity.reason());
		provisioning_done(false, why);
	}
}

void Improv::provision_timeout_due() {
	provision_timer_ = TimerHandle();
	char why[48];
	std::snprintf(why, sizeof(why), "no link after %u s", static_cast<unsigned>(kProvisionTimeoutMs / 1000));
	provisioning_done(false, why);
}

void Improv::provisioning_done(bool success, const char* why) {
	provisioning_ = false;
	cancel(provision_timer_);
	if (success) {
		char url[kMaxUrl];
		format_url(url, sizeof(url));
		Log::info(kTag, "Improv: provisioned (%s), device at %s", why, url);
		send_state();
		const char* strings[] = {url};
		send_rpc_result(improv::kRpcWifiSettings, strings, 1);
	} else {
		Log::warn(kTag, "Improv: provisioning failed: %s; the credentials stay stored", why);
		send_error(improv::Error::unable_to_connect);
		send_state();
	}
	// The client reads the outcome and may ask more.
	uint64_t keep_until = clock_.monotonic_ms() + kGraceMs;
	if (end_at_ < keep_until) {
		set_end(keep_until, "no client frame for the grace period");
	}
}

// ---- Scan -------------------------------------------------------------------------------------

void Improv::scan_start_due() {
	scan_timer_ = TimerHandle();
	if (!scanning_) {
		return;
	}
	if (!scanner_->start_scan(*this)) {
		finish_scan(false, "the platform could not start it");
	}
}

void Improv::scan_timeout_due() {
	scan_timeout_ = TimerHandle();
	char why[48];
	std::snprintf(why, sizeof(why), "no result after %u s", static_cast<unsigned>(kScanTimeoutMs / 1000));
	finish_scan(false, why);
}

void Improv::scan_network(const char* ssid, int8_t rssi, bool secured) {
	if (!scanning_) {
		Log::debug(kTag, "scan result outside a scan, ignored");
		return;
	}
	size_t length = std::strlen(ssid);
	if (length == 0 || length > setting::kMaxSsidLength) {
		return; // a hidden network: the client offers "join other" for it
	}
	for (size_t i = 0; i < network_count_; ++i) {
		if (std::strcmp(networks_[i].ssid, ssid) == 0) {
			// One record per access point of a mesh: keep the strongest.
			if (rssi > networks_[i].rssi) {
				networks_[i].rssi = rssi;
				networks_[i].secured = secured;
			}
			return;
		}
	}
	size_t slot = network_count_;
	if (network_count_ == kMaxNetworks) {
		slot = 0;
		for (size_t i = 1; i < network_count_; ++i) {
			if (networks_[i].rssi < networks_[slot].rssi) {
				slot = i;
			}
		}
		if (networks_[slot].rssi >= rssi) {
			return;
		}
	} else {
		++network_count_;
	}
	std::memcpy(networks_[slot].ssid, ssid, length + 1);
	networks_[slot].rssi = rssi;
	networks_[slot].secured = secured;
}

void Improv::scan_finished(bool ok, const char* reason) {
	if (!scanning_) {
		Log::debug(kTag, "scan end outside a scan, ignored");
		return;
	}
	finish_scan(ok, reason);
}

void Improv::finish_scan(bool ok, const char* reason) {
	cancel(scan_timer_);
	cancel(scan_timeout_);
	scanning_ = false;
	connectivity_.resume_pairing("Improv WiFi scan over");
	if (!ok) {
		Log::warn(kTag, "Improv: WiFi scan failed: %s; sending an empty network list", reason);
		network_count_ = 0;
	}
	// Strongest first (insertion sort: at most kMaxNetworks entries).
	for (size_t i = 1; i < network_count_; ++i) {
		Network entry = networks_[i];
		size_t j = i;
		while (j > 0 && networks_[j - 1].rssi < entry.rssi) {
			networks_[j] = networks_[j - 1];
			--j;
		}
		networks_[j] = entry;
	}
	if (active_) {
		for (size_t i = 0; i < network_count_; ++i) {
			char rssi[8];
			std::snprintf(rssi, sizeof(rssi), "%d", static_cast<int>(networks_[i].rssi));
			const char* strings[] = {networks_[i].ssid, rssi, networks_[i].secured ? "YES" : "NO"};
			send_rpc_result(improv::kRpcGetWifiNetworks, strings, 3);
		}
		send_rpc_result(improv::kRpcGetWifiNetworks, nullptr, 0);
		if (ok) {
			Log::info(kTag, "Improv: WiFi scan reported %u network(s)", static_cast<unsigned>(network_count_));
		}
	}
	network_count_ = 0;
}

// ---- Output -----------------------------------------------------------------------------------

void Improv::send_state() {
	send(improv::build_current_state(state(), frame_, sizeof(frame_)));
}

void Improv::send_error(improv::Error error) {
	send(improv::build_error(error, frame_, sizeof(frame_)));
}

void Improv::send_rpc_result(uint8_t command, const char* const* strings, size_t count) {
	size_t length = improv::build_rpc_result(command, strings, count, frame_, sizeof(frame_));
	if (length == 0) {
		Log::error(kTag, "Improv RPC result for 0x%02x does not fit a frame, not sent", static_cast<unsigned>(command));
		return;
	}
	send(length);
}

void Improv::send(size_t length) {
	if (length == 0 || port_ == nullptr) {
		return;
	}
	port_->send_frame(frame_, length);
}

void Improv::format_url(char* buffer, size_t size) const {
	std::snprintf(buffer, size, "http://%s.local/", port_->device_name());
}

// ---- Lifetime ---------------------------------------------------------------------------------

void Improv::set_end(uint64_t at, const char* reason) {
	end_at_ = at;
	end_reason_ = reason;
	cancel(end_timer_);
	end_timer_ = loop_.schedule_at(at, Callback::member<Improv, &Improv::end_due>(this));
	if (!end_timer_.valid()) {
		end("no timer for the Improv window");
	}
}

void Improv::end_due() {
	end_timer_ = TimerHandle();
	end(end_reason_);
}

void Improv::end(const char* reason) {
	if (!active_) {
		return;
	}
	active_ = false;
	cancel(end_timer_);
	cancel(provision_timer_);
	provisioning_ = false;
	if (scanning_) {
		cancel(scan_timer_);
		cancel(scan_timeout_);
		scanning_ = false;
		network_count_ = 0;
		connectivity_.resume_pairing("Improv ended");
	}
	Log::info(kTag, "Improv Wi-Fi serial ended (%s): the serial port belongs to the console", reason);
	port_->improv_ended();
}

void Improv::cancel(TimerHandle& timer) {
	if (timer.valid()) {
		loop_.cancel(timer);
		timer = TimerHandle();
	}
}

} // namespace hc
