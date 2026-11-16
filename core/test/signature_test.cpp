/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Ed25519 signatures (issue #35): the vendored Monocypher against RFC 8032, the fingerprint, the
// signature formats, the image construction (context v2 over the binary image, the signature its
// last 64 bytes; spec/runtime-image.md 9.17, 13, issue #87) against the bytes of minimal.signed.bin
// and the keys of spec/fixtures/signatures/vectors.json, the signed manifest, and the signature gate
// of PUT /api/image composed with the device's other refusals.

#include <string>
#include <vector>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/image/binary.h"
#include "hc/image/image_service.h"
#include "hc/image/memory_image_slot.h"
#include "hc/knx/adapter.h"
#include "hc/knx/tunnel.h"
#include "hc/network_settings.h"
#include "hc/sha.h"
#include "hc/signature.h"
#include "hc/update_check.h"
#include "image_support.h"
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
constexpr const char* kRotatedKey = "ed592207e3c3f843383c53d6d0b402b2b047323ca0eca31673655d6a940d96cd";
constexpr const char* kRotatedFingerprint = "90ae 3702 f3a9 63ab";
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

std::vector<uint8_t> from_hex(const char* text) {
	std::vector<uint8_t> out;
	int high = -1;
	for (const char* p = text; *p != '\0'; ++p) {
		int v = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : -1;
		if (v < 0) {
			continue;
		}
		if (high < 0) {
			high = v;
		} else {
			out.push_back(static_cast<uint8_t>(high << 4 | v));
			high = -1;
		}
	}
	return out;
}

/// spec/runtime-image.md 13: minimal.json compiled (112 bytes), unsigned.
const std::vector<uint8_t>& minimal() {
	static const std::vector<uint8_t> bytes = from_hex(
		"48 43 49 4d 01 00 00 00 70 00 00 00 42 2c 77 59"
		"00 00 00 00 20 00 02 00 00 00 00 00 00 00 00 00"
		"01 00 00 00 40 00 00 00 20 00 00 00 20 00 00 00"
		"02 00 00 00 60 00 00 00 0d 00 00 00 00 00 00 00"
		"05 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00"
		"00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"
		"00 31 2e 30 00 6d 69 6e 69 6d 61 6c 00 00 00 00");
	return bytes;
}

/// spec/runtime-image.md 13: minimal.signed.bin, signed with the test key "installation" (176 bytes).
const std::vector<uint8_t>& minimal_signed() {
	static const std::vector<uint8_t> bytes = from_hex(
		"48 43 49 4d 01 00 00 00 b0 00 00 00 87 ff f6 f6"
		"01 00 00 00 20 00 02 00 00 00 00 00 00 00 00 00"
		"01 00 00 00 40 00 00 00 20 00 00 00 20 00 00 00"
		"02 00 00 00 60 00 00 00 0d 00 00 00 00 00 00 00"
		"05 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00"
		"00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"
		"00 31 2e 30 00 6d 69 6e 69 6d 61 6c 00 00 00 00"
		"d8 39 e1 4f 50 d3 4b d3 97 ef 9d 57 2d d7 af 3e"
		"a8 3f d8 ce 24 a8 4e bb 28 ee ed 99 b4 1e 22 2d"
		"90 35 04 e9 07 2e 01 ac 4f 86 5c 39 3c b3 42 83"
		"fd 39 a2 ef 6e 21 aa 0f d6 7b e4 bf 92 0f 28 06");
	return bytes;
}

/// The signature of minimal.signed.bin (spec/runtime-image.md 13), 128 hex digits.
constexpr const char* kMinimalSignature =
	"d839e14f50d34bd397ef9d572dd7af3ea83fd8ce24a84ebb28eeed99b41e222d"
	"903504e9072e01ac4f865c393cb34283fd39a2ef6e21aa0fd67be4bf920f2806";

std::string as_text(const std::vector<uint8_t>& bytes) {
	return std::string(bytes.begin(), bytes.end());
}

/// The header's CRC written anew over [0, signedEnd) (spec/runtime-image.md 9.2).
void fix_crc(std::vector<uint8_t>& b) {
	const auto& h = *reinterpret_cast<const hc::image::bin::Header*>(b.data());
	size_t end = (h.flags & 1) != 0 ? h.length - 64 : h.length;
	uint32_t crc = hc::image::bin::image_crc(b.data(), end);
	for (int i = 0; i < 4; ++i) {
		b[12 + i] = static_cast<uint8_t>(crc >> (8 * i));
	}
}

/// The signature of a signed image `b` made anew with `seed` (9.17: over [0, signedEnd), appended).
void resign(std::vector<uint8_t>& b, const char* seed) {
	Bytes32 s = hex32(seed);
	size_t end = b.size() - 64;
	hc::sign_image(s.b, reinterpret_cast<const char*>(b.data()), end, b.data() + end);
}

/// The source `json` compiled, signed with `seed` (nullptr: unsigned).
std::vector<uint8_t> compiled_signed(const std::string& json, const char* seed) {
	hc::test::CompileOptions options;
	Bytes32 s{};
	if (seed != nullptr) {
		s = hex32(seed);
		options.sign_seed = s.b;
	}
	return hc_test::compiled(json, options);
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

	std::string sig_text = spec_file("signatures/manifest.sig");
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

TEST_CASE("Signature: minimal.signed.bin's signature verifies under the construction of context v2") {
	const std::vector<uint8_t>& image = minimal_signed();
	REQUIRE(image.size() == 176);
	// The committed corpus file is these bytes (spec/runtime-image.md 13.1).
	CHECK(spec_file("images/minimal.signed.bin") == as_text(image));
	CHECK(spec_file("images/minimal.bin") == as_text(minimal()));
	Bytes32 key = hex32(kInstallationKey);
	const uint8_t* sig = image.data() + 112; // the image's last 64 bytes
	CHECK(hex_signature(sig) == kMinimalSignature);
	const char* text = reinterpret_cast<const char*>(image.data());
	CHECK(hc::image_signature_valid(text, 112, sig, key.b));
	// The construction spelled out: "homecontrol-image-v2" || SHA-256(image[0, signedEnd)).
	uint8_t digest[32];
	hc::Sha256::hash(image.data(), 112, digest);
	char digest_hex[65];
	hc::to_hex(digest, 32, digest_hex);
	CHECK(std::string(digest_hex) == "c5e66fdbb811651835cd1b864e30e99b4b3a6070f51e8949d39c35807d3a79d8");
	uint8_t message[hc::kImageMessageSize];
	hc::image_signature_message(digest, message);
	CHECK(hc::kImageMessageSize == 52);
	CHECK(std::memcmp(message, "homecontrol-image-v2", 20) == 0);
	char message_hex[2 * 52 + 1];
	hc::to_hex(message, sizeof(message), message_hex);
	CHECK(std::string(message_hex) ==
		"686f6d65636f6e74726f6c2d696d6167652d7632c5e66fdbb811651835cd1b864e30e99b4b3a6070f51e8949d39c35807d3a79d8");
	CHECK(hc::ed25519_verify(sig, message, sizeof(message), key.b));
	// Not under the context v1 of firmware up to 1.2, nor over the bytes themselves, nor the bare
	// digest, nor over the whole file with the signature.
	uint8_t v1[52];
	std::memcpy(v1, "homecontrol-image-v1", 20);
	std::memcpy(v1 + 20, digest, 32);
	CHECK_FALSE(hc::ed25519_verify(sig, v1, sizeof(v1), key.b));
	CHECK_FALSE(hc::ed25519_verify(sig, image.data(), 112, key.b));
	CHECK_FALSE(hc::ed25519_verify(sig, digest, sizeof(digest), key.b));
	CHECK_FALSE(hc::image_signature_valid(text, 176, sig, key.b));
	// Monocypher signs the same bytes the same way (Ed25519 is deterministic), and so does the test
	// compiler: minimal.json signed with the installation seed is minimal.signed.bin.
	CHECK(sign_image_hex(kInstallationSeed, std::string(text, 112)) == kMinimalSignature);
	CHECK(compiled_signed(spec_file("images/minimal.json"), kInstallationSeed) == image);
	// Not under another key; one byte changed, and it fails.
	Bytes32 rotated = hex32(kRotatedKey);
	CHECK_FALSE(hc::image_signature_valid(text, 112, sig, rotated.b));
	std::vector<uint8_t> changed = image;
	changed[0x66] ^= 1;
	CHECK_FALSE(hc::image_signature_valid(reinterpret_cast<const char*>(changed.data()), 112, sig, key.b));
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

/// The API with the image service on an image slot in RAM; a restart is a second fixture on the same
/// blob store and slot.
struct ImageFixture : hc_test::WebFixture {
	hc_test::ScopedCapture capture{hc::LogLevel::debug};
	hc_test::FakeUdpSocket socket{fake.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), fake.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{controller.loop(), fake.clock, model, tunnel};
	hc_test::FakeBlobStore own_blobs;
	hc::image::MemoryImageSlot own_slot;
	hc_test::FakeBlobStore& blobs;
	hc::image::MemoryImageSlot& slot;
	hc::image::ImageService service;
	std::string cookie;

	/// `before`: the device restarted with `key` stored (nullptr: none).
	explicit ImageFixture(ImageFixture* before = nullptr, const char* key = nullptr)
		: own_slot(before != nullptr ? 4096 : hc::image::MemoryImageSlot::kDefaultSize),
		  blobs(before != nullptr ? before->blobs : own_blobs), slot(before != nullptr ? before->slot : own_slot),
		  service(controller, model, blobs, slot, adapter, &tunnel) {
		if (key != nullptr) {
			store_key(key);
		}
		controller.start();
		service.start();
		api.set_image(&service);
		cookie = onboard();
	}

	void store_key(const char* key) {
		REQUIRE(fake.settings.set_string("ehome.key", key) == hc::SettingsStatus::ok);
	}

	Result put_image(const std::vector<uint8_t>& image, const std::string& signature = "") {
		image_signature = signature;
		Result r = request(hc::HttpMethod::put, "/api/image", as_text(image), cookie);
		image_signature.clear();
		return r;
	}

	/// The slot holds `image` from its first byte.
	bool slot_holds(const std::vector<uint8_t>& image) {
		return std::memcmp(slot.bytes(), image.data(), image.size()) == 0;
	}
};

} // namespace

TEST_CASE("Signed images: a device with the key accepts the signed image and refuses unsigned and mis-signed ones") {
	ImageFixture f;
	std::string example = spec_file("images/example.json");
	// Without a key: the unsigned image goes in with the session, as before.
	auto r = f.put_image(minimal());
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(f.service.info().revision == 0);
	CHECK_FALSE(f.service.info().is_signed);
	CHECK(r.json["image"]["signed"].kind == hc_test::Json::Kind::boolean);
	CHECK_FALSE(r.json["image"]["signed"].boolean);
	uint32_t token = f.service.token();

	f.store_key(kInstallationKey);
	// Unsigned: refused from its header before the slot is erased; the previous image keeps running.
	r = f.put_image(compiled_signed(example, nullptr));
	CHECK(r.status == 403);
	CHECK(r.json["code"].text == "image.unsigned");
	REQUIRE(r.json["refusals"].items.size() == 1);
	CHECK(r.json["refusals"].items[0]["path"].text == "header.flags");
	CHECK(r.json["refusals"].items[0]["reason"].text ==
		std::string("the image is not signed; the device accepts only images signed with the installation key ") +
			kInstallationFingerprint);
	CHECK(r.json["loaded"].boolean);
	CHECK(r.json["revision"].number == 0);
	CHECK(f.service.loaded());
	CHECK(f.service.info().revision == 0);
	CHECK(std::string(f.service.info().project_id) == "minimal");
	CHECK(f.service.token() == token);
	CHECK(f.slot_holds(minimal()));
	CHECK(f.capture.sink.contains("W hc.image: image from api refused (unsigned): 1 problem; the running image stays"));

	// Signed with another key: found after the slot was written, a bad signature with the expected
	// key's fingerprint, and no image afterwards (spec/runtime-image.md I9).
	r = f.put_image(compiled_signed(example, kRotatedSeed));
	CHECK(r.status == 403);
	CHECK(r.json["code"].text == "image.bad_signature");
	CHECK(r.json["refusals"].items[0]["path"].text == "signature");
	CHECK(r.json["refusals"].items[0]["reason"].text ==
		std::string("the signature does not verify under the installation key ") + kInstallationFingerprint);
	CHECK_FALSE(r.json["loaded"].boolean);
	CHECK_FALSE(f.service.loaded());
	auto status = f.get("/api/status", f.cookie);
	CHECK(status.json["image"]["refused"].text == "image.bad_signature");
	CHECK(status.json["image"]["reason"].text.rfind("signature: the signature does not verify", 0) == 0);

	// The right signature over other bytes (the image changed after signing, its CRC made to fit).
	std::vector<uint8_t> changed = compiled_signed(example, kInstallationSeed);
	changed[changed.size() - 64 - 1] ^= 0x20; // a padding or string byte before the signature
	fix_crc(changed);
	r = f.put_image(changed);
	CHECK(r.json["code"].text == "image.bad_signature");
	CHECK_FALSE(f.service.loaded());

	// Signed with the installation key: accepted, written into the slot as it came.
	std::vector<uint8_t> good = compiled_signed(example, kInstallationSeed);
	r = f.put_image(good);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(f.service.info().revision == 7);
	CHECK(f.service.info().is_signed);
	CHECK(r.json["image"]["signed"].boolean);
	CHECK(f.service.token() != token);
	CHECK(f.slot_holds(good));
	CHECK(f.capture.sink.contains("PUT /api/image from 192.0.2.7: "));
	// GET /api/image returns it with its signature, deployable again as it is.
	bool streamed = false;
	auto back = f.get_streamed("/api/image", f.cookie, &streamed);
	CHECK(back.status == 200);
	CHECK(back.body == as_text(good));
	REQUIRE(f.put_image(std::vector<uint8_t>(back.body.begin(), back.body.end())).status == 200);
}

TEST_CASE("Signed images: the detached signature of firmware up to 1.2 is refused 400; without a key a signature is not checked") {
	ImageFixture f;
	// X-Image-Signature: refused whatever it holds, nothing loaded.
	auto r = f.put_image(minimal(), "abcd");
	CHECK(r.status == 400);
	CHECK(r.json["code"].text == "image.bad_request");
	CHECK(r.json["error"].text == "the signature is part of the binary image");
	CHECK_FALSE(f.service.loaded());
	r = f.put_image(minimal_signed(), kMinimalSignature);
	CHECK(r.status == 400);
	CHECK(r.json["code"].text == "image.bad_request");
	CHECK_FALSE(f.service.loaded());
	// No key: a signature (one of another key) is not checked, the image goes in with the session.
	r = f.put_image(compiled_signed(spec_file("images/minimal.json"), kRotatedSeed));
	CHECK_MESSAGE(r.status == 200, r.body);
	CHECK(f.service.info().is_signed);
	r = f.put_image(minimal_signed());
	CHECK(r.status == 200);
	// With a key the header is refused just the same.
	f.store_key(kInstallationKey);
	r = f.put_image(minimal_signed(), kMinimalSignature);
	CHECK(r.status == 400);
	CHECK(r.json["code"].text == "image.bad_request");
	CHECK(f.service.loaded()); // the running image untouched
}

TEST_CASE("Signed images probe: the order of the checks: too large, the header, the CRC, the signature, the structure") {
	ImageFixture f;
	f.store_key(kInstallationKey);
	// Signed correctly but structurally invalid (spec 13: sections[1].offset 0x61): the structure's
	// refusal, after the signature verified.
	std::vector<uint8_t> invalid = minimal_signed();
	invalid[0x34] = 0x61;
	fix_crc(invalid);
	resign(invalid, kInstallationSeed);
	auto r = f.put_image(invalid);
	CHECK(r.status == 422);
	CHECK(r.json["code"].text == "image.invalid");
	CHECK(r.json["refusals"].items[0]["path"].text == "sections[1].offset");
	CHECK(r.json["refusals"].items[0]["reason"].text.find("malformed: offset 0x61 is not a multiple of 8") == 0);
	// The same, signed with another key: the signature is checked before the structure.
	resign(invalid, kRotatedSeed);
	r = f.put_image(invalid);
	CHECK(r.json["code"].text == "image.bad_signature");
	// Damaged on the way: the CRC is checked before the signature.
	std::vector<uint8_t> damaged = minimal_signed();
	damaged[0x66] ^= 1;
	r = f.put_image(damaged);
	CHECK(r.status == 422);
	CHECK(r.json["code"].text == "image.damaged");
	CHECK(r.json["refusals"].items[0]["path"].text == "header.crc");
	// Not a binary image at all: the header's refusal, before the key's (the running image stays).
	REQUIRE(f.put_image(minimal_signed()).status == 200);
	std::string json = spec_file("images/minimal.json");
	r = f.request(hc::HttpMethod::put, "/api/image", json, f.cookie);
	CHECK(r.status == 400);
	CHECK(r.json["code"].text == "image.format");
	CHECK(r.json["loaded"].boolean);
	// Larger than the slot: too large, before the header's other checks and the key (none signed).
	std::vector<uint8_t> big(hc::image::MemoryImageSlot::kDefaultSize + 8, 0);
	std::memcpy(big.data(), minimal().data(), 32);
	CHECK_FALSE(hc_test::deploy_bytes(f.service, big));
	CHECK(f.service.report().kind == hc::image::Report::Kind::too_large);
	CHECK(f.service.loaded());
	// The transport's limit refuses it before WebApi ever sees it (413, both servers and the socket).
	CHECK(f.api.body_limit(hc::HttpMethod::put, "/api/image") == hc::image::MemoryImageSlot::kDefaultSize);
	char text[128];
	CHECK(std::string(f.api.body_too_long(hc::HttpMethod::put, "/api/image", big.size(), text, sizeof(text))) ==
		"image.too_large");
	// The other sources pass the same gate: a file, the console.
	CHECK_FALSE(hc_test::deploy_bytes(f.service, minimal(), hc::image::Source::file));
	CHECK(f.service.report().kind == hc::image::Report::Kind::unsigned_image);
	CHECK_FALSE(hc_test::deploy_bytes(f.service, minimal(), hc::image::Source::console));
	CHECK(f.service.report().kind == hc::image::Report::Kind::unsigned_image);
	CHECK_FALSE(hc_test::deploy_bytes(f.service, compiled_signed(json, kRotatedSeed), hc::image::Source::file));
	CHECK(f.service.report().kind == hc::image::Report::Kind::bad_signature);
	CHECK(hc_test::deploy_bytes(f.service, minimal_signed(), hc::image::Source::file));
	CHECK(f.service.source() == hc::image::Source::file);
}

TEST_CASE("Signed images: the slot's image loads at boot without its signature checked; a damaged key refuses every image") {
	ImageFixture f(nullptr, kInstallationKey);
	std::vector<uint8_t> example = compiled_signed(spec_file("images/example.json"), kInstallationSeed);
	REQUIRE(f.put_image(example).status == 200);
	// At boot the image's signature is not checked (spec/runtime-image.md I11): a key rotated since
	// keeps it loading.
	{
		ImageFixture g(&f, kRotatedKey);
		CHECK_MESSAGE(g.service.loaded(), hc_test::describe(g.service.report()));
		CHECK(g.service.source() == hc::image::Source::boot);
		CHECK(g.service.info().revision == 7);
		// A deployment is checked against the new key.
		auto r = g.put_image(example);
		CHECK(r.json["code"].text == "image.bad_signature");
		CHECK(r.json["refusals"].items[0]["reason"].text.find(kRotatedFingerprint) != std::string::npos);
	}
	// Nor is an unsigned image of the slot refused at boot because a key is stored now.
	REQUIRE(f.fake.settings.erase("ehome.key") == hc::SettingsStatus::ok);
	REQUIRE(f.put_image(minimal()).status == 200);
	{
		ImageFixture g(&f, kInstallationKey);
		CHECK(g.service.loaded());
		CHECK(std::string(g.service.info().project_id) == "minimal");
	}

	REQUIRE(f.put_image(example).status == 200);
	REQUIRE(f.fake.settings.set_string("ehome.key", "not a key") == hc::SettingsStatus::ok);
	auto r = f.put_image(example);
	CHECK(r.status == 403);
	CHECK(r.json["code"].text == "image.bad_signature");
	CHECK(r.json["refusals"].items[0]["path"].text == "signature");
	CHECK(r.json["refusals"].items[0]["reason"].text == "the installation key stored on the device is damaged");
	// Found before the slot is erased: the running image stays.
	CHECK(r.json["loaded"].boolean);
	CHECK(f.service.loaded());
	CHECK(f.slot_holds(example));
	CHECK(hc_test::deploy_bytes(f.service, minimal()) == false);
	CHECK(f.service.report().kind == hc::image::Report::Kind::bad_signature);
	// The key is masked on the console and never listed by the API.
	CHECK(hc::setting_is_secret("ehome.key"));
	CHECK(f.get("/api/settings", f.cookie).body.find("ehome.key") == std::string::npos);
}
