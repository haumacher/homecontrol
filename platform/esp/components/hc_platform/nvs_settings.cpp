/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "nvs_settings.h"

#include <cstring>

#include "esp_log.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.nvs";

} // namespace

bool NvsSettings::open() {
	esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle_);
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "cannot open NVS namespace \"%s\": %s; settings are unavailable", kNamespace,
			esp_err_to_name(err));
		return false;
	}
	open_ = true;
	return true;
}

SettingsStatus NvsSettings::check_read(const char* key, nvs_type_t type) const {
	SettingsStatus status = check_key(key);
	if (status != SettingsStatus::ok) {
		return status;
	}
	if (!open_) {
		return SettingsStatus::storage_error;
	}
	nvs_type_t stored = NVS_TYPE_ANY;
	esp_err_t err = nvs_find_key(handle_, key, &stored);
	if (err == ESP_ERR_NVS_NOT_FOUND) {
		return SettingsStatus::not_found;
	}
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "cannot look up %s: %s", key, esp_err_to_name(err));
		return SettingsStatus::storage_error;
	}
	return stored == type ? SettingsStatus::ok : SettingsStatus::wrong_type;
}

SettingsStatus NvsSettings::prepare_write(const char* key, nvs_type_t type) {
	SettingsStatus status = check_read(key, type);
	if (status == SettingsStatus::wrong_type) {
		// A set replaces a value of another type.
		esp_err_t err = nvs_erase_key(handle_, key);
		if (err != ESP_OK) {
			ESP_LOGE(kTag, "cannot replace %s: %s", key, esp_err_to_name(err));
			return SettingsStatus::storage_error;
		}
		return SettingsStatus::ok;
	}
	return status == SettingsStatus::not_found ? SettingsStatus::ok : status;
}

SettingsStatus NvsSettings::finish_write(const char* key, esp_err_t err) {
	if (err == ESP_OK) {
		err = nvs_commit(handle_);
	}
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "cannot store %s: %s", key, esp_err_to_name(err));
		return SettingsStatus::storage_error;
	}
	return SettingsStatus::ok;
}

SettingsStatus NvsSettings::get_string(const char* key, char* buffer, size_t size) const {
	if (size > 0) {
		buffer[0] = '\0';
	}
	SettingsStatus status = check_read(key, NVS_TYPE_STR);
	if (status != SettingsStatus::ok) {
		return status;
	}
	size_t length = 0;
	esp_err_t err = nvs_get_str(handle_, key, nullptr, &length);
	if (err == ESP_OK && length > size) {
		return SettingsStatus::too_long;
	}
	if (err == ESP_OK) {
		err = nvs_get_str(handle_, key, buffer, &length);
	}
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "cannot read %s: %s", key, esp_err_to_name(err));
		if (size > 0) {
			buffer[0] = '\0';
		}
		return SettingsStatus::storage_error;
	}
	return SettingsStatus::ok;
}

SettingsStatus NvsSettings::set_string(const char* key, const char* value) {
	SettingsStatus status = check_key(key);
	if (status == SettingsStatus::ok) {
		status = check_string(value);
	}
	if (status == SettingsStatus::ok) {
		status = prepare_write(key, NVS_TYPE_STR);
	}
	if (status != SettingsStatus::ok) {
		return status;
	}
	return finish_write(key, nvs_set_str(handle_, key, value));
}

SettingsStatus NvsSettings::get_int(const char* key, int32_t& value) const {
	SettingsStatus status = check_read(key, NVS_TYPE_I32);
	if (status != SettingsStatus::ok) {
		return status;
	}
	esp_err_t err = nvs_get_i32(handle_, key, &value);
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "cannot read %s: %s", key, esp_err_to_name(err));
		return SettingsStatus::storage_error;
	}
	return SettingsStatus::ok;
}

SettingsStatus NvsSettings::set_int(const char* key, int32_t value) {
	SettingsStatus status = prepare_write(key, NVS_TYPE_I32);
	if (status != SettingsStatus::ok) {
		return status;
	}
	return finish_write(key, nvs_set_i32(handle_, key, value));
}

SettingsStatus NvsSettings::get_bool(const char* key, bool& value) const {
	SettingsStatus status = check_read(key, NVS_TYPE_U8);
	if (status != SettingsStatus::ok) {
		return status;
	}
	uint8_t raw = 0;
	esp_err_t err = nvs_get_u8(handle_, key, &raw);
	if (err != ESP_OK) {
		ESP_LOGE(kTag, "cannot read %s: %s", key, esp_err_to_name(err));
		return SettingsStatus::storage_error;
	}
	value = raw != 0;
	return SettingsStatus::ok;
}

SettingsStatus NvsSettings::set_bool(const char* key, bool value) {
	SettingsStatus status = prepare_write(key, NVS_TYPE_U8);
	if (status != SettingsStatus::ok) {
		return status;
	}
	return finish_write(key, nvs_set_u8(handle_, key, value ? 1 : 0));
}

SettingsStatus NvsSettings::erase(const char* key) {
	SettingsStatus status = check_key(key);
	if (status != SettingsStatus::ok) {
		return status;
	}
	if (!open_) {
		return SettingsStatus::storage_error;
	}
	esp_err_t err = nvs_erase_key(handle_, key);
	if (err == ESP_ERR_NVS_NOT_FOUND) {
		return SettingsStatus::not_found;
	}
	return finish_write(key, err);
}

void NvsSettings::visit(SettingsVisitor& visitor) const {
	if (!open_) {
		return;
	}
	nvs_iterator_t it = nullptr;
	esp_err_t err = nvs_entry_find_in_handle(handle_, NVS_TYPE_ANY, &it);
	while (err == ESP_OK) {
		nvs_entry_info_t info;
		nvs_entry_info(it, &info);
		switch (info.type) {
		case NVS_TYPE_STR:
			visitor.entry(info.key, SettingType::string);
			break;
		case NVS_TYPE_I32:
			visitor.entry(info.key, SettingType::integer);
			break;
		case NVS_TYPE_U8:
			visitor.entry(info.key, SettingType::boolean);
			break;
		default:
			ESP_LOGW(kTag, "NVS entry %s has a type settings do not use (0x%x), skipped", info.key,
				static_cast<unsigned>(info.type));
			break;
		}
		err = nvs_entry_next(&it);
	}
	nvs_release_iterator(it);
}

} // namespace esp
} // namespace hc
