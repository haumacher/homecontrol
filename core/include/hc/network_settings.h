/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_NETWORK_SETTINGS_H
#define HC_NETWORK_SETTINGS_H

#include <cstddef>

#include "hc/platform/settings.h"

namespace hc {

/// The setting keys of the network bring-up (doc/network.md). Persisted names: never rename.
namespace setting {

constexpr const char* kWifiSsid = "wifi.ssid";
constexpr const char* kWifiPsk = "wifi.psk";
constexpr const char* kTimeZone = "tz";
constexpr const char* kNtpServer = "ntp.server";

/// Central European time with its EU DST rules.
constexpr const char* kDefaultTimeZone = "CET-1CEST,M3.5.0,M10.5.0/3";
constexpr const char* kDefaultNtpServer = "pool.ntp.org";

/// Longest SSID in bytes (IEEE 802.11).
constexpr size_t kMaxSsidLength = 32;

} // namespace setting

/// Whether a key holds a secret that listings and logs must mask (wifi.psk).
bool setting_is_secret(const char* key);

/// An SSID: 1 to 32 bytes. On refusal `error` names the problem.
bool wifi_ssid_valid(const char* ssid, const char** error = nullptr);

/// A WPA2/WPA3 personal key: a passphrase of 8 to 63 printable ASCII characters, or 64 hex digits.
bool wifi_psk_valid(const char* psk, const char** error = nullptr);

/// A host name or IPv4 literal for a server: 1 to Settings::kMaxStringLength characters of
/// [A-Za-z0-9.-], labels of 1 to 63 characters.
bool host_name_valid(const char* name, const char** error = nullptr);

/// Validates and stores WiFi credentials the one way every provisioning path (the console's
/// `wifi set`, WPS) uses: wifi.ssid, and wifi.psk or, for an open network (`psk` null or empty),
/// no wifi.psk. Returns false with `error` pointing to the reason (nothing stored if the
/// validation fails). Never logs the key.
bool store_wifi_credentials(Settings& settings, const char* ssid, const char* psk, const char** error);

/// Reads the string `key` into `buffer`; if nothing usable is stored, copies `fallback` instead.
/// A stored value that cannot be used (wrong type, too long, fails `validate`) is logged as an
/// error under `tag` and replaced by the fallback; a missing value is not an error. Returns true if
/// the stored value was used.
bool read_string_setting(const Settings& settings, const char* key, const char* fallback, char* buffer,
	size_t size, const char* tag, bool (*validate)(const char*, const char**) = nullptr);

} // namespace hc

#endif // HC_NETWORK_SETTINGS_H
