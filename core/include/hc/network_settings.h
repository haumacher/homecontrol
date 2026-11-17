/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_NETWORK_SETTINGS_H
#define HC_NETWORK_SETTINGS_H

#include <cstddef>
#include <cstdint>

#include "hc/platform/settings.h"

namespace hc {

/// The setting keys of the network bring-up (doc/network.md). Persisted names: never rename.
namespace setting {

constexpr const char* kWifiSsid = "wifi.ssid";
constexpr const char* kWifiPsk = "wifi.psk";
constexpr const char* kTimeZone = "tz";
constexpr const char* kNtpServer = "ntp.server";
/// The language of the web UI (doc/i18n.md): a tag `[a-z]{2}(-[A-Z]{2})?`, default kDefaultUiLang.
constexpr const char* kUiLang = "ui.lang";
/// WiFi power save (doc/network.md, doc/zigbee.md "coexistence"): `none`, `min` (modem sleep
/// between DTIM beacons) or `max`; default kDefaultWifiPowerSave. Only the ESP driver acts on it.
constexpr const char* kWifiPowerSave = "wifi.ps";
/// The network name (doc/network.md "Network name"): the DHCP host name, the mDNS host name and the
/// mDNS instance name, `<name>.local`. Picked once when the controller first enters a network
/// (kDefaultNetHostname, or the first of kDefaultNetHostname-2, -3, ... no other host answers for)
/// and stored; changed only by the user (PUT /api/settings/net.hostname). Never the device's
/// identity (MQTT node id, Improv, `device.name`: homecontrol-<MAC>). Rules: network_name_valid().
constexpr const char* kNetHostname = "net.hostname";

/// Central European time with its EU DST rules.
constexpr const char* kDefaultTimeZone = "CET-1CEST,M3.5.0,M10.5.0/3";
constexpr const char* kDefaultNtpServer = "pool.ntp.org";
/// The base language, built into the web UI.
constexpr const char* kDefaultUiLang = "en";
/// Longest language tag ("pt-BR").
constexpr size_t kMaxUiLangLength = 5;
/// WiFi power save: off (the dongle's choice: lowest latency, no lost ACKs), minimum modem sleep
/// (the radio sleeps between DTIM beacons; coexistence with 802.15.4 gets the air meanwhile) or
/// maximum modem sleep (the listen interval of the station config).
enum class WifiPowerSave : uint8_t { none, min, max };
constexpr WifiPowerSave kDefaultWifiPowerSave = WifiPowerSave::min;

/// The network name a controller announces when nothing is stored yet.
constexpr const char* kDefaultNetHostname = "homecontrol";
/// Longest network name (the phoneblock dongle's limit: short enough to type, fits the FRITZ!Box's
/// device list; RFC 1123 allows 63 per label).
constexpr size_t kMaxNetHostnameLength = 32;

/// Longest SSID in bytes (IEEE 802.11).
constexpr size_t kMaxSsidLength = 32;

} // namespace setting

/// Whether a key holds a secret that listings and logs must mask (wifi.psk, the web.* hashes,
/// mqtt.password, ehome.secret, and ehome.key, which is public but shown only as its fingerprint).
bool setting_is_secret(const char* key);

/// An SSID: 1 to 32 bytes. On refusal `error` names the problem.
bool wifi_ssid_valid(const char* ssid, const char** error = nullptr);

/// A WPA2/WPA3 personal key: a passphrase of 8 to 63 printable ASCII characters, or 64 hex digits.
bool wifi_psk_valid(const char* psk, const char** error = nullptr);

/// A host name or IPv4 literal for a server: 1 to Settings::kMaxStringLength characters of
/// [A-Za-z0-9.-], labels of 1 to 63 characters.
bool host_name_valid(const char* name, const char** error = nullptr);

/// A UI language tag: two lower-case letters, optionally `-` and two upper-case letters ("de",
/// "pt-BR"). Safe to splice into a URL or a file name. On refusal `error` names the problem.
bool ui_lang_valid(const char* lang, const char** error = nullptr);

/// The UI language from the settings: ui.lang if it is stored and valid, else kDefaultUiLang (an
/// invalid stored value is logged under `tag`). `buffer` holds kMaxUiLangLength + 1 bytes.
void read_ui_lang(const Settings& settings, char (&buffer)[setting::kMaxUiLangLength + 1], const char* tag);

/// Validates and stores ui.lang (the web API and the console use it). False with `error` pointing
/// to the reason if the tag is invalid or the write fails (nothing stored then).
bool store_ui_lang(Settings& settings, const char* lang, const char** error);

/// A network name as stored (net.hostname): one RFC 1123 label of 1 to kMaxNetHostnameLength
/// characters from [a-z0-9-], not starting or ending with a hyphen (the phoneblock dongle's rules).
/// Upper case is refused here (normalize_network_name() lowers it). On refusal `error` names the
/// problem.
bool network_name_valid(const char* name, const char** error = nullptr);

/// Turns user input into the network name to store: surrounding whitespace dropped, ASCII letters
/// lower-cased, then network_name_valid(). An empty input (or whitespace only) is refused ("a name
/// is required"). False with `error` and `out` set to "" for anything else (underscore, dot,
/// non-ASCII, too long, leading or trailing hyphen).
bool normalize_network_name(const char* input, char (&out)[setting::kMaxNetHostnameLength + 1],
	const char** error = nullptr);

/// The stored network name: true with `out` holding net.hostname if it is stored and valid; false
/// with `out` = "" if none is stored (an unusable stored value is logged under `tag` and counts as
/// none: the controller picks again).
bool read_network_name(const Settings& settings, char (&out)[setting::kMaxNetHostnameLength + 1], const char* tag);

/// Validates (network_name_valid(), no normalization) and stores net.hostname. False with `error`
/// if the name is invalid or the write fails (nothing stored then).
bool store_network_name(Settings& settings, const char* name, const char** error);

/// "none", "min" or "max".
const char* wifi_power_save_name(setting::WifiPowerSave mode);

/// Parses "none", "min" or "max" (case-sensitive); false with `error` naming the choices otherwise.
bool parse_wifi_power_save(const char* text, setting::WifiPowerSave& mode, const char** error = nullptr);

/// A wifi.ps value: one of the three names. On refusal `error` names the choices.
bool wifi_power_save_valid(const char* text, const char** error = nullptr);

/// The WiFi power save from the settings: wifi.ps if stored and valid, else kDefaultWifiPowerSave
/// (an invalid stored value is logged under `tag`).
setting::WifiPowerSave read_wifi_power_save(const Settings& settings, const char* tag);

/// Validates and stores wifi.ps. False with `error` if the value is invalid or the write fails.
bool store_wifi_power_save(Settings& settings, const char* text, const char** error);

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
