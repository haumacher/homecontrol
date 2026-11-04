/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Ed25519 signatures (issue #35): the vendored Monocypher against RFC 8032, the fingerprint, the
// signature formats, the image construction against the fixtures E-Home tests with
// (spec/fixtures/signatures/vectors.json), the signed manifest, and the signature gate of
// PUT /api/image composed with the loader's refusals.

#include <string>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/image/image_service.h"
#include "hc/knx/adapter.h"
#include "hc/knx/tunnel.h"
#include "hc/network_settings.h"
#include "hc/sha.h"
#include "hc/signature.h"
#include "hc/update_check.h"
#include "knx_test_support.h"
#include "rules_test_support.h"
#include "web_test_support.h"

namespace {

// The test keys of spec/fixtures/signatures/vectors.json: TEST KEYS ONLY (seed = SHA-256 of a
// public label), never trusted anywhere real.
constexpr const char* kInstallationSeed = "48613bb792f76c2201cb3521eaab920dad55d68982490274331eff6dc73a2fb3";
constexpr const char* kInstallationKey = "13833fbfa81080cdb4c1210131a5bb3e98f17675236e66124aed84cc490bccb1";
constexpr const char* kInstallationFingerprint = "ce7d 4a41 a476 9ee3";
constexpr const char* kRotatedSeed = "2bb9dbea0c0c160975dc5e23749d1891e42869b0b87f30c173d3155af920d817";
constexpr const char* kReleaseSeed = "2af0349383f7407cb9ede16d2ae3fd2bba3a4046ea25035ba66830c8273c8079";
constexpr const char* kReleaseKey = "0e12ea41a92beace805bd36530b61d68c82cfe5079c949e28203064f78679432";
constexpr const char* kReleaseFingerprint = "dfba 89a7 15ad 2217";

struct Bytes32 {
	uint8_t b[32];
};

Bytes32 hex32(const char* text) {
	Bytes32 out{};
	REQUIRE(hc::from_hex(text, out.b, sizeof(out.b)));
	return out;
}

std::string spec_file(const std::string& name) {
	std::string text;
	REQUIRE_MESSAGE(hc_test::read_text(HC_SPEC_DIR "/fixtures/" + name, text), "missing fixture " << name);
	return text;
}

std::string hex_signature(const uint8_t sig[64]) {
	char hex[129];
	hc::to_hex(sig, 64, hex);
	return hex;
}

std::string sign_image_hex(const char* seed, const std::string& image) {
	Bytes32 s = hex32(seed);
	uint8_t sig[64];
	hc::sign_image(s.b, image.data(), image.size(), sig);
	return hex_signature(sig);
}

} // namespace

TEST_CASE("Ed25519: RFC 8032 section 7.1 test 1 verifies, a flipped bit fails, signing reproduces it") {
	Bytes32 seed = hex32("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
	Bytes32 key = hex32("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
	uint8_t sig[64];
	REQUIRE(hc::parse_signature("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e3970"
		"1cf9b46bd25bf5f0595bbe24655141438e7a100b", 128, sig));
	CHECK(hc::ed25519_verify(sig, nullptr, 0, key.b));

	uint8_t derived[32];
	hc::ed25519_public_key(seed.b, derived);
	CHECK(std::memcmp(derived, key.b, 32) == 0);
	uint8_t again[64];
	hc::ed25519_sign(seed.b, nullptr, 0, again);
	CHECK(std::memcmp(again, sig, 64) == 0);

	uint8_t flipped[64];
	std::memcpy(flipped, sig, 64);
	flipped[17] ^= 0x04;
	CHECK_FALSE(hc::ed25519_verify(flipped, nullptr, 0, key.b));
	Bytes32 other = key;
	other.b[0] ^= 0x01;
	CHECK_FALSE(hc::ed25519_verify(sig, nullptr, 0, other.b));
	const uint8_t one = 0;
	CHECK_FALSE(hc::ed25519_verify(sig, &one, 1, key.b)); // another message
}

TEST_CASE("Signature: the fingerprint, the key and signature formats") {
	Bytes32 key = hex32(kInstallationKey);
	char fingerprint[hc::kFingerprintSize];
	hc::key_fingerprint(key.b, fingerprint);
	CHECK(std::string(fingerprint) == kInstallationFingerprint);
	// The first 8 bytes of SHA-256 over the 32 key bytes.
	uint8_t digest[32];
	hc::Sha256::hash(key.b, 32, digest);
	char hex[17];
	hc::to_hex(digest, 8, hex);
	std::string grouped = std::string(hex, 4) + " " + std::string(hex + 4, 4) + " " + std::string(hex + 8, 4) + " " +
		std::string(hex + 12, 4);
	CHECK(grouped == fingerprint);

	uint8_t parsed[32];
	CHECK(hc::parse_public_key(kInstallationKey, parsed));
	CHECK(hc::parse_public_key("13833FBFA81080CDB4C1210131A5BB3E98F17675236E66124AED84CC490BCCB1", parsed));
	CHECK_FALSE(hc::parse_public_key("13833fbf", parsed));
	CHECK_FALSE(hc::parse_public_key((std::string(kInstallationKey) + "00").c_str(), parsed));
	CHECK_FALSE(hc::parse_public_key("x3833fbfa81080cdb4c1210131a5bb3e98f17675236e66124aed84cc490bccb1", parsed));

	std::string sig_text = spec_file("images/example.json.sig");
	uint8_t sig[64];
	CHECK(hc::parse_signature(sig_text.data(), sig_text.size(), sig)); // with its final newline
	std::string padded = "  " + sig_text.substr(0, 128) + "\r\n";
	uint8_t same[64];
	CHECK(hc::parse_signature(padded.data(), padded.size(), same));
	CHECK(std::memcmp(sig, same, 64) == 0);
	std::string raw(reinterpret_cast<const char*>(sig), 64); // a binary .sig file
	CHECK(hc::parse_signature(raw.data(), raw.size(), same));
	CHECK(std::memcmp(sig, same, 64) == 0);
	CHECK_FALSE(hc::parse_signature(sig_text.data(), 127, same));
	std::string inner = sig_text.substr(0, 64) + " " + sig_text.substr(64, 64);
	CHECK_FALSE(hc::parse_signature(inner.data(), inner.size(), same));
	CHECK_FALSE(hc::parse_signature(nullptr, 0, same));
}

TEST_CASE("Signature: the fixture image's signature (made by OpenSSL) verifies under the construction") {
	std::string image = spec_file("images/example.json");
	std::string sig_text = spec_file("images/example.json.sig");
	Bytes32 key = hex32(kInstallationKey);
	uint8_t sig[64];
	REQUIRE(hc::parse_signature(sig_text.data(), sig_text.size(), sig));
	CHECK(hc::image_signature_valid(image.data(), image.size(), sig, key.b));
	// The construction spelled out: "homecontrol-image-v1" || SHA-256(image).
	uint8_t digest[32];
	hc::Sha256::hash(image.data(), image.size(), digest);
	uint8_t message[hc::kImageMessageSize];
	hc::image_signature_message(digest, message);
	CHECK(hc::kImageMessageSize == 52);
	CHECK(std::memcmp(message, "homecontrol-image-v1", 20) == 0);
	CHECK(hc::ed25519_verify(sig, message, sizeof(message), key.b));
	// Not a signature over the image bytes themselves, nor over the bare digest.
	CHECK_FALSE(hc::ed25519_verify(sig, reinterpret_cast<const uint8_t*>(image.data()), image.size(), key.b));
	CHECK_FALSE(hc::ed25519_verify(sig, digest, sizeof(digest), key.b));
	// Monocypher signs the same bytes the same way (Ed25519 is deterministic).
	CHECK(sign_image_hex(kInstallationSeed, image) == sig_text.substr(0, 128));
	// One byte more, and it fails.
	std::string changed = image + " ";
	CHECK_FALSE(hc::image_signature_valid(changed.data(), changed.size(), sig, key.b));
}

TEST_CASE("Manifest: signed by the release key it verifies; unsigned, malformed, mis-signed are refused") {
	std::string manifest = spec_file("signatures/manifest.json");
	std::string sig = spec_file("signatures/manifest.sig");
	Bytes32 key = hex32(kReleaseKey);
	char reason[200];
	CHECK(hc::verify_manifest_signature(manifest.data(), manifest.size(), sig.data(), sig.size(), key.b, reason,
		sizeof(reason)) == hc::ManifestSignature::valid);
	CHECK(std::string(reason).empty());
	// The verified bytes parse as the manifest the update check decides on.
	hc::UpdateManifest parsed;
	REQUIRE(hc::parse_update_manifest(manifest.data(), manifest.size(), parsed));
	CHECK(std::string(parsed.version) == "0.2.0");

	CHECK(hc::verify_manifest_signature(manifest.data(), manifest.size(), nullptr, 0, key.b, reason, sizeof(reason)) ==
		hc::ManifestSignature::missing);
	CHECK(std::string(reason) == std::string("the manifest is not signed (no manifest.sig next to it; the release key is ") +
		kReleaseFingerprint + ")");
	CHECK(hc::verify_manifest_signature(manifest.data(), manifest.size(), "not a signature", 15, key.b, reason,
		sizeof(reason)) == hc::ManifestSignature::malformed);
	CHECK(std::string(reason) == "manifest.sig is not a signature (128 hex digits or 64 bytes)");
	// The same manifest with one byte changed (a newer version slipped in).
	std::string forged = manifest;
	forged.replace(forged.find("0.2.0"), 5, "0.3.0");
	CHECK(hc::verify_manifest_signature(forged.data(), forged.size(), sig.data(), sig.size(), key.b, reason,
		sizeof(reason)) == hc::ManifestSignature::mismatch);
	CHECK(std::string(reason).find(kReleaseFingerprint) != std::string::npos);
	// Signed by another key (the installation key is not the release key).
	Bytes32 other = hex32(kInstallationSeed);
	uint8_t wrong[64];
	hc::ed25519_sign(other.b, reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size(), wrong);
	std::string wrong_hex = hex_signature(wrong);
	CHECK(hc::verify_manifest_signature(manifest.data(), manifest.size(), wrong_hex.data(), wrong_hex.size(), key.b,
		reason, sizeof(reason)) == hc::ManifestSignature::mismatch);
	CHECK(std::string(reason) == std::string("the manifest's signature does not match the release key ") +
		kReleaseFingerprint + " (another key, or the manifest changed)");
	// A firmware without a release key refuses everything.
	CHECK(hc::verify_manifest_signature(manifest.data(), manifest.size(), sig.data(), sig.size(), nullptr, reason,
		sizeof(reason)) == hc::ManifestSignature::no_key);
	// Signing with the test release seed reproduces the fixture.
	Bytes32 seed = hex32(kReleaseSeed);
	uint8_t again[64];
	hc::ed25519_sign(seed.b, reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size(), again);
	CHECK(hex_signature(again) == sig.substr(0, 128));

	char url[128];
	REQUIRE(hc::update_signature_url("http://192.0.2.5:8070/", url, sizeof(url)));
	CHECK(std::string(url) == "http://192.0.2.5:8070/manifest.sig");
	CHECK(std::string(hc::manifest_signature_name(hc::ManifestSignature::mismatch)) == "mismatch");
	// The build's release key is a usable key (platform/esp/release-key.pub or HC_RELEASE_KEY).
	CHECK(hc::release_public_key() != nullptr);
}

namespace {

struct ImageFixture : hc_test::WebFixture {
	hc_test::ScopedCapture capture{hc::LogLevel::debug};
	hc_test::FakeUdpSocket socket{fake.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), fake.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{controller.loop(), fake.clock, model, tunnel};
	hc_test::FakeBlobStore blobs;
	hc::image::ImageService service{controller, model, blobs, adapter, &tunnel};
	std::string cookie;

	ImageFixture() {
		controller.start();
		service.start();
		api.set_image(&service);
		cookie = onboard();
	}

	void store_key(const char* key) {
		REQUIRE(fake.settings.set_string("ehome.key", key) == hc::SettingsStatus::ok);
	}

	Result put_image(const std::string& image, const std::string& signature = "") {
		image_signature = signature;
		Result r = request(hc::HttpMethod::put, "/api/image", image, cookie);
		image_signature.clear();
		return r;
	}
};

} // namespace

TEST_CASE("Signed images: a device with the key accepts the signed image and refuses unsigned and mis-signed ones") {
	ImageFixture f;
	std::string minimal = spec_file("images/minimal.json");
	std::string example = spec_file("images/example.json");
	// Without a key: the unsigned image goes in with the session, as before.
	auto r = f.put_image(minimal);
	REQUIRE(r.status == 200);
	CHECK(f.service.info().revision == 0);
	uint32_t token = f.service.store_token();

	f.store_key(kInstallationKey);
	// Unsigned: refused, nothing written, the previous image keeps running.
	r = f.put_image(example);
	CHECK(r.status == 403);
	CHECK(r.json["code"].text == "image.unsigned");
	REQUIRE(r.json["refusals"].items.size() == 1);
	CHECK(r.json["refusals"].items[0]["reason"].text.find(kInstallationFingerprint) != std::string::npos);
	CHECK(r.json["loaded"].boolean);
	CHECK(f.service.info().revision == 0);
	CHECK(std::string(f.service.info().project_id) == "minimal");
	CHECK(f.service.store_token() == token);
	CHECK(f.capture.sink.contains("W hc.image: image from api refused (unsigned): the image is not signed; the device "
		"accepts only images signed with the installation key ce7d 4a41 a476 9ee3; the loaded image stays"));

	// Signed with another key: refused as a bad signature, with the expected key's fingerprint.
	r = f.put_image(example, sign_image_hex(kRotatedSeed, example));
	CHECK(r.status == 403);
	CHECK(r.json["code"].text == "image.bad_signature");
	CHECK(r.json["refusals"].items[0]["reason"].text.find(kInstallationFingerprint) != std::string::npos);
	CHECK(f.service.store_token() == token);
	// The right signature over other bytes (the image changed after signing).
	r = f.put_image(example + "\n", spec_file("images/example.json.sig").substr(0, 128));
	CHECK(r.json["code"].text == "image.bad_signature");
	CHECK(f.service.store_token() == token);

	// The fixture's signature: accepted, stored.
	r = f.put_image(example, spec_file("images/example.json.sig").substr(0, 128));
	REQUIRE(r.status == 200);
	CHECK(f.service.info().revision == 7);
	CHECK(f.service.store_token() != token);
	CHECK(f.capture.sink.contains("I hc.image: image from api: signature verified (installation key ce7d 4a41 a476 9ee3, " +
		std::to_string(example.size()) + " bytes in "));
	CHECK(f.capture.sink.contains("PUT /api/image from 192.0.2.7: "));
}

TEST_CASE("Signed images: a malformed header is refused 400 whatever the key; without a key a signature is not checked") {
	ImageFixture f;
	std::string minimal = spec_file("images/minimal.json");
	auto r = f.put_image(minimal, "abcd");
	CHECK(r.status == 400);
	CHECK(r.json["code"].text == "image.bad_signature");
	CHECK(r.json["error"].text == "X-Image-Signature is the image's Ed25519 signature in 128 hex digits");
	CHECK_FALSE(f.service.loaded());
	// No key: a (wrong) signature is not checked, the image goes in with the session.
	r = f.put_image(minimal, sign_image_hex(kRotatedSeed, "something else"));
	CHECK(r.status == 200);
	CHECK(f.capture.sink.contains("its signature is not checked (the device holds no installation key)"));
}

TEST_CASE("Signed images probe: the loader's refusal of a signed image wins; an oversized one is too large first") {
	ImageFixture f;
	f.store_key(kInstallationKey);
	// Signed correctly but invalid content: the loader's reason, not the signature.
	std::string invalid = "{\"image\": {\"schemaVersion\": \"1.0\", \"projectId\": \"x\", \"revision\": -1}}";
	auto r = f.put_image(invalid, sign_image_hex(kInstallationSeed, invalid));
	CHECK(r.status == 422);
	CHECK(r.json["code"].text == "image.invalid");
	CHECK(f.capture.sink.contains("signature verified"));
	// Not JSON at all, signed: the syntax refusal.
	std::string garbage = "not json";
	r = f.put_image(garbage, sign_image_hex(kInstallationSeed, garbage));
	CHECK(r.json["code"].text == "image.bad_json");
	// Larger than the device takes: too large, before the signature (none was given).
	std::string big(hc::image::kMaxImageBytes + 1, ' ');
	CHECK_FALSE(f.service.load(big.data(), big.size(), hc::image::Source::api, true, nullptr));
	CHECK(f.service.report().kind == hc::image::Report::Kind::too_large);
	// The transport's limit refuses it before WebApi ever sees it (413, both servers and the socket).
	CHECK(f.api.body_limit(hc::HttpMethod::put, "/api/image") == hc::image::kMaxImageBytes);
	// The other sources pass the same gate: a file, the console.
	std::string minimal = spec_file("images/minimal.json");
	CHECK_FALSE(f.service.load(minimal.data(), minimal.size(), hc::image::Source::file, true, nullptr));
	CHECK(f.service.report().kind == hc::image::Report::Kind::unsigned_image);
	CHECK_FALSE(f.service.load(minimal.data(), minimal.size(), hc::image::Source::console, true, nullptr));
	CHECK(f.service.report().kind == hc::image::Report::Kind::unsigned_image);
	uint8_t sig[64];
	Bytes32 seed = hex32(kInstallationSeed);
	hc::sign_image(seed.b, minimal.data(), minimal.size(), sig);
	CHECK(f.service.load(minimal.data(), minimal.size(), hc::image::Source::file, true, sig));
}

TEST_CASE("Signed images: the stored image loads at boot without a signature; a damaged key refuses every image") {
	ImageFixture f;
	f.store_key(kInstallationKey);
	std::string example = spec_file("images/example.json");
	REQUIRE(f.put_image(example, spec_file("images/example.json.sig").substr(0, 128)).status == 200);
	// The boot load of the stored copy needs no signature (it passed the gate when it was stored).
	CHECK(f.service.load_stored(hc::image::Source::boot));
	CHECK(f.service.info().revision == 7);

	REQUIRE(f.fake.settings.set_string("ehome.key", "not a key") == hc::SettingsStatus::ok);
	auto r = f.put_image(example, spec_file("images/example.json.sig").substr(0, 128));
	CHECK(r.status == 403);
	CHECK(r.json["code"].text == "image.bad_signature");
	CHECK(r.json["refusals"].items[0]["reason"].text ==
		"the stored installation key (setting ehome.key) is damaged: disconnect from E-Home and enroll again");
	// The key is masked on the console and never listed by the API.
	CHECK(hc::setting_is_secret("ehome.key"));
	CHECK(f.get("/api/settings", f.cookie).body.find("ehome.key") == std::string::npos);
}
