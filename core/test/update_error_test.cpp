/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The updater's reason for its last failed attempt (homecontrol #96): stored bounded in
// `update.error`, read with the instruction, appended to the application's report of an abandoned
// update or upload, and gone with the instruction or a new one.
#include <cstring>
#include <string>

#include "doctest.h"
#include "hc/update_check.h"
#include "test_support.h"

using hc::InstructionStatus;
using hc::SettingsStatus;

namespace {

constexpr const char* kToken = "0123456789abcdef0123456789abcdef";

hc::UpdateManifest manifest(const char* version) {
	hc::UpdateManifest m;
	std::strcpy(m.version, version);
	std::strcpy(m.sha256, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
	m.size = 1239776;
	return m;
}

std::string report(InstructionStatus status, const hc::UpdateInstruction& instruction) {
	char out[256];
	size_t length = hc::format_abandoned_report(status, instruction, out, sizeof(out));
	CHECK(length == std::strlen(out));
	return out;
}

std::string stored(const hc_test::FakeSettings& settings) {
	char text[hc::Settings::kMaxStringLength + 1];
	if (settings.get_string(hc::setting::kUpdateError, text, sizeof(text)) != SettingsStatus::ok) {
		return "<none>";
	}
	return text;
}

} // namespace

TEST_CASE("UpdateError: the key is the persisted name") {
	CHECK(std::string(hc::setting::kUpdateError) == "update.error");
	CHECK(std::strlen(hc::setting::kUpdateError) <= hc::Settings::kMaxKeyLength);
}

TEST_CASE("UpdateError: the report of an abandoned download, without and with the updater's reason") {
	hc_test::FakeSettings settings;
	const char* error = nullptr;
	REQUIRE(hc::store_update_instruction(settings, manifest("1.5.0"), "http://192.0.2.10:8070/homecontrol.bin", &error));
	REQUIRE(settings.set_int(hc::setting::kUpdateAttempts, 3) == SettingsStatus::ok);
	hc::UpdateInstruction instruction;
	REQUIRE(hc::load_update_instruction(settings, instruction, &error) == InstructionStatus::pending);
	CHECK(instruction.error[0] == '\0');
	// An updater that predates update.error: the report as before.
	CHECK(report(InstructionStatus::pending, instruction) == "update to 1.5.0 abandoned after 3 attempt(s)");

	REQUIRE(hc::store_update_error(settings, "cannot connect: ESP_ERR_HTTP_CONNECT") == SettingsStatus::ok);
	REQUIRE(hc::load_update_instruction(settings, instruction, &error) == InstructionStatus::pending);
	CHECK(std::string(instruction.error) == "cannot connect: ESP_ERR_HTTP_CONNECT");
	CHECK(report(InstructionStatus::pending, instruction) ==
		"update to 1.5.0 abandoned after 3 attempt(s): cannot connect: ESP_ERR_HTTP_CONNECT");

	// The next attempt's reason replaces the one before.
	REQUIRE(hc::store_update_error(settings, "network stack not started: ESP_ERR_NO_MEM") == SettingsStatus::ok);
	REQUIRE(hc::load_update_instruction(settings, instruction, &error) == InstructionStatus::pending);
	CHECK(report(InstructionStatus::pending, instruction) ==
		"update to 1.5.0 abandoned after 3 attempt(s): network stack not started: ESP_ERR_NO_MEM");
}

TEST_CASE("UpdateError: the report of an abandoned upload, without and with the updater's reason") {
	hc_test::FakeSettings settings;
	const char* error = nullptr;
	REQUIRE(hc::store_upload_instruction(settings, kToken, &error));
	hc::UpdateInstruction instruction;
	REQUIRE(hc::load_update_instruction(settings, instruction, &error) == InstructionStatus::upload);
	CHECK(report(InstructionStatus::upload, instruction) == "firmware upload abandoned: no image was installed");
	REQUIRE(hc::store_update_error(settings, "WiFi not started (ESP_ERR_WIFI_NOT_INIT)") == SettingsStatus::ok);
	REQUIRE(hc::load_update_instruction(settings, instruction, &error) == InstructionStatus::upload);
	CHECK(report(InstructionStatus::upload, instruction) ==
		"firmware upload abandoned: no image was installed: WiFi not started (ESP_ERR_WIFI_NOT_INIT)");
}

TEST_CASE("UpdateError: cleared with the instruction and by a new one, never left behind") {
	hc_test::FakeSettings settings;
	const char* error = nullptr;
	REQUIRE(hc::store_update_instruction(settings, manifest("1.5.0"), "http://h/a.bin", &error));
	REQUIRE(hc::store_update_error(settings, "HTTP status 404") == SettingsStatus::ok);

	// A new download instruction starts without the old reason.
	REQUIRE(hc::store_update_instruction(settings, manifest("1.5.1"), "http://h/b.bin", &error));
	CHECK(stored(settings) == "<none>");
	hc::UpdateInstruction instruction;
	REQUIRE(hc::load_update_instruction(settings, instruction, &error) == InstructionStatus::pending);
	CHECK(instruction.error[0] == '\0');

	// So does an upload instruction.
	REQUIRE(hc::store_update_error(settings, "HTTP status 404") == SettingsStatus::ok);
	REQUIRE(hc::store_upload_instruction(settings, kToken, &error));
	CHECK(stored(settings) == "<none>");

	// The application's clearing after its confirmed start (completed or abandoned) erases it.
	REQUIRE(hc::store_update_error(settings, "attempt 1 interrupted: the updater restarted") == SettingsStatus::ok);
	REQUIRE(hc::clear_update_instruction(settings));
	CHECK(stored(settings) == "<none>");
	CHECK(settings.entries.empty());

	// A failed write of the new instruction's erase is refused with the reason.
	REQUIRE(hc::store_update_error(settings, "x") == SettingsStatus::ok);
	settings.fail_writes = true;
	CHECK_FALSE(hc::store_update_instruction(settings, manifest("1.5.2"), "http://h/c.bin", &error));
}

TEST_CASE("UpdateError: the reason is bounded and stays a storable string") {
	hc_test::FakeSettings settings;
	// Longer than a setting may be: cut at Settings::kMaxStringLength bytes.
	std::string longer(300, 'x');
	REQUIRE(hc::store_update_error(settings, longer.c_str()) == SettingsStatus::ok);
	CHECK(stored(settings) == std::string(hc::Settings::kMaxStringLength, 'x'));

	// The cut never splits a character: "ü" (2 bytes) across the limit goes as a whole.
	std::string umlauts(hc::Settings::kMaxStringLength - 1, 'a');
	umlauts += "\xC3\xBC\xC3\xBC";
	REQUIRE(hc::store_update_error(settings, umlauts.c_str()) == SettingsStatus::ok);
	CHECK(stored(settings) == std::string(hc::Settings::kMaxStringLength - 1, 'a'));
	// Exactly at the limit with the character complete: kept.
	std::string exact(hc::Settings::kMaxStringLength - 2, 'a');
	exact += "\xC3\xBC";
	REQUIRE(hc::store_update_error(settings, exact.c_str()) == SettingsStatus::ok);
	CHECK(stored(settings) == exact);

	// Control characters, which a setting refuses, become spaces instead of losing the reason.
	REQUIRE(hc::store_update_error(settings, "line one\nline\ttwo\x7f") == SettingsStatus::ok);
	CHECK(stored(settings) == "line one line two ");

	// No reason: an empty one, which the report leaves out.
	REQUIRE(hc::store_update_error(settings, nullptr) == SettingsStatus::ok);
	CHECK(stored(settings).empty());
	const char* error = nullptr;
	REQUIRE(hc::store_upload_instruction(settings, kToken, &error));
	REQUIRE(hc::store_update_error(settings, "") == SettingsStatus::ok);
	hc::UpdateInstruction instruction;
	REQUIRE(hc::load_update_instruction(settings, instruction, &error) == InstructionStatus::upload);
	CHECK(report(InstructionStatus::upload, instruction) == "firmware upload abandoned: no image was installed");

	// A failed write is reported, not hidden.
	settings.fail_writes = true;
	CHECK(hc::store_update_error(settings, "x") == SettingsStatus::storage_error);
}

TEST_CASE("UpdateError: the longest report fits the application's buffer, a small buffer is cut and terminated") {
	hc_test::FakeSettings settings;
	const char* error = nullptr;
	// The longest version a manifest may carry.
	std::string version = "1.0.0-" + std::string(hc::Version::kMaxText - 6, 'a');
	REQUIRE(hc::store_update_instruction(settings, manifest(version.c_str()), "http://h/a.bin", &error));
	REQUIRE(settings.set_int(hc::setting::kUpdateAttempts, 2147483647) == SettingsStatus::ok);
	REQUIRE(hc::store_update_error(settings, std::string(400, 'r').c_str()) == SettingsStatus::ok);
	hc::UpdateInstruction instruction;
	REQUIRE(hc::load_update_instruction(settings, instruction, &error) == InstructionStatus::pending);
	std::string whole = report(InstructionStatus::pending, instruction);
	CHECK(whole.size() < 256); // OtaService's last_ holds it whole
	CHECK(whole.substr(whole.size() - hc::Settings::kMaxStringLength) == std::string(hc::Settings::kMaxStringLength, 'r'));

	char small[20];
	CHECK(hc::format_abandoned_report(InstructionStatus::pending, instruction, small, sizeof(small)) == 19);
	CHECK(std::string(small) == "update to 1.0.0-aaa");
	char tiny[1];
	CHECK(hc::format_abandoned_report(InstructionStatus::pending, instruction, tiny, sizeof(tiny)) == 0);
	CHECK(tiny[0] == '\0');
}

TEST_CASE("UpdateError: an unreadable reason leaves the instruction usable") {
	hc_test::FakeSettings settings;
	const char* error = nullptr;
	REQUIRE(hc::store_update_instruction(settings, manifest("1.5.0"), "http://h/a.bin", &error));
	// Stored with the wrong type (an older or broken writer): diagnostic only, the instruction stays pending.
	REQUIRE(settings.set_int(hc::setting::kUpdateError, 7) == SettingsStatus::ok);
	hc::UpdateInstruction instruction;
	CHECK(hc::load_update_instruction(settings, instruction, &error) == InstructionStatus::pending);
	CHECK(instruction.error[0] == '\0');
	CHECK(report(InstructionStatus::pending, instruction) == "update to 1.5.0 abandoned after 0 attempt(s)");
}
