/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_SETTINGS_H
#define HC_PLATFORM_SETTINGS_H

#include <cstddef>
#include <cstdint>

namespace hc {

/// The type of a stored setting.
enum class SettingType : uint8_t {
	string,
	integer, ///< int32_t
	boolean,
};

/// Outcome of a Settings call. Everything except `ok` is a refusal; settings_status_name() gives
/// the text to log.
enum class SettingsStatus : uint8_t {
	ok,
	not_found,     ///< No value stored under the key.
	invalid_key,   ///< Empty, longer than Settings::kMaxKeyLength or a character outside [A-Za-z0-9._-].
	too_long,      ///< String value longer than Settings::kMaxStringLength, or the caller's buffer too small.
	invalid_value, ///< String value with a control character (below 0x20).
	wrong_type,    ///< A value is stored under the key, but of another type.
	storage_error, ///< The backing store failed (flash, file system); the platform logged the cause.
};

/// Human-readable name of a status ("ok", "not found", "key invalid", ...).
const char* settings_status_name(SettingsStatus status);

/// Name of a type ("string", "integer", "boolean").
const char* setting_type_name(SettingType type);

/// Receives the entries of Settings::visit(). Only the key and the type are passed; read the value
/// with the matching getter.
class SettingsVisitor {
public:
	virtual void entry(const char* key, SettingType type) = 0;

protected:
	~SettingsVisitor() = default;
};

/// Small named values that survive a restart: credentials, the time zone, server names. The ESP
/// platform stores them in NVS (namespace "hc"), POSIX in a text file, the tests in memory.
///
/// Keys are 1 to kMaxKeyLength characters from [A-Za-z0-9._-] (the NVS key limit). String values
/// are at most kMaxStringLength bytes without control characters. Every violation is refused with
/// a status, nothing is truncated. A set replaces the stored value whatever its previous type;
/// every set is persistent when it returns ok.
///
/// Threading: not thread-safe; the core calls it on the event loop's thread.
class Settings {
public:
	/// Longest key in characters (NVS allows 15).
	static constexpr size_t kMaxKeyLength = 15;

	/// Longest string value in bytes, without the terminating NUL.
	static constexpr size_t kMaxStringLength = 127;

	/// Copies the string stored under `key` into `buffer` (NUL-terminated). Refuses with too_long
	/// if it does not fit `size` (buffer then holds an empty string).
	virtual SettingsStatus get_string(const char* key, char* buffer, size_t size) const = 0;
	virtual SettingsStatus set_string(const char* key, const char* value) = 0;

	virtual SettingsStatus get_int(const char* key, int32_t& value) const = 0;
	virtual SettingsStatus set_int(const char* key, int32_t value) = 0;

	virtual SettingsStatus get_bool(const char* key, bool& value) const = 0;
	virtual SettingsStatus set_bool(const char* key, bool value) = 0;

	/// Removes the value; not_found if there was none.
	virtual SettingsStatus erase(const char* key) = 0;

	/// Calls `visitor` for every stored entry (order unspecified). The visitor must not modify the
	/// settings.
	virtual void visit(SettingsVisitor& visitor) const = 0;

	/// The shared validation rules every implementation applies before it touches its store.
	static SettingsStatus check_key(const char* key);
	static SettingsStatus check_string(const char* value);

protected:
	~Settings() = default;
};

} // namespace hc

#endif // HC_PLATFORM_SETTINGS_H
