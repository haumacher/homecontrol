/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe tests written at review time for the update check (issue #3): semantic version edge
// cases, the dev-build rule, min_version, manifest refusals, URL resolution, the updater's
// decision table and the instruction hand-over through Settings.

#include <string>

#include "doctest.h"
#include "hc/update_check.h"
#include "test_support.h"

using hc::AppImageState;
using hc::InstructionStatus;
using hc::UpdateManifest;
using hc::UpdaterAction;
using hc::UpdateVerdict;
using hc::Version;
using hc_test::FakeSettings;
using hc_test::ScopedCapture;

namespace {

int cmp(const char* a, const char* b) {
	Version va, vb;
	REQUIRE(hc::parse_version(a, va));
	REQUIRE(hc::parse_version(b, vb));
	return hc::compare_versions(va, vb);
}

UpdateManifest manifest(const char* version, const char* min_version = "") {
	UpdateManifest m;
	std::snprintf(m.version, sizeof(m.version), "%s", version);
	std::snprintf(m.url, sizeof(m.url), "%s", "homecontrol.bin");
	std::snprintf(m.sha256, sizeof(m.sha256), "%s", std::string(64, 'a').c_str());
	m.size = 1000000;
	std::snprintf(m.min_version, sizeof(m.min_version), "%s", min_version);
	return m;
}

} // namespace

TEST_CASE("UpdateCheckProbe.SemanticVersionPrecedenceFollowsTheSpec") {
	CHECK(cmp("1.0.0-alpha", "1.0.0-alpha.1") < 0);
	CHECK(cmp("1.0.0-alpha.1", "1.0.0-alpha.beta") < 0);
	CHECK(cmp("1.0.0-alpha.beta", "1.0.0-beta") < 0);
	CHECK(cmp("1.0.0-beta.2", "1.0.0-beta.11") < 0); // numeric identifiers compare numerically
	CHECK(cmp("1.0.0-rc.1", "1.0.0") < 0);           // a pre-release precedes the release
	CHECK(cmp("1.0.0", "1.0.0+build.7") == 0);        // build metadata is ignored
	CHECK(cmp("1.0.0+a", "1.0.0+b") == 0);
	CHECK(cmp("0.9.9", "0.10.0") < 0);
	CHECK(cmp("2.0.0", "10.0.0") < 0);
	CHECK(cmp("0.1.0-dev", "0.1.0") < 0);
	Version v;
	const char* error = nullptr;
	CHECK_FALSE(hc::parse_version("1.0", v, &error));
	CHECK(error != nullptr);
	CHECK_FALSE(hc::parse_version("01.0.0", v, &error)); // leading zero
	CHECK_FALSE(hc::parse_version("1.0.0-", v, &error));
	CHECK_FALSE(hc::parse_version("1.0.0-rc..1", v, &error));
	CHECK_FALSE(hc::parse_version("v1.0.0", v, &error));
	CHECK(hc::parse_version("1.2.3-rc.1+esp32c6.40mhz", v));
	CHECK(std::string(v.prerelease) == "rc.1");
	CHECK(std::string(v.build) == "esp32c6.40mhz");
}

TEST_CASE("UpdateCheckProbe.VerdictsForDevBuildsReleasesMinVersionAndForce") {
	const char* base = "http://update.example/homecontrol";
	// A dev build takes the release of its own version, and any newer release.
	CHECK(hc::check_update("0.1.0-dev", manifest("0.1.0"), base).verdict == UpdateVerdict::apply);
	CHECK(hc::check_update("0.1.0-dev", manifest("0.2.0"), base).verdict == UpdateVerdict::apply);
	// A dev build ignores min_version (it would otherwise be stuck forever).
	CHECK(hc::check_update("0.1.0-dev", manifest("0.3.0", "0.2.0"), base).verdict == UpdateVerdict::apply);
	// A release never downgrades, never reinstalls, and respects min_version.
	CHECK(hc::check_update("0.2.0", manifest("0.2.0"), base).verdict == UpdateVerdict::up_to_date);
	CHECK(hc::check_update("0.2.0", manifest("0.1.9"), base).verdict == UpdateVerdict::up_to_date);
	hc::UpdateCheckResult blocked = hc::check_update("0.1.0", manifest("0.3.0", "0.2.0"), base);
	CHECK(blocked.verdict == UpdateVerdict::refused);
	CHECK(std::string(blocked.reason).size() > 0);
	CHECK(hc::check_update("0.2.0", manifest("0.3.0", "0.2.0"), base).verdict == UpdateVerdict::apply);
	// Force installs even an older image (development), but never a malformed one.
	CHECK(hc::check_update("0.2.0", manifest("0.1.9"), base, true).verdict == UpdateVerdict::apply);
	UpdateManifest bad = manifest("0.3.0");
	bad.sha256[0] = '\0';
	CHECK(hc::check_update("0.2.0", bad, base, true).verdict == UpdateVerdict::refused);
	// The image URL is resolved against the base.
	hc::UpdateCheckResult ok = hc::check_update("0.1.0", manifest("0.2.0"), base);
	REQUIRE(ok.verdict == UpdateVerdict::apply);
	CHECK(std::string(ok.image_url) == "http://update.example/homecontrol/homecontrol.bin");
	// A garbage running version is a refusal, not a crash.
	CHECK(hc::check_update("garbage", manifest("0.2.0"), base).verdict == UpdateVerdict::refused);
}

TEST_CASE("UpdateCheckProbe.ManifestParsingRefusesWhatItCannotTrust") {
	UpdateManifest m;
	const char* error = nullptr;
	std::string good = std::string("{\"version\":\"0.2.0\",\"url\":\"homecontrol.bin\",\"sha256\":\"") + std::string(64, 'f')
		+ "\",\"size\":123456}";
	CHECK(hc::parse_update_manifest(good.c_str(), good.size(), m, &error));
	CHECK(std::string(m.version) == "0.2.0");
	CHECK(m.size == 123456);
	CHECK(m.min_version[0] == '\0');
	std::string upper = good;
	upper.replace(upper.find("ffff"), 4, "FFFF");
	CHECK(hc::parse_update_manifest(upper.c_str(), upper.size(), m, &error)); // upper-case hex is normalized
	CHECK(std::string(m.sha256) == std::string(64, 'f'));
	std::string short_hash = std::string("{\"version\":\"0.2.0\",\"url\":\"x.bin\",\"sha256\":\"") + std::string(63, 'f')
		+ "\",\"size\":1}";
	CHECK_FALSE(hc::parse_update_manifest(short_hash.c_str(), short_hash.size(), m, &error));
	std::string no_size = std::string("{\"version\":\"0.2.0\",\"url\":\"x.bin\",\"sha256\":\"") + std::string(64, 'f') + "\"}";
	CHECK_FALSE(hc::parse_update_manifest(no_size.c_str(), no_size.size(), m, &error));
	std::string truncated = good.substr(0, good.size() - 5);
	CHECK_FALSE(hc::parse_update_manifest(truncated.c_str(), truncated.size(), m, &error));
	std::string huge(UpdateManifest::kMaxLength + 1, ' ');
	CHECK_FALSE(hc::parse_update_manifest(huge.c_str(), huge.size(), m, &error));
	CHECK(error != nullptr);
	// Unknown fields are tolerated (forward compatibility).
	std::string extra = good;
	extra.insert(1, "\"notes\":\"anything\",");
	CHECK(hc::parse_update_manifest(extra.c_str(), extra.size(), m, &error));
}

TEST_CASE("UpdateCheckProbe.UrlHandling") {
	char out[160];
	CHECK(hc::update_manifest_url("http://h:8070", out, sizeof(out)));
	CHECK(std::string(out) == "http://h:8070/manifest.json");
	CHECK(hc::update_manifest_url("http://h:8070/dir/", out, sizeof(out)));
	CHECK(std::string(out) == "http://h:8070/dir/manifest.json");
	CHECK(hc::resolve_update_url("http://h/dir", "https://cdn.example/fw.bin", out, sizeof(out)));
	CHECK(std::string(out) == "https://cdn.example/fw.bin"); // absolute stays
	CHECK(hc::resolve_update_url("http://h/dir", "/root.bin", out, sizeof(out)));
	CHECK(std::string(out) == "http://h/root.bin");
	const char* error = nullptr;
	CHECK_FALSE(hc::update_url_valid("ftp://h/x", &error));
	CHECK_FALSE(hc::update_url_valid("http://", &error));
	CHECK(hc::update_url_valid("https://update.example/homecontrol", &error));
	char tiny[8];
	CHECK_FALSE(hc::update_manifest_url("http://h:8070", tiny, sizeof(tiny))); // does not overflow
}

TEST_CASE("UpdateCheckProbe.UpdaterDecisionTable") {
	using hc::decide_updater_step;
	CHECK(decide_updater_step(InstructionStatus::none, 0, AppImageState::valid).action == UpdaterAction::boot_app);
	CHECK(decide_updater_step(InstructionStatus::none, 0, AppImageState::aborted).action == UpdaterAction::boot_app);
	hc::UpdaterStep idle = decide_updater_step(InstructionStatus::none, 0, AppImageState::missing);
	CHECK(idle.action == UpdaterAction::wait);
	CHECK(idle.delay_ms == hc::kUpdateIdleRetryMs);
	hc::UpdaterStep first = decide_updater_step(InstructionStatus::pending, 0, AppImageState::valid);
	CHECK(first.action == UpdaterAction::download);
	CHECK(first.delay_ms == 0);
	hc::UpdaterStep retry = decide_updater_step(InstructionStatus::pending, 1, AppImageState::missing);
	CHECK(retry.action == UpdaterAction::download);
	CHECK(retry.delay_ms == hc::kUpdateRetryDelayMs);
	CHECK(decide_updater_step(InstructionStatus::pending, 2, AppImageState::missing).action == UpdaterAction::download);
	CHECK(decide_updater_step(InstructionStatus::pending, hc::kUpdateMaxAttempts, AppImageState::valid).action
		== UpdaterAction::boot_app);
	// Attempts used up and nothing bootable: keep trying, slowly; an aborted image is not bootable.
	hc::UpdaterStep stuck = decide_updater_step(InstructionStatus::pending, hc::kUpdateMaxAttempts, AppImageState::missing);
	CHECK(stuck.action == UpdaterAction::download);
	CHECK(stuck.delay_ms == hc::kUpdateIdleRetryMs);
	CHECK(decide_updater_step(InstructionStatus::pending, 99, AppImageState::aborted).action == UpdaterAction::download);
	CHECK(decide_updater_step(InstructionStatus::corrupt, 0, AppImageState::valid).action == UpdaterAction::boot_app);
	CHECK(decide_updater_step(InstructionStatus::corrupt, 0, AppImageState::missing).action == UpdaterAction::wait);
	for (int a = 0; a < 5; ++a) {
		CHECK(std::string(decide_updater_step(InstructionStatus::pending, a, AppImageState::missing).reason).size() > 0);
	}
}

TEST_CASE("UpdateCheckProbe.InstructionHandOverThroughSettings") {
	ScopedCapture capture;
	FakeSettings settings;
	hc::UpdateInstruction in;
	const char* error = nullptr;
	CHECK(hc::load_update_instruction(settings, in, &error) == InstructionStatus::none);
	UpdateManifest m = manifest("0.2.0");
	REQUIRE(hc::store_update_instruction(settings, m, "http://h/homecontrol.bin", &error));
	REQUIRE(hc::load_update_instruction(settings, in, &error) == InstructionStatus::pending);
	CHECK(std::string(in.version) == "0.2.0");
	CHECK(std::string(in.image_url) == "http://h/homecontrol.bin");
	CHECK(in.size == 1000000);
	CHECK(in.attempts == 0);
	CHECK_FALSE(hc::update_instruction_fulfilled("0.1.0", in));
	CHECK(hc::update_instruction_fulfilled("0.2.0", in));
	// A half-written instruction (image present, version gone) is corrupt, not pending.
	settings.erase(hc::setting::kUpdateVersion);
	CHECK(hc::load_update_instruction(settings, in, &error) == InstructionStatus::corrupt);
	CHECK(error != nullptr);
	// Clearing removes every key; the user's update source stays.
	settings.set_string(hc::setting::kUpdateUrl, "http://h");
	REQUIRE(hc::clear_update_instruction(settings));
	CHECK(hc::load_update_instruction(settings, in, &error) == InstructionStatus::none);
	CHECK(settings.entries.count(hc::setting::kUpdateUrl) == 1);
	CHECK(settings.entries.count(hc::setting::kUpdateSha256) == 0);
	// A failing store reports.
	settings.fail_writes = true;
	CHECK_FALSE(hc::store_update_instruction(settings, m, "http://h/homecontrol.bin", &error));
	CHECK(error != nullptr);
}
