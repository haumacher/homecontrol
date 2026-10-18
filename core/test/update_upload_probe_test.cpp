/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe of the firmware upload (issue #39) composed with the download path and the token check:
// an upload instruction leaves no token behind when a download replaces it or the instruction is
// cleared, and the bearer check accepts exactly one header form.
#include <cstring>
#include <string>

#include "doctest.h"
#include "hc/update_check.h"
#include "test_support.h"

using hc::InstructionStatus;

namespace {
constexpr const char* kToken = "0123456789abcdef0123456789abcdef";
}

TEST_CASE("UpdateUpload probe: a download replacing an upload instruction, and clearing it, leave no token") {
	hc_test::FakeSettings settings;
	const char* error = nullptr;
	REQUIRE(hc::store_upload_instruction(settings, kToken, &error));
	hc::UpdateInstruction instruction;
	REQUIRE(hc::load_update_instruction(settings, instruction, &error) == InstructionStatus::upload);
	CHECK(std::string(instruction.token) == kToken);

	// A download instruction takes its place: the reader sees the download, the token is gone.
	hc::UpdateManifest manifest;
	std::strcpy(manifest.version, "0.2.0");
	std::strcpy(manifest.sha256, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
	manifest.size = 1239776;
	REQUIRE(hc::store_update_instruction(settings, manifest, "http://updates.example/0.2.0.bin", &error));
	REQUIRE(hc::load_update_instruction(settings, instruction, &error) == InstructionStatus::pending);
	CHECK(instruction.token[0] == '\0');
	char token[hc::kUploadTokenLength + 1];
	CHECK(settings.get_string(hc::setting::kUpdateToken, token, sizeof(token)) == hc::SettingsStatus::not_found);

	// And the other way round: an upload replaces the download, clearing erases everything.
	REQUIRE(hc::store_upload_instruction(settings, kToken, &error));
	REQUIRE(hc::load_update_instruction(settings, instruction, &error) == InstructionStatus::upload);
	REQUIRE(hc::clear_update_instruction(settings));
	CHECK(hc::load_update_instruction(settings, instruction, &error) == InstructionStatus::none);
	CHECK(settings.get_string(hc::setting::kUpdateToken, token, sizeof(token)) == hc::SettingsStatus::not_found);
	CHECK(settings.get_string(hc::setting::kUpdateImage, token, sizeof(token)) == hc::SettingsStatus::not_found);
}

TEST_CASE("UpdateUpload probe: the bearer check takes exactly `Bearer <token>`") {
	CHECK(hc::upload_authorized(kToken, "Bearer 0123456789abcdef0123456789abcdef"));
	CHECK_FALSE(hc::upload_authorized(kToken, "bearer 0123456789abcdef0123456789abcdef"));
	CHECK_FALSE(hc::upload_authorized(kToken, "Bearer  0123456789abcdef0123456789abcdef"));
	CHECK_FALSE(hc::upload_authorized(kToken, "Bearer 0123456789abcdef0123456789abcdeF"));
	CHECK_FALSE(hc::upload_authorized(kToken, "Bearer 0123456789abcdef0123456789abcdef0"));
	CHECK_FALSE(hc::upload_authorized(kToken, "Bearer 0123456789abcdef0123456789abcde"));
	CHECK_FALSE(hc::upload_authorized(kToken, "0123456789abcdef0123456789abcdef"));
	CHECK_FALSE(hc::upload_authorized(kToken, ""));
	CHECK_FALSE(hc::upload_authorized(kToken, nullptr));
	CHECK_FALSE(hc::upload_authorized("", "Bearer "));
	CHECK_FALSE(hc::upload_authorized(nullptr, "Bearer 0123456789abcdef0123456789abcdef"));
}
