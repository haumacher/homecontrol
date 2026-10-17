/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/network_settings.h"

#include <cstdio>
#include <cstring>

#include "hc/log.h"

namespace hc {

namespace {

bool refuse(const char** error, const char* text) {
	if (error != nullptr) {
		*error = text;
	}
	return false;
}

bool is_hex(char c) {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

} // namespace

bool setting_is_secret(const char* key) {
	return key != nullptr && std::strcmp(key, setting::kWifiPsk) == 0;
}

bool wifi_ssid_valid(const char* ssid, const char** error) {
	if (ssid == nullptr || ssid[0] == '\0') {
		return refuse(error, "SSID is empty");
	}
	if (std::strlen(ssid) > setting::kMaxSsidLength) {
		return refuse(error, "SSID is longer than 32 bytes");
	}
	return true;
}

bool wifi_psk_valid(const char* psk, const char** error) {
	if (psk == nullptr) {
		return refuse(error, "key is missing");
	}
	size_t length = std::strlen(psk);
	if (length == 64) {
		for (size_t i = 0; i < length; ++i) {
			if (!is_hex(psk[i])) {
				return refuse(error, "a 64-character key must be hex digits (a passphrase has 8 to 63 characters)");
			}
		}
		return true;
	}
	if (length < 8 || length > 63) {
		return refuse(error, "passphrase must have 8 to 63 characters");
	}
	for (size_t i = 0; i < length; ++i) {
		unsigned char c = static_cast<unsigned char>(psk[i]);
		if (c < 0x20 || c > 0x7e) {
			return refuse(error, "passphrase must be printable ASCII");
		}
	}
	return true;
}

bool host_name_valid(const char* name, const char** error) {
	if (name == nullptr || name[0] == '\0') {
		return refuse(error, "host name is empty");
	}
	size_t length = std::strlen(name);
	if (length > Settings::kMaxStringLength) {
		return refuse(error, "host name is too long");
	}
	size_t label = 0;
	for (size_t i = 0; i < length; ++i) {
		char c = name[i];
		if (c == '.') {
			if (label == 0) {
				return refuse(error, "host name has an empty label");
			}
			label = 0;
			continue;
		}
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
		if (!ok) {
			return refuse(error, "host name may contain only letters, digits, '-' and '.'");
		}
		if (++label > 63) {
			return refuse(error, "host name label is longer than 63 characters");
		}
	}
	if (label == 0) {
		return refuse(error, "host name ends with '.'");
	}
	return true;
}

bool store_wifi_credentials(Settings& settings, const char* ssid, const char* psk, const char** error) {
	if (!wifi_ssid_valid(ssid, error)) {
		return false;
	}
	bool open = psk == nullptr || psk[0] == '\0';
	if (!open && !wifi_psk_valid(psk, error)) {
		return false;
	}
	SettingsStatus status = settings.set_string(setting::kWifiSsid, ssid);
	if (status == SettingsStatus::ok) {
		status = open ? settings.erase(setting::kWifiPsk) : settings.set_string(setting::kWifiPsk, psk);
		if (status == SettingsStatus::not_found) {
			status = SettingsStatus::ok;
		}
	}
	if (status != SettingsStatus::ok) {
		return refuse(error, settings_status_name(status));
	}
	return true;
}

bool read_string_setting(const Settings& settings, const char* key, const char* fallback, char* buffer,
	size_t size, const char* tag, bool (*validate)(const char*, const char**)) {
	SettingsStatus status = settings.get_string(key, buffer, size);
	if (status == SettingsStatus::ok) {
		const char* error = nullptr;
		if (validate == nullptr || validate(buffer, &error)) {
			return true;
		}
		Log::error(tag, "setting %s is invalid (%s), using the default %s", key, error, fallback);
	} else if (status != SettingsStatus::not_found) {
		Log::error(tag, "setting %s unreadable (%s), using the default %s", key, settings_status_name(status),
			fallback);
	}
	std::snprintf(buffer, size, "%s", fallback);
	return false;
}

} // namespace hc
