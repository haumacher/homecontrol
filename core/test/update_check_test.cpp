/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <cstring>
#include <string>

#include "doctest.h"
#include "hc/update_check.h"
#include "hc/version.h"
#include "test_support.h"

using hc::AppImageState;
using hc::InstructionStatus;
using hc::UpdateCheckResult;
using hc::UpdateManifest;
using hc::UpdaterAction;
using hc::UpdateVerdict;
using hc::Version;

namespace {

const char* const kSha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

Version v(const char* text) {
	Version result;
	REQUIRE(hc::parse_version(text, result));
	return result;
}

int cmp(const char* a, const char* b) {
	return hc::compare_versions(v(a), v(b));
}

std::string parse_error(const std::string& json) {
	UpdateManifest manifest;
	const char* error = nullptr;
	bool ok = hc::parse_update_manifest(json.c_str(), json.size(), manifest, &error);
	if (ok) {
		return "ok";
	}
	REQUIRE(error != nullptr);
	return error;
}

UpdateManifest manifest(const char* version, const char* url = "homecontrol.bin", const char* min_version = "") {
	std::string json = std::string("{\"version\":\"") + version + "\",\"url\":\"" + url + "\",\"sha256\":\"" + kSha +
		"\",\"size\":1034576";
	if (min_version[0] != '\0') {
		json += std::string(",\"min_version\":\"") + min_version + "\"";
	}
	json += "}";
	UpdateManifest result;
	const char* error = "";
	INFO(json);
	bool ok = hc::parse_update_manifest(json.c_str(), json.size(), result, &error);
	INFO(error);
	REQUIRE(ok);
	return result;
}

const char* const kBase = "http://192.0.2.10:8070/fw";

} // namespace

TEST_CASE("UpdateCheck.ParsesVersions") {
	Version version = v("0.1.0-dev");
	CHECK(version.major == 0);
	CHECK(version.minor == 1);
	CHECK(version.patch == 0);
	CHECK(std::string(version.prerelease) == "dev");
	CHECK(std::string(version.build).empty());
	CHECK(version.is_prerelease());

	version = v("1.22.333-rc.1+g1a2b3c4-dirty");
	CHECK(version.major == 1);
	CHECK(version.minor == 22);
	CHECK(version.patch == 333);
	CHECK(std::string(version.prerelease) == "rc.1");
	CHECK(std::string(version.build) == "g1a2b3c4-dirty");

	version = v("2.0.0+build.7");
	CHECK_FALSE(version.is_prerelease());
	CHECK(std::string(version.build) == "build.7");

	// The firmware's own version parses.
	CHECK(hc::parse_version(hc::kVersion, version));
}

TEST_CASE("UpdateCheck.RefusesMalformedVersionsWithAReason") {
	Version version;
	const char* error = nullptr;
	CHECK_FALSE(hc::parse_version("", version, &error));
	CHECK(std::string(error) == "version is empty");
	CHECK_FALSE(hc::parse_version(nullptr, version, &error));
	const char* const bad_core[] = {"1", "1.2", "1.2.", "v1.2.3", "01.2.3", "1.02.3", "1.2.03", "1.2.3.4", "a.b.c",
		"1..3", "-1.2.3", "4294967296.0.0"};
	for (const char* text : bad_core) {
		INFO(text);
		error = nullptr;
		CHECK_FALSE(hc::parse_version(text, version, &error));
		REQUIRE(error != nullptr);
	}
	CHECK_FALSE(hc::parse_version("1.02.3", version, &error));
	CHECK(std::string(error) == "version is not MAJOR.MINOR.PATCH (numbers without leading zeros)");
	CHECK_FALSE(hc::parse_version("1.2.3.4", version, &error));
	CHECK(std::string(error) == "unexpected characters after the version");
	CHECK_FALSE(hc::parse_version("1.2.3-", version, &error));
	CHECK(std::string(error) == "invalid pre-release in version (dot-separated [0-9A-Za-z-] identifiers)");
	CHECK_FALSE(hc::parse_version("1.2.3-a..b", version, &error));
	CHECK_FALSE(hc::parse_version("1.2.3-01", version, &error)); // numeric identifier with a leading zero
	CHECK_FALSE(hc::parse_version("1.2.3-dev_1", version, &error));
	CHECK_FALSE(hc::parse_version("1.2.3+", version, &error));
	CHECK(std::string(error) == "invalid build metadata in version (dot-separated [0-9A-Za-z-] identifiers)");
	CHECK_FALSE(hc::parse_version("1.2.3+a+b", version, &error));
	CHECK_FALSE(hc::parse_version("1.2.3 ", version, &error));
	CHECK(std::string(error) == "unexpected characters after the version");
	CHECK(hc::parse_version("1.2.3+001", version)); // build metadata may have leading zeros
	std::string too_long = "1.2.3-" + std::string(60, 'a');
	CHECK_FALSE(hc::parse_version(too_long.c_str(), version, &error));
	CHECK(std::string(error) == "version longer than 63 characters");
	// A refused parse leaves nothing half-filled.
	CHECK(version.major == 0);
	CHECK(std::string(version.prerelease).empty());
}

