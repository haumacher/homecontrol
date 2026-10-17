/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_POSIX_FILE_SETTINGS_H
#define HC_POSIX_FILE_SETTINGS_H

#include <map>
#include <string>

#include "hc/platform/settings.h"

namespace hc {
namespace posix {

/// Settings in a text file, one entry per line (format 1, doc/network.md):
///
///     # comment
///     wifi.ssid=s:Example Net
///     count=i:-42
///     flag=b:1
///
/// The key ends at the first '='; the type letter (s, i, b) and ':' follow; a string value is the
/// rest of the line verbatim (values cannot contain control characters, so no escaping is needed).
/// Every successful set rewrites the whole file through a temporary file and rename(), created
/// with mode 0600 because it holds the WiFi key. Lines that do not parse are logged and dropped
/// at the next write.
class FileSettings : public Settings {
public:
	explicit FileSettings(std::string path);

	/// Reads the file. A missing file is an empty store (logged at info); an unreadable file is
	/// logged as an error and returns false (the store stays empty, writes will try to create it).
	bool load();

	const std::string& path() const { return path_; }

	SettingsStatus get_string(const char* key, char* buffer, size_t size) const override;
	SettingsStatus set_string(const char* key, const char* value) override;
	SettingsStatus get_int(const char* key, int32_t& value) const override;
	SettingsStatus set_int(const char* key, int32_t value) override;
	SettingsStatus get_bool(const char* key, bool& value) const override;
	SettingsStatus set_bool(const char* key, bool value) override;
	SettingsStatus erase(const char* key) override;
	void visit(SettingsVisitor& visitor) const override;

private:
	struct Entry {
		SettingType type;
		std::string text;
		int32_t number;
	};

	SettingsStatus find(const char* key, SettingType type, const Entry*& out) const;
	SettingsStatus store(const char* key, const Entry& entry);
	bool save() const;
	bool parse_line(const std::string& line, const char*& error);

	std::string path_;
	std::map<std::string, Entry> entries_;
};

} // namespace posix
} // namespace hc

#endif // HC_POSIX_FILE_SETTINGS_H
