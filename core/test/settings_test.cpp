/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The Settings contract, exercised on the in-memory fake (which shares the validation rules of
// the NVS and file implementations), and the validators of the network settings.

#include <string>

#include "doctest.h"
#include "hc/network_settings.h"
#include "hc/platform/settings.h"
#include "test_support.h"

using hc::Settings;
using hc::SettingsStatus;
using hc_test::FakeSettings;
using hc_test::ScopedCapture;

TEST_CASE("Settings.KeyRules") {
	CHECK(Settings::check_key("wifi.ssid") == SettingsStatus::ok);
	CHECK(Settings::check_key("a") == SettingsStatus::ok);
	CHECK(Settings::check_key("A_b-c.9") == SettingsStatus::ok);
	CHECK(Settings::check_key("fifteen.chars.x") == SettingsStatus::ok);
	CHECK(Settings::check_key("sixteen.chars.xx") == SettingsStatus::invalid_key);
	CHECK(Settings::check_key("") == SettingsStatus::invalid_key);
	CHECK(Settings::check_key(nullptr) == SettingsStatus::invalid_key);
	CHECK(Settings::check_key("with space") == SettingsStatus::invalid_key);
	CHECK(Settings::check_key("a=b") == SettingsStatus::invalid_key);
}

TEST_CASE("Settings.StringRules") {
	CHECK(Settings::check_string("") == SettingsStatus::ok);
	CHECK(Settings::check_string(std::string(Settings::kMaxStringLength, 'x').c_str()) == SettingsStatus::ok);
	CHECK(Settings::check_string(std::string(Settings::kMaxStringLength + 1, 'x').c_str()) ==
		SettingsStatus::too_long);
	CHECK(Settings::check_string("line\nbreak") == SettingsStatus::invalid_value);
	CHECK(Settings::check_string("ümlaut and = and \\") == SettingsStatus::ok);
	CHECK(Settings::check_string(nullptr) == SettingsStatus::invalid_value);
}

TEST_CASE("Settings.TypedRoundTripAndTypeChecks") {
	FakeSettings s;
	char buffer[16];
	CHECK(s.get_string("missing", buffer, sizeof(buffer)) == SettingsStatus::not_found);
	CHECK(s.set_string("name", "value") == SettingsStatus::ok);
	CHECK(s.set_int("count", -42) == SettingsStatus::ok);
	CHECK(s.set_bool("flag", true) == SettingsStatus::ok);

	CHECK(s.get_string("name", buffer, sizeof(buffer)) == SettingsStatus::ok);
	CHECK(std::string(buffer) == "value");
	int32_t number = 0;
	CHECK(s.get_int("count", number) == SettingsStatus::ok);
	CHECK(number == -42);
	bool flag = false;
	CHECK(s.get_bool("flag", flag) == SettingsStatus::ok);
	CHECK(flag);

	CHECK(s.get_int("name", number) == SettingsStatus::wrong_type);
	CHECK(s.get_bool("count", flag) == SettingsStatus::wrong_type);
	CHECK(s.get_string("flag", buffer, sizeof(buffer)) == SettingsStatus::wrong_type);

	// A set replaces the value whatever its previous type.
	CHECK(s.set_int("name", 7) == SettingsStatus::ok);
	CHECK(s.get_int("name", number) == SettingsStatus::ok);
	CHECK(number == 7);
}

TEST_CASE("Settings.RefusalsLeaveTheStoreUnchanged") {
	FakeSettings s;
	CHECK(s.set_string("k", "old") == SettingsStatus::ok);
	CHECK(s.set_string("k", std::string(200, 'x').c_str()) == SettingsStatus::too_long);
	CHECK(s.set_string("sixteen.chars.xx", "v") == SettingsStatus::invalid_key);
	CHECK(s.set_int("bad key", 1) == SettingsStatus::invalid_key);
	char small[3];
	CHECK(s.get_string("k", small, sizeof(small)) == SettingsStatus::too_long);
	CHECK(std::string(small).empty());
	char buffer[8];
	CHECK(s.get_string("k", buffer, sizeof(buffer)) == SettingsStatus::ok);
	CHECK(std::string(buffer) == "old");
	CHECK(s.entries.size() == 1);
}

TEST_CASE("Settings.EraseAndVisit") {
	FakeSettings s;
	s.set_string("b", "x");
	s.set_int("a", 1);
	s.set_bool("c", false);
	struct Collector : hc::SettingsVisitor {
		void entry(const char* key, hc::SettingType type) override {
			seen += std::string(key) + ":" + hc::setting_type_name(type) + " ";
		}
		std::string seen;
	} collector;
	s.visit(collector);
	CHECK(collector.seen == "a:integer b:string c:boolean ");
	CHECK(s.erase("b") == SettingsStatus::ok);
	CHECK(s.erase("b") == SettingsStatus::not_found);
	CHECK(s.entries.size() == 2);
}

TEST_CASE("Settings.StatusNamesSpeak") {
	CHECK(std::string(hc::settings_status_name(SettingsStatus::too_long)) == "value too long (at most 127 bytes)");
	CHECK(std::string(hc::settings_status_name(SettingsStatus::wrong_type)) == "stored with another type");
}

TEST_CASE("NetworkSettings.SsidRules") {
	CHECK(hc::wifi_ssid_valid("x"));
	CHECK(hc::wifi_ssid_valid(std::string(32, 's').c_str()));
	const char* error = nullptr;
	CHECK_FALSE(hc::wifi_ssid_valid(std::string(33, 's').c_str(), &error));
	CHECK(std::string(error) == "SSID is longer than 32 bytes");
	CHECK_FALSE(hc::wifi_ssid_valid("", &error));
	CHECK(std::string(error) == "SSID is empty");
}

TEST_CASE("NetworkSettings.PskRules") {
	CHECK_FALSE(hc::wifi_psk_valid("1234567"));
	CHECK(hc::wifi_psk_valid("12345678"));
	CHECK(hc::wifi_psk_valid(std::string(63, 'p').c_str()));
	CHECK(hc::wifi_psk_valid(std::string(64, 'a').c_str()));
	CHECK(hc::wifi_psk_valid(std::string(32, 'F').append(32, '0').c_str()));
	CHECK_FALSE(hc::wifi_psk_valid(std::string(64, 'p').c_str()));
	CHECK_FALSE(hc::wifi_psk_valid(std::string(65, 'a').c_str()));
	CHECK_FALSE(hc::wifi_psk_valid("tab\there12"));
	CHECK_FALSE(hc::wifi_psk_valid(nullptr));
}

TEST_CASE("NetworkSettings.HostNameRules") {
	CHECK(hc::host_name_valid("pool.ntp.org"));
	CHECK(hc::host_name_valid("ptbtime1.ptb.de"));
	CHECK(hc::host_name_valid("192.0.2.1"));
	CHECK(hc::host_name_valid("fritz-box"));
	CHECK_FALSE(hc::host_name_valid(""));
	CHECK_FALSE(hc::host_name_valid("bad..name"));
	CHECK_FALSE(hc::host_name_valid(".lead"));
	CHECK_FALSE(hc::host_name_valid("trail."));
	CHECK_FALSE(hc::host_name_valid("under_score"));
	CHECK_FALSE(hc::host_name_valid(std::string(64, 'l').c_str()));
	CHECK(hc::host_name_valid(std::string(63, 'l').c_str()));
}

TEST_CASE("NetworkSettings.PskIsSecret") {
	CHECK(hc::setting_is_secret("wifi.psk"));
	CHECK_FALSE(hc::setting_is_secret("wifi.ssid"));
	CHECK_FALSE(hc::setting_is_secret(nullptr));
}

TEST_CASE("NetworkSettings.ReadStringSettingFallsBackVisibly") {
	ScopedCapture capture;
	FakeSettings s;
	char buffer[32];
	CHECK_FALSE(hc::read_string_setting(s, "ntp.server", "pool.ntp.org", buffer, sizeof(buffer), "hc.t",
		&hc::host_name_valid));
	CHECK(std::string(buffer) == "pool.ntp.org");
	CHECK(capture.sink.lines.empty()); // missing is not an error

	s.set_string("ntp.server", "time.example");
	CHECK(hc::read_string_setting(s, "ntp.server", "pool.ntp.org", buffer, sizeof(buffer), "hc.t",
		&hc::host_name_valid));
	CHECK(std::string(buffer) == "time.example");

	s.set_string("ntp.server", "not a host");
	CHECK_FALSE(hc::read_string_setting(s, "ntp.server", "pool.ntp.org", buffer, sizeof(buffer), "hc.t",
		&hc::host_name_valid));
	CHECK(std::string(buffer) == "pool.ntp.org");
	CHECK(capture.sink.contains("E hc.t: setting ntp.server is invalid (host name may contain only"));

	s.set_string("ntp.server", std::string(40, 'h').c_str());
	CHECK_FALSE(hc::read_string_setting(s, "ntp.server", "pool.ntp.org", buffer, sizeof(buffer), "hc.t"));
	CHECK(capture.sink.contains("E hc.t: setting ntp.server unreadable (value too long"));
}

TEST_CASE("NetworkSettings.StoreWifiCredentials") {
	FakeSettings s;
	const char* error = nullptr;
	CHECK(hc::store_wifi_credentials(s, "Example Net", "correct horse", &error));
	char buffer[80];
	CHECK(s.get_string("wifi.ssid", buffer, sizeof(buffer)) == SettingsStatus::ok);
	CHECK(std::string(buffer) == "Example Net");
	CHECK(s.get_string("wifi.psk", buffer, sizeof(buffer)) == SettingsStatus::ok);
	CHECK(std::string(buffer) == "correct horse");

	// Open network: the old key goes.
	CHECK(hc::store_wifi_credentials(s, "Open Net", nullptr, &error));
	CHECK(s.get_string("wifi.psk", buffer, sizeof(buffer)) == SettingsStatus::not_found);
	CHECK(hc::store_wifi_credentials(s, "Open Net", "", &error));

	// Refusals store nothing.
	CHECK_FALSE(hc::store_wifi_credentials(s, "Other", "short", &error));
	CHECK(std::string(error) == "passphrase must have 8 to 63 characters");
	CHECK_FALSE(hc::store_wifi_credentials(s, "", "long enough key", &error));
	CHECK(s.get_string("wifi.ssid", buffer, sizeof(buffer)) == SettingsStatus::ok);
	CHECK(std::string(buffer) == "Open Net");

	s.fail_writes = true;
	CHECK_FALSE(hc::store_wifi_credentials(s, "Other", "long enough key", &error));
	CHECK(std::string(error) == "storage error");
}
