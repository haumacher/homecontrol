/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_NVS_SETTINGS_H
#define HC_ESP_NVS_SETTINGS_H

#include "hc/platform/settings.h"
#include "nvs.h"

namespace hc {
namespace esp {

/// Settings in NVS, namespace "hc" of the default partition. Strings are NVS strings, integers
/// i32, booleans u8 (persisted format: never change the mapping). nvs_flash_init() must have
/// succeeded before open().
class NvsSettings : public Settings {
public:
	static constexpr const char* kNamespace = "hc";

	/// Opens the namespace; logs and returns false on failure (every call then reports
	/// storage_error).
	bool open();

	SettingsStatus get_string(const char* key, char* buffer, size_t size) const override;
	SettingsStatus set_string(const char* key, const char* value) override;
	SettingsStatus get_int(const char* key, int32_t& value) const override;
	SettingsStatus set_int(const char* key, int32_t value) override;
	SettingsStatus get_bool(const char* key, bool& value) const override;
	SettingsStatus set_bool(const char* key, bool value) override;
	SettingsStatus erase(const char* key) override;
	void visit(SettingsVisitor& visitor) const override;

private:
	SettingsStatus check_read(const char* key, nvs_type_t type) const;
	SettingsStatus prepare_write(const char* key, nvs_type_t type);
	SettingsStatus finish_write(const char* key, esp_err_t err);

	nvs_handle_t handle_ = 0;
	bool open_ = false;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_NVS_SETTINGS_H
