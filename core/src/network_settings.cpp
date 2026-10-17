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
	// web.pw and web.tok.N hold password and token hashes (doc/web.md): never shown either.
	return key != nullptr && (std::strcmp(key, setting::kWifiPsk) == 0 || std::strncmp(key, "web.", 4) == 0 ||
		std::strcmp(key, "mqtt.password") == 0);
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

bool ui_lang_valid(const char* lang, const char** error) {
	static const char kRule[] = "a language is two lower-case letters, optionally '-' and two upper-case letters (de, pt-BR)";
	if (lang == nullptr || lang[0] == '\0') {
		return refuse(error, "the language is empty");
	}
	size_t length = std::strlen(lang);
	if (length != 2 && length != 5) {
		return refuse(error, kRule);
	}
	auto lower = [](char c) { return c >= 'a' && c <= 'z'; };
	auto upper = [](char c) { return c >= 'A' && c <= 'Z'; };
	if (!lower(lang[0]) || !lower(lang[1])) {
		return refuse(error, kRule);
	}
	if (length == 5 && (lang[2] != '-' || !upper(lang[3]) || !upper(lang[4]))) {
		return refuse(error, kRule);
	}
	return true;
}

void read_ui_lang(const Settings& settings, char (&buffer)[setting::kMaxUiLangLength + 1], const char* tag) {
	// A buffer one byte longer than the longest tag would truncate "en-GBX" to a valid "en-GB":
	// read into a setting-sized buffer and validate the whole value.
	char value[Settings::kMaxStringLength + 1];
	read_string_setting(settings, setting::kUiLang, setting::kDefaultUiLang, value, sizeof(value), tag, &ui_lang_valid);
	size_t length = std::strlen(value); // valid, or the default: at most kMaxUiLangLength
	if (length > setting::kMaxUiLangLength) {
		length = setting::kMaxUiLangLength;
	}
	std::memcpy(buffer, value, length);
	buffer[length] = '\0';
}

bool store_ui_lang(Settings& settings, const char* lang, const char** error) {
	if (!ui_lang_valid(lang, error)) {
		return false;
	}
	SettingsStatus status = settings.set_string(setting::kUiLang, lang);
	if (status != SettingsStatus::ok) {
		return refuse(error, settings_status_name(status));
	}
	return true;
}

const char* wifi_power_save_name(setting::WifiPowerSave mode) {
	switch (mode) {
	case setting::WifiPowerSave::min:
		return "min";
	case setting::WifiPowerSave::max:
		return "max";
	case setting::WifiPowerSave::none:
	default:
		return "none";
	}
}

bool parse_wifi_power_save(const char* text, setting::WifiPowerSave& mode, const char** error) {
	if (text != nullptr) {
		if (std::strcmp(text, "none") == 0) {
			mode = setting::WifiPowerSave::none;
			return true;
		}
		if (std::strcmp(text, "min") == 0) {
			mode = setting::WifiPowerSave::min;
			return true;
		}
		if (std::strcmp(text, "max") == 0) {
			mode = setting::WifiPowerSave::max;
			return true;
		}
	}
	return refuse(error, "WiFi power save is none, min or max");
}

bool wifi_power_save_valid(const char* text, const char** error) {
	setting::WifiPowerSave mode;
	return parse_wifi_power_save(text, mode, error);
}

setting::WifiPowerSave read_wifi_power_save(const Settings& settings, const char* tag) {
	char value[Settings::kMaxStringLength + 1];
	read_string_setting(settings, setting::kWifiPowerSave, wifi_power_save_name(setting::kDefaultWifiPowerSave), value,
		sizeof(value), tag, &wifi_power_save_valid);
	setting::WifiPowerSave mode = setting::kDefaultWifiPowerSave;
	parse_wifi_power_save(value, mode);
	return mode;
}

bool store_wifi_power_save(Settings& settings, const char* text, const char** error) {
	if (!wifi_power_save_valid(text, error)) {
		return false;
	}
	SettingsStatus status = settings.set_string(setting::kWifiPowerSave, text);
	if (status != SettingsStatus::ok) {
		return refuse(error, settings_status_name(status));
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
