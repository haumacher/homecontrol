/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <cstring>
#include <string>

#include "doctest.h"
#include "hc/sha.h"

namespace {

std::string hex(const uint8_t* bytes, size_t length) {
	char text[200];
	hc::to_hex(bytes, length, text);
	return text;
}

std::string sha256(const std::string& text) {
	uint8_t digest[32];
	hc::Sha256::hash(text.data(), text.size(), digest);
	return hex(digest, sizeof(digest));
}

std::string pbkdf2(const std::string& password, const std::string& salt, uint32_t iterations, size_t length) {
	uint8_t out[64];
	hc::pbkdf2_sha256(reinterpret_cast<const uint8_t*>(password.data()), password.size(),
		reinterpret_cast<const uint8_t*>(salt.data()), salt.size(), iterations, out, length);
	return hex(out, length);
}

} // namespace

TEST_CASE("Sha256 matches the FIPS 180-4 examples") {
	CHECK(sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	CHECK(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	CHECK(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
		"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	// One million 'a', fed in uneven pieces.
	hc::Sha256 sha;
	std::string piece(997, 'a');
	size_t left = 1000000;
	while (left > 0) {
		size_t n = left < piece.size() ? left : piece.size();
		sha.update(piece.data(), n);
		left -= n;
	}
	uint8_t digest[32];
	sha.finish(digest);
	CHECK(hex(digest, 32) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("HmacSha256 matches RFC 4231") {
	uint8_t key[20];
	std::memset(key, 0x0b, sizeof(key));
	uint8_t mac[32];
	hc::hmac_sha256(key, sizeof(key), reinterpret_cast<const uint8_t*>("Hi There"), 8, mac);
	CHECK(hex(mac, 32) == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
	hc::hmac_sha256(reinterpret_cast<const uint8_t*>("Jefe"), 4,
		reinterpret_cast<const uint8_t*>("what do ya want for nothing?"), 28, mac);
	CHECK(hex(mac, 32) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
	// A key longer than the block is hashed first (test case 6).
	uint8_t long_key[131];
	std::memset(long_key, 0xaa, sizeof(long_key));
	const char* text = "Test Using Larger Than Block-Size Key - Hash Key First";
	hc::hmac_sha256(long_key, sizeof(long_key), reinterpret_cast<const uint8_t*>(text), std::strlen(text), mac);
	CHECK(hex(mac, 32) == "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST_CASE("Pbkdf2Sha256 matches the published vectors") {
	// RFC 7914 section 11.
	CHECK(pbkdf2("passwd", "salt", 1, 64) ==
		"55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
		"49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
	// The common PBKDF2-HMAC-SHA256 vectors (as in RFC 6070 for SHA-1).
	CHECK(pbkdf2("password", "salt", 1, 32) == "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
	CHECK(pbkdf2("password", "salt", 2, 32) == "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
	CHECK(pbkdf2("password", "salt", 4096, 32) == "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
}

TEST_CASE("Sha1 matches FIPS 180-4") {
	uint8_t digest[20];
	hc::sha1("abc", 3, digest);
	CHECK(hex(digest, 20) == "a9993e364706816aba3e25717850c26c9cd0d89d");
	hc::sha1("", 0, digest);
	CHECK(hex(digest, 20) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
	const char* two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	hc::sha1(two_blocks, std::strlen(two_blocks), digest);
	CHECK(hex(digest, 20) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

TEST_CASE("Hex conversion is strict") {
	uint8_t bytes[2];
	CHECK(hc::from_hex("0aFf", bytes, 2));
	CHECK(bytes[0] == 0x0a);
	CHECK(bytes[1] == 0xff);
	CHECK_FALSE(hc::from_hex("0aF", bytes, 2));
	CHECK_FALSE(hc::from_hex("0aFf0", bytes, 2));
	CHECK_FALSE(hc::from_hex("0g00", bytes, 2));
	CHECK_FALSE(hc::from_hex(nullptr, bytes, 2));
	uint8_t a[3] = {1, 2, 3};
	uint8_t b[3] = {1, 2, 4};
	CHECK(hc::constant_time_equal(a, a, 3));
	CHECK_FALSE(hc::constant_time_equal(a, b, 3));
}
