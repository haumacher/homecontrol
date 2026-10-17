/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/posix/file_settings.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "hc/log.h"

namespace hc {
namespace posix {

namespace {

constexpr const char* kTag = "hc.settings";

constexpr const char* kHeader =
	"# HomeControl settings, format 1 (doc/network.md). Written by the controller; edit only while it\n"
	"# is stopped. Holds the WiFi key: keep this file private.\n";

char type_letter(SettingType type) {
	switch (type) {
	case SettingType::string:
		return 's';
	case SettingType::integer:
		return 'i';
	case SettingType::boolean:
		return 'b';
	}
	return '?';
}

} // namespace

FileSettings::FileSettings(std::string path) : path_(std::move(path)) {}

bool FileSettings::load() {
	entries_.clear();
	FILE* file = std::fopen(path_.c_str(), "r");
	if (file == nullptr) {
		if (errno == ENOENT) {
			Log::info(kTag, "settings file %s does not exist yet, starting empty", path_.c_str());
			return true;
		}
		Log::error(kTag, "cannot read settings file %s: %s", path_.c_str(), std::strerror(errno));
		return false;
	}
	char buffer[512];
	unsigned line_number = 0;
	unsigned ignored = 0;
	std::string line;
	while (std::fgets(buffer, sizeof(buffer), file) != nullptr) {
		line.append(buffer);
		if (line.empty() || (line.back() != '\n' && !std::feof(file))) {
			continue; // A longer line: read on.
		}
		++line_number;
		while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
			line.pop_back();
		}
		const char* error = nullptr;
		if (!line.empty() && line[0] != '#' && !parse_line(line, error)) {
			Log::warn(kTag, "%s line %u ignored (%s); it is dropped at the next write", path_.c_str(), line_number,
				error);
			++ignored;
		}
		line.clear();
	}
	std::fclose(file);
	Log::info(kTag, "settings file %s: %u entries%s", path_.c_str(), static_cast<unsigned>(entries_.size()),
		ignored > 0 ? ", some lines ignored" : "");
	return true;
}

bool FileSettings::parse_line(const std::string& line, const char*& error) {
	size_t equals = line.find('=');
	if (equals == std::string::npos) {
		error = "no '='";
		return false;
	}
	std::string key = line.substr(0, equals);
	if (check_key(key.c_str()) != SettingsStatus::ok) {
		error = settings_status_name(SettingsStatus::invalid_key);
		return false;
	}
	if (line.size() < equals + 3 || line[equals + 2] != ':') {
		error = "no type (s:, i:, b:) after '='";
		return false;
	}
	char type = line[equals + 1];
	std::string value = line.substr(equals + 3);
	Entry entry{SettingType::string, std::string(), 0};
	if (type == 's') {
		SettingsStatus status = check_string(value.c_str());
		if (status != SettingsStatus::ok) {
			error = settings_status_name(status);
			return false;
		}
		entry.text = value;
	} else if (type == 'i' || type == 'b') {
		char* end = nullptr;
		errno = 0;
		long number = std::strtol(value.c_str(), &end, 10);
		if (value.empty() || *end != '\0' || errno != 0 || number < INT32_MIN || number > INT32_MAX) {
			error = "integer value invalid";
			return false;
		}
		if (type == 'b' && number != 0 && number != 1) {
			error = "boolean value must be 0 or 1";
			return false;
		}
		entry.type = type == 'i' ? SettingType::integer : SettingType::boolean;
		entry.number = static_cast<int32_t>(number);
	} else {
		error = "unknown type letter";
		return false;
	}
	entries_[key] = entry;
	return true;
}

SettingsStatus FileSettings::find(const char* key, SettingType type, const Entry*& out) const {
	SettingsStatus status = check_key(key);
	if (status != SettingsStatus::ok) {
		return status;
	}
	auto it = entries_.find(key);
	if (it == entries_.end()) {
		return SettingsStatus::not_found;
	}
	if (it->second.type != type) {
		return SettingsStatus::wrong_type;
	}
	out = &it->second;
	return SettingsStatus::ok;
}

SettingsStatus FileSettings::get_string(const char* key, char* buffer, size_t size) const {
	if (size > 0) {
		buffer[0] = '\0';
	}
	const Entry* entry = nullptr;
	SettingsStatus status = find(key, SettingType::string, entry);
	if (status != SettingsStatus::ok) {
		return status;
	}
	if (entry->text.size() + 1 > size) {
		return SettingsStatus::too_long;
	}
	std::memcpy(buffer, entry->text.c_str(), entry->text.size() + 1);
	return SettingsStatus::ok;
}

SettingsStatus FileSettings::set_string(const char* key, const char* value) {
	SettingsStatus status = check_key(key);
	if (status == SettingsStatus::ok) {
		status = check_string(value);
	}
	if (status != SettingsStatus::ok) {
		return status;
	}
	return store(key, Entry{SettingType::string, value, 0});
}

SettingsStatus FileSettings::get_int(const char* key, int32_t& value) const {
	const Entry* entry = nullptr;
	SettingsStatus status = find(key, SettingType::integer, entry);
	if (status == SettingsStatus::ok) {
		value = entry->number;
	}
	return status;
}

SettingsStatus FileSettings::set_int(const char* key, int32_t value) {
	SettingsStatus status = check_key(key);
	if (status != SettingsStatus::ok) {
		return status;
	}
	return store(key, Entry{SettingType::integer, std::string(), value});
}

SettingsStatus FileSettings::get_bool(const char* key, bool& value) const {
	const Entry* entry = nullptr;
	SettingsStatus status = find(key, SettingType::boolean, entry);
	if (status == SettingsStatus::ok) {
		value = entry->number != 0;
	}
	return status;
}

SettingsStatus FileSettings::set_bool(const char* key, bool value) {
	SettingsStatus status = check_key(key);
	if (status != SettingsStatus::ok) {
		return status;
	}
	return store(key, Entry{SettingType::boolean, std::string(), value ? 1 : 0});
}

SettingsStatus FileSettings::erase(const char* key) {
	SettingsStatus status = check_key(key);
	if (status != SettingsStatus::ok) {
		return status;
	}
	auto it = entries_.find(key);
	if (it == entries_.end()) {
		return SettingsStatus::not_found;
	}
	Entry previous = it->second;
	entries_.erase(it);
	if (!save()) {
		entries_[key] = previous;
		return SettingsStatus::storage_error;
	}
	return SettingsStatus::ok;
}

void FileSettings::visit(SettingsVisitor& visitor) const {
	for (const auto& entry : entries_) {
		visitor.entry(entry.first.c_str(), entry.second.type);
	}
}

SettingsStatus FileSettings::store(const char* key, const Entry& entry) {
	auto it = entries_.find(key);
	bool existed = it != entries_.end();
	Entry previous = existed ? it->second : Entry{SettingType::string, std::string(), 0};
	entries_[key] = entry;
	if (!save()) {
		if (existed) {
			entries_[key] = previous;
		} else {
			entries_.erase(key);
		}
		return SettingsStatus::storage_error;
	}
	return SettingsStatus::ok;
}

bool FileSettings::save() const {
	std::string temporary = path_ + ".tmp";
	int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		Log::error(kTag, "cannot write settings file %s: %s", temporary.c_str(), std::strerror(errno));
		return false;
	}
	FILE* file = ::fdopen(fd, "w");
	if (file == nullptr) {
		Log::error(kTag, "cannot write settings file %s: %s", temporary.c_str(), std::strerror(errno));
		::close(fd);
		return false;
	}
	bool ok = std::fputs(kHeader, file) >= 0;
	for (const auto& entry : entries_) {
		if (entry.second.type == SettingType::string) {
			ok = ok && std::fprintf(file, "%s=s:%s\n", entry.first.c_str(), entry.second.text.c_str()) >= 0;
		} else {
			ok = ok && std::fprintf(file, "%s=%c:%ld\n", entry.first.c_str(), type_letter(entry.second.type),
				static_cast<long>(entry.second.number)) >= 0;
		}
	}
	ok = ok && std::fflush(file) == 0 && ::fsync(fd) == 0;
	int saved_errno = errno;
	ok = (std::fclose(file) == 0) && ok;
	if (!ok) {
		Log::error(kTag, "cannot write settings file %s: %s", temporary.c_str(), std::strerror(saved_errno));
		::unlink(temporary.c_str());
		return false;
	}
	if (std::rename(temporary.c_str(), path_.c_str()) != 0) {
		Log::error(kTag, "cannot replace settings file %s: %s", path_.c_str(), std::strerror(errno));
		::unlink(temporary.c_str());
		return false;
	}
	return true;
}

} // namespace posix
} // namespace hc