TEST_CASE("UpdateCheck.ComparesBySemverPrecedence") {
	CHECK(cmp("0.1.0", "0.1.0") == 0);
	CHECK(cmp("0.1.0", "0.1.1") < 0);
	CHECK(cmp("0.2.0", "0.1.9") > 0);
	CHECK(cmp("1.0.0", "0.99.99") > 0);
	CHECK(cmp("0.10.0", "0.9.0") > 0); // numeric, not textual
	// A pre-release precedes its release.
	CHECK(cmp("0.1.0-dev", "0.1.0") < 0);
	CHECK(cmp("0.1.0", "0.1.0-dev") > 0);
	CHECK(cmp("0.1.0-dev", "0.1.1-dev") < 0);
	CHECK(cmp("0.1.0-dev", "0.0.9") > 0);
	// semver.org's example chain.
	const char* const chain[] = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2",
		"1.0.0-beta.11", "1.0.0-rc.1", "1.0.0"};
	for (size_t i = 0; i + 1 < sizeof(chain) / sizeof(chain[0]); ++i) {
		INFO(chain[i]);
		CHECK(cmp(chain[i], chain[i + 1]) < 0);
		CHECK(cmp(chain[i + 1], chain[i]) > 0);
	}
	// Build metadata does not count.
	CHECK(cmp("0.1.0-dev+g1a2b3c4", "0.1.0-dev+g5d6e7f8-dirty") == 0);
	CHECK(cmp("0.1.0+a", "0.1.0") == 0);
}

TEST_CASE("UpdateCheck.ParsesAManifest") {
	const std::string json = std::string("{\n  \"version\": \"0.2.0+g1a2b3c4\",\n  \"url\": \"homecontrol.bin\",\n") +
		"  \"sha256\": \"0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef\",\n  \"size\": 1034576,\n" +
		"  \"min_version\": \"0.1.0\",\n  \"notes\": {\"text\": [\"a\", 1, true, null, -1.5e3]}, \"esc\": \"\\\"\\u0041\"\n}\n";
	UpdateManifest m;
	const char* error = nullptr;
	REQUIRE(hc::parse_update_manifest(json.c_str(), json.size(), m, &error));
	CHECK(std::string(m.version) == "0.2.0+g1a2b3c4");
	CHECK(std::string(m.url) == "homecontrol.bin");
	CHECK(std::string(m.sha256) == kSha); // normalized to lower case
	CHECK(m.size == 1034576);
	CHECK(std::string(m.min_version) == "0.1.0");

	// min_version is optional; escapes are decoded.
	const std::string minimal = std::string("{\"size\":1,\"sha256\":\"") + kSha +
		"\",\"url\":\"http:\\/\\/h\\/a.bin\",\"version\":\"1.0.0\"}";
	REQUIRE(hc::parse_update_manifest(minimal.c_str(), minimal.size(), m, &error));
	CHECK(std::string(m.url) == "http://h/a.bin");
	CHECK(std::string(m.min_version).empty());
}

