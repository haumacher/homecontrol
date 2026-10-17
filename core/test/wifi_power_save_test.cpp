/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The setting wifi.ps (doc/network.md): its values, its default, and the web API applying it to the
// network driver at once.

#include <string>

#include "doctest.h"
#include "hc/network_settings.h"
#include "web_test_support.h"

using hc::setting::WifiPowerSave;

TEST_CASE("wifi.ps: the three names and nothing else") {
	WifiPowerSave mode = WifiPowerSave::max;
	const char* error = nullptr;
	CHECK(hc::parse_wifi_power_save("none", mode, &error));
	CHECK(mode == WifiPowerSave::none);
	CHECK(hc::parse_wifi_power_save("min", mode));
	CHECK(mode == WifiPowerSave::min);
	CHECK(hc::parse_wifi_power_save("max", mode));
	CHECK(mode == WifiPowerSave::max);
	CHECK_FALSE(hc::parse_wifi_power_save("MIN", mode, &error));
	CHECK(std::string(error).find("none, min or max") != std::string::npos);
	CHECK_FALSE(hc::wifi_power_save_valid(""));
	CHECK_FALSE(hc::wifi_power_save_valid(nullptr));
	CHECK(std::string(hc::wifi_power_save_name(WifiPowerSave::min)) == "min");
	CHECK(std::string(hc::wifi_power_save_name(hc::setting::kDefaultWifiPowerSave)) == "none");
}

TEST_CASE("wifi.ps: read falls back to the default for a missing or invalid value, store validates") {
	hc_test::FakeSettings settings;
	CHECK(hc::read_wifi_power_save(settings, "test") == hc::setting::kDefaultWifiPowerSave);
	settings.set_string(hc::setting::kWifiPowerSave, "sometimes");
	CHECK(hc::read_wifi_power_save(settings, "test") == hc::setting::kDefaultWifiPowerSave);
	const char* error = nullptr;
	CHECK_FALSE(hc::store_wifi_power_save(settings, "off", &error));
	CHECK(settings.entries[hc::setting::kWifiPowerSave].text == "sometimes"); // nothing stored on refusal
	CHECK(hc::store_wifi_power_save(settings, "min", &error));
	CHECK(hc::read_wifi_power_save(settings, "test") == WifiPowerSave::min);
}

TEST_CASE("wifi.ps: the web API lists it with the value in use, and a change reaches the driver at once") {
	hc_test::WebFixture f;
	std::string cookie = f.onboard();
	auto list = f.get("/api/settings", cookie);
	bool found = false;
	for (const auto& s : list.json["settings"].items) {
		if (s["key"].text == "wifi.ps") {
			found = true;
			CHECK(s["value"].text == "none");
			CHECK(s["default"].text == "none");
			CHECK(s["editable"].boolean);
		}
	}
	CHECK(found);

	auto bad = f.request(hc::HttpMethod::put, "/api/settings/wifi.ps", "{\"value\":\"low\"}", cookie);
	CHECK(bad.status == 422);
	CHECK(bad.json["code"].text == "settings.invalid");
	CHECK(f.fake.network.power_save_changes == 0);
	CHECK(f.fake.settings.entries.count("wifi.ps") == 0);

	auto good = f.request(hc::HttpMethod::put, "/api/settings/wifi.ps", "{\"value\":\"min\"}", cookie);
	REQUIRE(good.status == 200);
	CHECK(f.fake.settings.entries["wifi.ps"].text == "min");
	CHECK(f.fake.network.power_save_changes == 1);
	for (const auto& s : good.json["settings"].items) {
		if (s["key"].text == "wifi.ps") {
			CHECK(s["value"].text == "min");
		}
	}
}
