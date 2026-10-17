/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/platform/settings.h"

#include <cstring>

namespace hc {

const char* settings_status_name(SettingsStatus status) {
	switch (status) {
	case SettingsStatus::ok:
		return "ok";
	case SettingsStatus::not_found:
		return "not found";
	case SettingsStatus::invalid_key:
		return "key invalid (1 to 15 characters of A-Z a-z 0-9 . _ -)";
	case SettingsStatus::too_long:
		return "value too long (at most 127 bytes)";
	case SettingsStatus::invalid_value:
		return "value contains a control character";
	case SettingsStatus::wrong_type:
		return "stored with another type";
	case SettingsStatus::storage_error:
		return "storage error";
	}
	return "?";
}

const char* setting_type_name(SettingType type) {
	switch (type) {
	case SettingType::string:
		return "string";
	case SettingType::integer:
		return "integer";
	case SettingType::boolean:
		return "boolean";
	}
	return "?";
}

SettingsStatus Settings::check_key(const char* key) {
	if (key == nullptr || key[0] == '\0') {
		return SettingsStatus::invalid_key;
	}
	size_t length = 0;
	for (const char* p = key; *p != '\0'; ++p) {
		char c = *p;
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
			c == '_' || c == '-';
		if (!ok || ++length > kMaxKeyLength) {
			return SettingsStatus::invalid_key;
		}
	}
	return SettingsStatus::ok;
}

SettingsStatus Settings::check_string(const char* value) {
	if (value == nullptr) {
		return SettingsStatus::invalid_value;
	}
	size_t length = 0;
	for (const char* p = value; *p != '\0'; ++p) {
		if (static_cast<unsigned char>(*p) < 0x20) {
			return SettingsStatus::invalid_value;
		}
		if (++length > kMaxStringLength) {
			return SettingsStatus::too_long;
		}
	}
	return SettingsStatus::ok;
}

} // namespace hc