TEST_CASE("UpdateCheck.RefusesMalformedManifestsWithAReason") {
	const std::string sha = std::string("\"sha256\":\"") + kSha + "\"";
	const std::string good_rest = ",\"url\":\"a.bin\"," + sha + ",\"size\":5}";
	CHECK(parse_error("{\"version\":\"1.0.0\"" + good_rest) == "ok");

	CHECK(parse_error("") == "manifest is empty");
	CHECK(parse_error("[1]") == "manifest is not a JSON object");
	CHECK(parse_error("{}") == "manifest has no version");
	CHECK(parse_error("{\"version\":\"1.0.0\"}") == "manifest has no url");
	CHECK(parse_error("{\"version\":\"1.0.0\",\"url\":\"a\"}") == "manifest has no sha256");
	CHECK(parse_error("{\"version\":\"1.0.0\",\"url\":\"a\"," + sha + "}") == "manifest has no size");
	CHECK(parse_error("{\"version\":1" + good_rest) == "version is not a string");
	CHECK(parse_error("{\"version\":\"1.0\"" + good_rest) ==
		"version is not MAJOR.MINOR.PATCH (numbers without leading zeros)");
	CHECK(parse_error("{\"version\":\"1.0.0\",\"version\":\"1.0.0\"" + good_rest) == "duplicate member in manifest");
	CHECK(parse_error("{\"version\":\"1.0.0\",\"url\":\"\"," + sha + ",\"size\":5}") == "url is empty");
	CHECK(parse_error("{\"version\":\"1.0.0\",\"url\":\"a b\"," + sha + ",\"size\":5}") ==
		"url contains a space, control or non-ASCII character");
	CHECK(parse_error("{\"version\":\"1.0.0\",\"url\":\"" + std::string(128, 'a') + "\"," + sha + ",\"size\":5}") ==
		"url longer than 127 characters");
	CHECK(parse_error("{\"version\":\"1.0.0\",\"url\":\"a\",\"sha256\":\"abc\",\"size\":5}") ==
		"sha256 is not 64 hex digits");
	CHECK(parse_error("{\"version\":\"1.0.0\",\"url\":\"a\",\"sha256\":\"" + std::string(64, 'g') + "\",\"size\":5}") ==
		"sha256 is not 64 hex digits");
	CHECK(parse_error("{\"version\":\"1.0.0\",\"url\":\"a\",\"sha256\":\"" + std::string(65, 'a') + "\",\"size\":5}") ==
		"sha256 is not 64 hex digits");
	CHECK(parse_error("{\"version\":\"1.0.0\",\"url\":\"a\"," + sha + ",\"size\":0}") == "size is 0");
	CHECK(parse_error("{\"version\":\"1.0.0\",\"url\":\"a\"," + sha + ",\"size\":-5}") ==
		"size is not a non-negative integer");
	CHECK(parse_error("{\"version\":\"1.0.0\",\"url\":\"a\"," + sha + ",\"size\":1.5}") ==
		"size is not a non-negative integer");
	CHECK(parse_error("{\"version\":\"1.0.0\",\"url\":\"a\"," + sha + ",\"size\":\"5\"}") ==
		"size is not a non-negative integer");
	CHECK(parse_error("{\"version\":\"1.0.0\",\"url\":\"a\"," + sha + ",\"size\":16777217}") == "size larger than 16 MB");
	CHECK(parse_error("{\"version\":\"1.0.0\",\"url\":\"a\"," + sha + ",\"size\":99999999999}") ==
		"size is not a non-negative integer");
	CHECK(parse_error("{\"version\":\"1.0.0\",\"min_version\":\"x\"" + good_rest) ==
		"version is not MAJOR.MINOR.PATCH (numbers without leading zeros)");
	CHECK(parse_error("{\"version\":\"1.0.0\"" + good_rest + " x") == "unexpected text after the manifest object");
	CHECK(parse_error("{\"version\":\"1.0.0\"" + good_rest.substr(0, good_rest.size() - 1)) ==
		"expected ',' or '}' in manifest");
	CHECK(parse_error("{\"version\" \"1.0.0\"" + good_rest) == "expected ':' after a member name");
	CHECK(parse_error("{\"version\":\"1.0.0") == "unterminated string");
	CHECK(parse_error("{\"version\":\"1.0.0\\q\"" + good_rest) == "invalid escape in a string");
	CHECK(parse_error("{\"version\":\"1.0.0\\u00e4\"" + good_rest) == "\\u escape outside ASCII");
	CHECK(parse_error("{\"version\":\"1.0.0\",\"x\":\"a\nb\"" + good_rest) == "control character in a string");
	CHECK(parse_error("{\"version\":\"1.0.0\",\"x\":nope" + good_rest) == "invalid value");
	CHECK(parse_error("{\"x\":[[[[[[[[[[1]]]]]]]]]," "\"version\":\"1.0.0\"" + good_rest) == "nested too deeply");
	std::string big = "{\"version\":\"1.0.0\",\"pad\":\"" + std::string(1100, 'x') + "\"" + good_rest;
	CHECK(parse_error(big) == "manifest larger than 1024 bytes");
	// Every prefix of a valid manifest is refused, never accepted or crashed on.
	const std::string valid = "{\"version\":\"1.0.0\"" + good_rest;
	for (size_t n = 1; n < valid.size(); ++n) {
		INFO(valid.substr(0, n));
		CHECK(parse_error(valid.substr(0, n)) != "ok");
	}
}

