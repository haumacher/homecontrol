/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_CRYPTO_H
#define HC_PLATFORM_CRYPTO_H

#include <cstddef>
#include <cstdint>

namespace hc {

/// The cryptographic primitives the web authentication needs (doc/web.md). The ESP platform uses
/// the hardware random number generator and mbedTLS (hardware SHA); POSIX uses getrandom() and the
/// core's software hc::pbkdf2_sha256(). Both must compute standard PBKDF2-HMAC-SHA256 (RFC 8018):
/// the stored password hash is a persisted format.
class Crypto {
public:
	/// Fills `out` with cryptographically secure random bytes. Returns false (logged) if the
	/// platform has no secure source; the caller then refuses what needed them.
	virtual bool random(uint8_t* out, size_t length) = 0;

	/// PBKDF2-HMAC-SHA256. May take a second: never call it on the event loop's thread.
	virtual void pbkdf2_sha256(const uint8_t* password, size_t password_length, const uint8_t* salt,
		size_t salt_length, uint32_t iterations, uint8_t* out, size_t out_length) = 0;

protected:
	~Crypto() = default;
};

} // namespace hc

#endif // HC_PLATFORM_CRYPTO_H
