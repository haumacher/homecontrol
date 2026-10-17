/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_SHA_H
#define HC_SHA_H

#include <cstddef>
#include <cstdint>

namespace hc {

/// SHA-256 (FIPS 180-4) in software. Incremental: update() any number of times, then finish().
/// The POSIX build hashes passwords with it; the ESP build uses mbedTLS (hardware SHA) through the
/// hc::Crypto interface, and the shared test vectors keep both equal.
class Sha256 {
public:
	static constexpr size_t kDigestSize = 32;
	static constexpr size_t kBlockSize = 64;

	Sha256();
	void update(const void* data, size_t length);
	void finish(uint8_t digest[kDigestSize]);

	static void hash(const void* data, size_t length, uint8_t digest[kDigestSize]);

private:
	void block(const uint8_t* data);

	uint32_t state_[8];
	uint64_t total_ = 0;
	uint8_t buffer_[kBlockSize];
	size_t used_ = 0;
};

/// HMAC-SHA256 (RFC 2104).
void hmac_sha256(const uint8_t* key, size_t key_length, const uint8_t* data, size_t length,
	uint8_t mac[Sha256::kDigestSize]);

/// PBKDF2-HMAC-SHA256 (RFC 8018) producing `out_length` bytes.
void pbkdf2_sha256(const uint8_t* password, size_t password_length, const uint8_t* salt, size_t salt_length,
	uint32_t iterations, uint8_t* out, size_t out_length);

/// SHA-1 (FIPS 180-4), only for the WebSocket handshake (RFC 6455 fixes it there; it is not used
/// for anything that needs collision resistance).
void sha1(const void* data, size_t length, uint8_t digest[20]);

/// Lower-case hex of `length` bytes into `out` (2 * length + 1 bytes with the NUL).
void to_hex(const uint8_t* bytes, size_t length, char* out);

/// Parses exactly 2 * length hex digits; false on any other character or length.
bool from_hex(const char* text, uint8_t* bytes, size_t length);

/// Compares two byte strings in time independent of their content.
bool constant_time_equal(const uint8_t* a, const uint8_t* b, size_t length);

} // namespace hc

#endif // HC_SHA_H