TEST_CASE("UpdateCheck.ValidatesUpdateUrls") {
	const char* error = nullptr;
	CHECK(hc::update_url_valid("http://192.0.2.10:8070/fw"));
	CHECK(hc::update_url_valid("https://example.org/homecontrol/"));
	CHECK_FALSE(hc::update_url_valid("", &error));
	CHECK(std::string(error) == "URL is empty");
	CHECK_FALSE(hc::update_url_valid("ftp://example.org/", &error));
	CHECK(std::string(error) == "URL must start with http:// or https://");
	CHECK_FALSE(hc::update_url_valid("http://", &error));
	CHECK(std::string(error) == "URL has no host");
	CHECK_FALSE(hc::update_url_valid("http:///a", &error));
	CHECK_FALSE(hc::update_url_valid("http://a b/", &error));
	CHECK(std::string(error) == "URL contains a space, control or non-ASCII character");
	std::string long_url = "http://h/" + std::string(119, 'a');
	CHECK(long_url.size() == 128);
	CHECK_FALSE(hc::update_url_valid(long_url.c_str(), &error));
	CHECK(std::string(error) == "URL longer than 127 characters");
}

TEST_CASE("UpdateCheck.BuildsAndResolvesUrls") {
	char out[160];
	REQUIRE(hc::update_manifest_url("http://h:8070/fw", out, sizeof(out)));
	CHECK(std::string(out) == "http://h:8070/fw/manifest.json");
	REQUIRE(hc::update_manifest_url("http://h:8070/fw//", out, sizeof(out)));
	CHECK(std::string(out) == "http://h:8070/fw/manifest.json");
	CHECK_FALSE(hc::update_manifest_url("http://h:8070/fw", out, 20));

	const char* error = nullptr;
	REQUIRE(hc::resolve_update_url("http://h:8070/fw", "homecontrol.bin", out, sizeof(out), &error));
	CHECK(std::string(out) == "http://h:8070/fw/homecontrol.bin");
	REQUIRE(hc::resolve_update_url("http://h:8070/fw/", "esp32c6/homecontrol.bin", out, sizeof(out)));
	CHECK(std::string(out) == "http://h:8070/fw/esp32c6/homecontrol.bin");
	REQUIRE(hc::resolve_update_url("http://h:8070/fw", "/other/a.bin", out, sizeof(out)));
	CHECK(std::string(out) == "http://h:8070/other/a.bin");
	REQUIRE(hc::resolve_update_url("http://h:8070", "/a.bin", out, sizeof(out)));
	CHECK(std::string(out) == "http://h:8070/a.bin");
	REQUIRE(hc::resolve_update_url("http://h:8070/fw", "https://cdn.example.org/a.bin", out, sizeof(out)));
	CHECK(std::string(out) == "https://cdn.example.org/a.bin");

	CHECK_FALSE(hc::resolve_update_url("http://h/fw", "ftp://h/a.bin", out, sizeof(out), &error));
	CHECK(std::string(error) == "image URL must use http:// or https://");
	CHECK(std::string(out).empty());
	CHECK_FALSE(hc::resolve_update_url("nonsense", "a.bin", out, sizeof(out), &error));
	CHECK(std::string(error) == "URL must start with http:// or https://");
	CHECK_FALSE(hc::resolve_update_url("http://h/fw", "", out, sizeof(out), &error));
	CHECK(std::string(error) == "image URL is empty");
	std::string base = "http://h/" + std::string(100, 'b');
	CHECK_FALSE(hc::resolve_update_url(base.c_str(), std::string(30, 'a').c_str(), out, sizeof(out), &error));
	CHECK(std::string(error) == "image URL longer than 127 characters");
	CHECK_FALSE(hc::resolve_update_url("http://h/fw", "a.bin", out, 10, &error));
	CHECK(std::string(error) == "image URL longer than 127 characters");
}

