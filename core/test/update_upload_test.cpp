/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The firmware upload from the browser (doc/ota.md "Upload from the browser"): the upload
// instruction in the settings, what an updater that predates uploads makes of it, the updater's
// decisions while it waits for the image, the token check and the check of an image's first bytes.

#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "hc/update_check.h"
#include "test_support.h"

using hc::AppImageState;
using hc::InstructionStatus;
using hc::UpdaterAction;

namespace {

constexpr const char* kToken = "0123456789abcdef0123456789abcdef";

/// The instruction reader of the updater as released before uploads existed (update_check.cpp at
/// c3d6c53), kept verbatim in what it decides: it reads `update.image` first and requires a URL.
/// An updater flashed before uploads runs this code, and it is updated over USB only.
InstructionStatus legacy_load(const hc::Settings& settings, const char** error) {
	char image[hc::Settings::kMaxStringLength + 1];
	hc::SettingsStatus status = settings.get_string(hc::setting::kUpdateImage, image, sizeof(image));
	if (status == hc::SettingsStatus::not_found) {
		return InstructionStatus::none;
	}
	if (status != hc::SettingsStatus::ok) {
		*error = "update.image unreadable";
		return InstructionStatus::corrupt;
	}
	if (!hc::update_url_valid(image)) {
		*error = "update.image is not a valid URL";
		return InstructionStatus::corrupt;
	}
	char version[hc::Version::kMaxText + 1];
	hc::Version parsed;
	if (settings.get_string(hc::setting::kUpdateVersion, version, sizeof(version)) != hc::SettingsStatus::ok ||
		!hc::parse_version(version, parsed)) {
		*error = "update.version missing or invalid";
		return InstructionStatus::corrupt;
	}
	char sha[65];
	if (settings.get_string(hc::setting::kUpdateSha256, sha, sizeof(sha)) != hc::SettingsStatus::ok ||
		std::strlen(sha) != 64) {
		*error = "update.sha256 missing or invalid";
		return InstructionStatus::corrupt;
	}
	int32_t size = 0;
	if (settings.get_int(hc::setting::kUpdateSize, size) != hc::SettingsStatus::ok || size <= 0) {
		*error = "update.size missing or invalid";
		return InstructionStatus::corrupt;
	}
	return InstructionStatus::pending;
}

/// What the legacy updater erases when it finds a corrupt instruction (its five keys).
void legacy_clear(hc::Settings& settings) {
	for (const char* key : {hc::setting::kUpdateImage, hc::setting::kUpdateVersion, hc::setting::kUpdateSha256,
			 hc::setting::kUpdateSize, hc::setting::kUpdateAttempts}) {
		settings.erase(key);
	}
}

hc::UpdateManifest manifest(const char* version) {
	hc::UpdateManifest m;
	std::strcpy(m.version, version);
	std::strcpy(m.sha256, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
	m.size = 1239776;
	return m;
}

/// The first bytes of an ESP application image: image header, segment header, app descriptor.
std::vector<uint8_t> image_head(const char* project, uint16_t chip_id, const char* version = "0.2.0") {
	std::vector<uint8_t> data(hc::app_image::kHeadLength + 64, 0);
	data[0] = hc::app_image::kMagic;
	data[hc::app_image::kChipIdOffset] = static_cast<uint8_t>(chip_id & 0xff);
	data[hc::app_image::kChipIdOffset + 1] = static_cast<uint8_t>(chip_id >> 8);
	uint8_t* desc = data.data() + hc::app_image::kDescOffset;
	uint32_t magic = hc::app_image::kDescMagic;
	for (int i = 0; i < 4; ++i) {
		desc[i] = static_cast<uint8_t>(magic >> (8 * i));
	}
	std::memcpy(desc + hc::app_image::kVersionOffset, version, std::strlen(version));
	std::memcpy(desc + hc::app_image::kProjectOffset, project, std::strlen(project)); // 32 bytes, unterminated if full
	return data;
}

constexpr uint16_t kC6 = 13; // ESP_CHIP_ID_ESP32C6

} // namespace

TEST_CASE("UpdateUpload: the upload instruction round-trips with its token and replaces a download") {
	hc_test::FakeSettings settings;
	const char* error = nullptr;
	REQUIRE(hc::store_update_instruction(settings, manifest("0.2.0"), "http://192.0.2.10:8070/homecontrol.bin", &error));
	REQUIRE(settings.set_int(hc::setting::kUpdateAttempts, 2) == hc::SettingsStatus::ok);
	REQUIRE(settings.set_string(hc::setting::kUpdateUploaded, "0.1.0") == hc::SettingsStatus::ok);

	REQUIRE(hc::store_upload_instruction(settings, kToken, &error));
	hc::UpdateInstruction instruction;
	REQUIRE(hc::load_update_instruction(settings, instruction, &error) == InstructionStatus::upload);
	CHECK(std::string(instruction.token) == kToken);
	CHECK(instruction.image_url[0] == '\0');
	CHECK(std::string(hc::instruction_status_name(InstructionStatus::upload)) == "awaiting upload");
	// Nothing of the download is left over, nor an old upload report.
	char text[80];
	CHECK(settings.get_string(hc::setting::kUpdateVersion, text, sizeof(text)) == hc::SettingsStatus::not_found);
	int32_t number = 0;
	CHECK(settings.get_int(hc::setting::kUpdateAttempts, number) == hc::SettingsStatus::not_found);
	CHECK(settings.get_string(hc::setting::kUpdateUploaded, text, sizeof(text)) == hc::SettingsStatus::not_found);

	// The token goes with the instruction.
	CHECK(hc::clear_update_instruction(settings));
	CHECK(hc::load_update_instruction(settings, instruction) == InstructionStatus::none);
	CHECK(settings.get_string(hc::setting::kUpdateToken, text, sizeof(text)) == hc::SettingsStatus::not_found);
	CHECK(settings.entries.empty());
}

TEST_CASE("UpdateUpload: a malformed token is refused and an instruction without one is corrupt") {
	hc_test::FakeSettings settings;
	const char* error = nullptr;
	CHECK_FALSE(hc::store_upload_instruction(settings, "0123", &error));
	CHECK(std::string(error) == "the upload token is not 32 lower-case hex digits");
	CHECK_FALSE(hc::store_upload_instruction(settings, "0123456789ABCDEF0123456789abcdef", &error));
	CHECK(settings.entries.empty());

	REQUIRE(settings.set_string(hc::setting::kUpdateImage, hc::kUploadMarker) == hc::SettingsStatus::ok);
	hc::UpdateInstruction instruction;
	CHECK(hc::load_update_instruction(settings, instruction, &error) == InstructionStatus::corrupt);
	CHECK(std::string(error) == "update.token missing or invalid");
	REQUIRE(settings.set_string(hc::setting::kUpdateToken, "not hex at all, not hex at all!!") == hc::SettingsStatus::ok);
	CHECK(hc::load_update_instruction(settings, instruction, &error) == InstructionStatus::corrupt);
	CHECK(instruction.token[0] == '\0');

	// A write failure leaves no complete instruction.
	hc_test::FakeSettings failing;
	failing.fail_writes = true;
	CHECK_FALSE(hc::store_upload_instruction(failing, kToken, &error));
	CHECK(hc::load_update_instruction(failing, instruction) == InstructionStatus::none);
}

TEST_CASE("UpdateUpload: an updater that predates uploads reads the upload instruction as corrupt") {
	hc_test::FakeSettings settings;
	const char* error = nullptr;
	// A download instruction of today's application: both readers see it pending.
	REQUIRE(hc::store_update_instruction(settings, manifest("0.2.0"), "https://updates.example/homecontrol.bin"));
	CHECK(legacy_load(settings, &error) == InstructionStatus::pending);
	hc::UpdateInstruction instruction;
	CHECK(hc::load_update_instruction(settings, instruction) == InstructionStatus::pending);

	// The upload instruction: never a download for the old reader, but corrupt, so it erases it and
	// (with a valid ota_0) boots the application, which then finds the token alone.
	REQUIRE(hc::store_upload_instruction(settings, kToken));
	CHECK(legacy_load(settings, &error) == InstructionStatus::corrupt);
	CHECK(std::string(error) == "update.image is not a valid URL");
	CHECK(hc::decide_updater_step(InstructionStatus::corrupt, 0, AppImageState::valid).action == UpdaterAction::boot_app);
	legacy_clear(settings);
	CHECK(hc::load_update_instruction(settings, instruction) == InstructionStatus::none);
	char token[40];
	CHECK(settings.get_string(hc::setting::kUpdateToken, token, sizeof(token)) == hc::SettingsStatus::ok);
	CHECK(hc::clear_update_instruction(settings));
	CHECK(settings.entries.empty());
}

TEST_CASE("UpdateUpload: the updater serves an upload for 10 minutes, without a limit when it has nothing to boot") {
	hc::UpdaterStep step = hc::decide_updater_step(InstructionStatus::upload, 0, AppImageState::valid);
	CHECK(step.action == UpdaterAction::serve_upload);
	CHECK(step.delay_ms == 0);
	CHECK(step.timeout_ms == hc::kUploadTimeoutMs);
	CHECK(hc::kUploadTimeoutMs == 600000u);
	for (AppImageState app : {AppImageState::missing, AppImageState::aborted}) {
		CAPTURE(hc::app_image_state_name(app));
		step = hc::decide_updater_step(InstructionStatus::upload, 0, app);
		CHECK(step.action == UpdaterAction::serve_upload);
		CHECK(step.timeout_ms == 0);
	}
	// The attempt counter of downloads plays no part.
	CHECK(hc::decide_updater_step(InstructionStatus::upload, 5, AppImageState::valid).timeout_ms == hc::kUploadTimeoutMs);
	CHECK(std::string(hc::updater_action_name(UpdaterAction::serve_upload)) == "serve upload");

	// The timeout ran out: boot a valid ota_0; serve on otherwise, and while an upload is written.
	step = hc::decide_upload_expired(AppImageState::valid, false);
	CHECK(step.action == UpdaterAction::boot_app);
	step = hc::decide_upload_expired(AppImageState::missing, false);
	CHECK(step.action == UpdaterAction::serve_upload);
	CHECK(step.timeout_ms == 0);
	CHECK(hc::decide_upload_expired(AppImageState::aborted, false).action == UpdaterAction::serve_upload);
	CHECK(hc::decide_upload_expired(AppImageState::valid, true).action == UpdaterAction::serve_upload);

	// The download rows are unchanged.
	CHECK(hc::decide_updater_step(InstructionStatus::pending, 0, AppImageState::valid).action == UpdaterAction::download);
	CHECK(hc::decide_updater_step(InstructionStatus::none, 0, AppImageState::valid).action == UpdaterAction::boot_app);
	CHECK(hc::decide_updater_step(InstructionStatus::none, 0, AppImageState::missing).action == UpdaterAction::wait);
}

TEST_CASE("UpdateUpload: only the bearer of the stored token may upload") {
	CHECK(hc::upload_token_valid(kToken));
	CHECK_FALSE(hc::upload_token_valid(""));
	CHECK_FALSE(hc::upload_token_valid(nullptr));
	CHECK_FALSE(hc::upload_token_valid("0123456789abcdef0123456789abcdef0"));
	CHECK_FALSE(hc::upload_token_valid("0123456789abcdef0123456789abcdeg"));

	std::string bearer = std::string("Bearer ") + kToken;
	CHECK(hc::upload_authorized(kToken, bearer.c_str()));
	CHECK_FALSE(hc::upload_authorized(kToken, nullptr));
	CHECK_FALSE(hc::upload_authorized(kToken, kToken));
	CHECK_FALSE(hc::upload_authorized(kToken, "Bearer "));
	CHECK_FALSE(hc::upload_authorized(kToken, "Bearer 0123456789abcdef0123456789abcdee"));
	CHECK_FALSE(hc::upload_authorized(kToken, (bearer + "0").c_str()));
	CHECK_FALSE(hc::upload_authorized(kToken, "Basic 0123456789abcdef0123456789abcdef"));
	CHECK_FALSE(hc::upload_authorized(kToken, "Bearer hct_0123456789abcdef0123456789abcdef"));
	// No stored token (erased after the upload): nothing passes, not even an empty bearer.
	CHECK_FALSE(hc::upload_authorized("", "Bearer "));
	CHECK_FALSE(hc::upload_authorized("", bearer.c_str()));
}

TEST_CASE("UpdateUpload: the first bytes of an image say whether it is the application for this chip") {
	char reason[160];
	hc::AppImageInfo info;
	std::vector<uint8_t> good = image_head("homecontrol", kC6, "0.2.0-dev");
	REQUIRE(hc::check_app_image(good.data(), good.size(), "homecontrol", kC6, info, reason, sizeof(reason)));
	CHECK(std::string(info.project) == "homecontrol");
	CHECK(std::string(info.version) == "0.2.0-dev");
	CHECK(info.chip_id == kC6);
	CHECK(hc::check_app_image(good.data(), hc::app_image::kHeadLength, "homecontrol", kC6, info, reason, sizeof(reason)));

	CHECK_FALSE(hc::check_app_image(good.data(), hc::app_image::kHeadLength - 1, "homecontrol", kC6, info, reason,
		sizeof(reason)));
	CHECK(std::string(reason) == "image shorter than its header (287 bytes)");

	std::vector<uint8_t> page(400, '<');
	CHECK_FALSE(hc::check_app_image(page.data(), page.size(), "homecontrol", kC6, info, reason, sizeof(reason)));
	CHECK(std::string(reason) == "not an ESP firmware image (magic 0x3c; an error page?)");

	std::vector<uint8_t> s3 = image_head("homecontrol", 9);
	CHECK_FALSE(hc::check_app_image(s3.data(), s3.size(), "homecontrol", kC6, info, reason, sizeof(reason)));
	CHECK(std::string(reason) == "image is for another chip (chip id 9, this is 13)");

	std::vector<uint8_t> bare = image_head("homecontrol", kC6);
	bare[hc::app_image::kDescOffset] = 0;
	CHECK_FALSE(hc::check_app_image(bare.data(), bare.size(), "homecontrol", kC6, info, reason, sizeof(reason)));
	CHECK(std::string(reason) == "image has no app descriptor");

	std::vector<uint8_t> updater = image_head("hc_updater", kC6);
	CHECK_FALSE(hc::check_app_image(updater.data(), updater.size(), "homecontrol", kC6, info, reason, sizeof(reason)));
	CHECK(std::string(reason) == "image is \"hc_updater\", not the application \"homecontrol\"");

	// A project name that fills its 32 bytes is cut there, never read past.
	std::vector<uint8_t> full = image_head("homecontrol-and-a-very-long-name", kC6);
	CHECK_FALSE(hc::check_app_image(full.data(), full.size(), "homecontrol", kC6, info, reason, sizeof(reason)));
	CHECK(std::string(info.project) == "homecontrol-and-a-very-long-name");
}

TEST_CASE("UpdateUpload: the application recognises an updater that serves uploads by its version") {
	CHECK(hc::updater_supports_upload("0.1.0-dev+upload"));
	CHECK(hc::updater_supports_upload("0.2.0+g1a2b3c4.upload"));
	CHECK(hc::updater_supports_upload("0.2.0+upload.2"));
	CHECK_FALSE(hc::updater_supports_upload("0.1.0-dev"));
	CHECK_FALSE(hc::updater_supports_upload("0.1.0-upload"));
	CHECK_FALSE(hc::updater_supports_upload("0.1.0+uploads"));
	CHECK_FALSE(hc::updater_supports_upload(""));
	CHECK_FALSE(hc::updater_supports_upload(nullptr));
	CHECK_FALSE(hc::updater_supports_upload("garbage+upload"));
}