TEST_CASE("UpdateCheck.AppliesOnlyNewerVersions") {
	UpdateCheckResult r = hc::check_update("0.1.0", manifest("0.2.0"), kBase);
	CHECK(r.verdict == UpdateVerdict::apply);
	CHECK(std::string(r.reason) == "newer version");
	CHECK(std::string(r.image_url) == "http://192.0.2.10:8070/fw/homecontrol.bin");

	r = hc::check_update("0.2.0", manifest("0.2.0+g1a2b3c4"), kBase);
	CHECK(r.verdict == UpdateVerdict::up_to_date);
	CHECK(std::string(r.reason) == "the manifest offers the running version");
	CHECK(std::string(r.image_url).empty());

	r = hc::check_update("0.3.0", manifest("0.2.0"), kBase);
	CHECK(r.verdict == UpdateVerdict::up_to_date);
	CHECK(std::string(r.reason) == "the manifest offers an older version");

	// A -dev build updates to its release and to any newer one, not to an older one.
	CHECK(hc::check_update("0.1.0-dev", manifest("0.1.0"), kBase).verdict == UpdateVerdict::apply);
	CHECK(hc::check_update("0.1.0-dev", manifest("0.1.1-dev"), kBase).verdict == UpdateVerdict::apply);
	CHECK(hc::check_update("0.1.0-dev", manifest("0.1.0-dev+other"), kBase).verdict == UpdateVerdict::up_to_date);
	CHECK(hc::check_update("0.1.0-dev", manifest("0.0.9"), kBase).verdict == UpdateVerdict::up_to_date);
	CHECK(hc::check_update("0.1.0", manifest("0.1.0-dev"), kBase).verdict == UpdateVerdict::up_to_date);
}

TEST_CASE("UpdateCheck.MinVersionGatesReleasesButNotDevBuilds") {
	UpdateCheckResult r = hc::check_update("0.1.0", manifest("0.3.0", "a.bin", "0.2.0"), kBase);
	CHECK(r.verdict == UpdateVerdict::refused);
	CHECK(std::string(r.reason) ==
		"the running version is below the manifest's min_version: install an intermediate release first");
	CHECK(hc::check_update("0.2.0", manifest("0.3.0", "a.bin", "0.2.0"), kBase).verdict == UpdateVerdict::apply);
	CHECK(hc::check_update("0.2.1", manifest("0.3.0", "a.bin", "0.2.0"), kBase).verdict == UpdateVerdict::apply);
	// -dev builds update to any newer release, min_version or not.
	CHECK(hc::check_update("0.1.0-dev", manifest("0.3.0", "a.bin", "0.2.0"), kBase).verdict == UpdateVerdict::apply);
}

TEST_CASE("UpdateCheck.ForceInstallsAnyValidImage") {
	UpdateCheckResult r = hc::check_update("0.2.0", manifest("0.2.0"), kBase, true);
	CHECK(r.verdict == UpdateVerdict::apply);
	CHECK(std::string(r.reason) == "forced");
	CHECK(hc::check_update("0.2.0", manifest("0.1.0", "a.bin", "0.3.0"), kBase, true).verdict == UpdateVerdict::apply);
	// Force does not bypass validation.
	CHECK(hc::check_update("0.2.0", manifest("0.1.0", "ftp://h/a.bin"), kBase, true).verdict == UpdateVerdict::refused);
}

TEST_CASE("UpdateCheck.RefusesWhatItCannotResolve") {
	UpdateCheckResult r = hc::check_update("0.1.0", manifest("0.2.0", "ftp://h/a.bin"), kBase);
	CHECK(r.verdict == UpdateVerdict::refused);
	CHECK(std::string(r.reason) == "image URL must use http:// or https://");
	r = hc::check_update("not a version", manifest("0.2.0"), kBase);
	CHECK(r.verdict == UpdateVerdict::refused);
	CHECK(std::string(r.reason) == "the running firmware's version is malformed");
	r = hc::check_update("0.1.0", manifest("0.2.0"), "");
	CHECK(r.verdict == UpdateVerdict::refused);
	CHECK(std::string(r.reason) == "URL is empty");
	UpdateManifest broken; // not from the parser
	std::strcpy(broken.version, "x");
	r = hc::check_update("0.1.0", broken, kBase);
	CHECK(r.verdict == UpdateVerdict::refused);
	CHECK(std::string(r.reason) == "version is not MAJOR.MINOR.PATCH (numbers without leading zeros)");
}

TEST_CASE("UpdateCheck.InstructionRoundTripsThroughTheSettings") {
	hc_test::FakeSettings settings;
	hc::UpdateInstruction instruction;
	CHECK(hc::load_update_instruction(settings, instruction) == InstructionStatus::none);

	UpdateManifest m = manifest("0.2.0+g1a2b3c4");
	const char* error = nullptr;
	REQUIRE(hc::store_update_instruction(settings, m, "http://192.0.2.10:8070/fw/homecontrol.bin", &error));
	REQUIRE(hc::load_update_instruction(settings, instruction, &error) == InstructionStatus::pending);
	CHECK(std::string(instruction.image_url) == "http://192.0.2.10:8070/fw/homecontrol.bin");
	CHECK(std::string(instruction.version) == "0.2.0+g1a2b3c4");
	CHECK(std::string(instruction.sha256) == kSha);
	CHECK(instruction.size == 1034576);
	CHECK(instruction.attempts == 0);

	// The updater counts attempts in the same settings.
	REQUIRE(settings.set_int(hc::setting::kUpdateAttempts, 2) == hc::SettingsStatus::ok);
	REQUIRE(hc::load_update_instruction(settings, instruction) == InstructionStatus::pending);
	CHECK(instruction.attempts == 2);

	CHECK(hc::update_instruction_fulfilled("0.2.0", instruction));
	CHECK_FALSE(hc::update_instruction_fulfilled("0.1.0", instruction));
	CHECK_FALSE(hc::update_instruction_fulfilled("0.2.0-dev", instruction));

	// A new instruction resets the attempts.
	REQUIRE(hc::store_update_instruction(settings, manifest("0.3.0"), "http://h/b.bin"));
	REQUIRE(hc::load_update_instruction(settings, instruction) == InstructionStatus::pending);
	CHECK(instruction.attempts == 0);
	CHECK(std::string(instruction.version) == "0.3.0");

	CHECK(hc::clear_update_instruction(settings));
	CHECK(hc::load_update_instruction(settings, instruction) == InstructionStatus::none);
	CHECK(settings.entries.empty());
	// Clearing twice is fine.
	CHECK(hc::clear_update_instruction(settings));
}

TEST_CASE("UpdateCheck.IncompleteInstructionsAreCorruptNotPending") {
	hc_test::FakeSettings settings;
	hc::UpdateInstruction instruction;
	const char* error = nullptr;
	REQUIRE(hc::store_update_instruction(settings, manifest("0.2.0"), "http://h/a.bin"));

	settings.entries.erase(hc::setting::kUpdateSha256);
	CHECK(hc::load_update_instruction(settings, instruction, &error) == InstructionStatus::corrupt);
	CHECK(std::string(error) == "update.sha256 missing or invalid");

	REQUIRE(hc::store_update_instruction(settings, manifest("0.2.0"), "http://h/a.bin"));
	REQUIRE(settings.set_string(hc::setting::kUpdateVersion, "garbage") == hc::SettingsStatus::ok);
	CHECK(hc::load_update_instruction(settings, instruction, &error) == InstructionStatus::corrupt);
	CHECK(std::string(error) == "update.version missing or invalid");

	REQUIRE(hc::store_update_instruction(settings, manifest("0.2.0"), "http://h/a.bin"));
	REQUIRE(settings.set_string(hc::setting::kUpdateSize, "5") == hc::SettingsStatus::ok);
	CHECK(hc::load_update_instruction(settings, instruction, &error) == InstructionStatus::corrupt);
	CHECK(std::string(error) == "update.size missing or invalid");

	REQUIRE(hc::store_update_instruction(settings, manifest("0.2.0"), "http://h/a.bin"));
	REQUIRE(settings.set_string(hc::setting::kUpdateImage, "nonsense") == hc::SettingsStatus::ok);
	CHECK(hc::load_update_instruction(settings, instruction, &error) == InstructionStatus::corrupt);
	CHECK(std::string(error) == "update.image is not a valid URL");

	// Without the image key the rest is not an instruction (a store interrupted by a power loss).
	REQUIRE(hc::store_update_instruction(settings, manifest("0.2.0"), "http://h/a.bin"));
	settings.entries.erase(hc::setting::kUpdateImage);
	CHECK(hc::load_update_instruction(settings, instruction) == InstructionStatus::none);
}

TEST_CASE("UpdateCheck.StoreRefusesAndReportsFailures") {
	hc_test::FakeSettings settings;
	const char* error = nullptr;
	CHECK_FALSE(hc::store_update_instruction(settings, manifest("0.2.0"), "ftp://h/a.bin", &error));
	CHECK(std::string(error) == "URL must start with http:// or https://");
	settings.fail_writes = true;
	CHECK_FALSE(hc::store_update_instruction(settings, manifest("0.2.0"), "http://h/a.bin", &error));
	CHECK(std::string(error) == "cannot store update.version");
	hc::UpdateInstruction instruction;
	CHECK(hc::load_update_instruction(settings, instruction) == InstructionStatus::none);
}

TEST_CASE("UpdateCheck.UpdaterDecisionTable") {
	using hc::decide_updater_step;
	// Nothing pending: boot whatever verifies, also a rolled-back image (a reset in its first minute).
	CHECK(decide_updater_step(InstructionStatus::none, 0, AppImageState::valid).action == UpdaterAction::boot_app);
	CHECK(decide_updater_step(InstructionStatus::none, 0, AppImageState::aborted).action == UpdaterAction::boot_app);
	hc::UpdaterStep step = decide_updater_step(InstructionStatus::none, 0, AppImageState::missing);
	CHECK(step.action == UpdaterAction::wait);
	CHECK(step.delay_ms == hc::kUpdateIdleRetryMs);
	CHECK(decide_updater_step(InstructionStatus::corrupt, 0, AppImageState::valid).action == UpdaterAction::boot_app);
	CHECK(decide_updater_step(InstructionStatus::corrupt, 0, AppImageState::missing).action == UpdaterAction::wait);

	// Pending: download, at once the first time, then with the retry delay - over any ota_0 content.
	for (AppImageState app : {AppImageState::missing, AppImageState::valid, AppImageState::aborted}) {
		INFO(hc::app_image_state_name(app));
		step = decide_updater_step(InstructionStatus::pending, 0, app);
		CHECK(step.action == UpdaterAction::download);
		CHECK(step.delay_ms == 0);
		step = decide_updater_step(InstructionStatus::pending, hc::kUpdateMaxAttempts - 1, app);
		CHECK(step.action == UpdaterAction::download);
		CHECK(step.delay_ms == hc::kUpdateRetryDelayMs);
	}

	// Attempts used up: back to an intact old application, else retry slowly forever.
	step = decide_updater_step(InstructionStatus::pending, hc::kUpdateMaxAttempts, AppImageState::valid);
	CHECK(step.action == UpdaterAction::boot_app);
	step = decide_updater_step(InstructionStatus::pending, hc::kUpdateMaxAttempts, AppImageState::missing);
	CHECK(step.action == UpdaterAction::download);
	CHECK(step.delay_ms == hc::kUpdateIdleRetryMs);
	step = decide_updater_step(InstructionStatus::pending, hc::kUpdateMaxAttempts + 7, AppImageState::aborted);
	CHECK(step.action == UpdaterAction::download);
	CHECK(step.delay_ms == hc::kUpdateIdleRetryMs);
	CHECK(std::string(step.reason).find("every 5 minutes") != std::string::npos);
}

TEST_CASE("Update source: the channel of the release site, stable by default, a development server in its place") {
	hc_test::FakeSettings settings;
	char source[hc::Settings::kMaxStringLength + 1];
	// Nothing stored: the stable channel of the Pages site.
	CHECK(std::string(hc::update_channel(settings)) == "stable");
	REQUIRE(hc::update_source(settings, source, sizeof(source)));
	CHECK(std::string(source) == "https://haumacher.github.io/homecontrol/stable");
	CHECK(hc::update_url_valid(source));

	const char* error = nullptr;
	REQUIRE(hc::store_update_channel(settings, "beta", &error));
	REQUIRE(hc::update_source(settings, source, sizeof(source)));
	CHECK(std::string(source) == "https://haumacher.github.io/homecontrol/beta");

	REQUIRE(hc::store_update_channel(settings, "off", &error));
	CHECK_FALSE(hc::update_source(settings, source, sizeof(source)));
	CHECK(std::string(source).empty());

	CHECK_FALSE(hc::store_update_channel(settings, "nightly", &error));
	CHECK(std::string(error) == "the update channel is stable, beta or off");
	CHECK(std::string(hc::update_channel(settings)) == "off");
	// An unreadable value reads as stable.
	settings.set_string(hc::setting::kUpdateChannel, "x");
	CHECK(std::string(hc::update_channel(settings)) == "stable");

	// The development server replaces the channel until it is cleared.
	REQUIRE(hc::store_update_channel(settings, "off", &error));
	REQUIRE(hc::store_update_url(settings, "http://192.0.2.10:8070", &error));
	REQUIRE(hc::update_source(settings, source, sizeof(source)));
	CHECK(std::string(source) == "http://192.0.2.10:8070");
	CHECK_FALSE(hc::store_update_url(settings, "ftp://192.0.2.10", &error));
	REQUIRE(hc::store_update_url(settings, "", &error));
	CHECK(settings.entries.count(hc::setting::kUpdateUrl) == 0);
	CHECK_FALSE(hc::update_source(settings, source, sizeof(source)));

	char small[8];
	REQUIRE(hc::store_update_channel(settings, "stable", &error));
	CHECK_FALSE(hc::update_source(settings, small, sizeof(small)));
}

TEST_CASE("Update source: an update.url of an older firmware becomes a channel") {
	struct Case {
		const char* stored;
		const char* channel; // nullptr: stays as the development server
	};
	const Case cases[] = {
		{"https://haumacher.github.io/homecontrol/stable", "stable"},
		{"https://haumacher.github.io/homecontrol/stable.", "stable"}, // copied with a full stop
		{"https://haumacher.github.io/homecontrol/stable/", "stable"},
		{"https://haumacher.github.io/homecontrol/beta", "beta"},
		{"https://haumacher.github.io/homecontrol", "stable"},
		{"off", "off"},
		{"", "off"},
		{"http://192.0.2.10:8070", nullptr},
		{"https://haumacher.github.io/homecontrolx/stable", nullptr},
	};
	for (const Case& c : cases) {
		hc_test::FakeSettings settings;
		settings.set_string(hc::setting::kUpdateUrl, c.stored);
		const char* migrated = hc::migrate_update_source(settings);
		if (c.channel == nullptr) {
			CHECK_MESSAGE(migrated == nullptr, c.stored);
			CHECK_MESSAGE(settings.entries.count(hc::setting::kUpdateUrl) == 1, c.stored);
		} else {
			REQUIRE_MESSAGE(migrated != nullptr, c.stored);
			CHECK_MESSAGE(std::string(migrated) == c.channel, c.stored);
			CHECK_MESSAGE(std::string(hc::update_channel(settings)) == c.channel, c.stored);
			CHECK_MESSAGE(settings.entries.count(hc::setting::kUpdateUrl) == 0, c.stored);
		}
	}
	hc_test::FakeSettings none;
	CHECK(hc::migrate_update_source(none) == nullptr);
	CHECK(none.entries.empty());
}
