/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_SIGNATURE_H
#define HC_SIGNATURE_H

#include <cstddef>
#include <cstdint>

#include "hc/platform/settings.h"

namespace hc {

/// The setting of the installation key (issue #35, spec/api.md "E-Home connection", doc/ehome.md).
/// Persisted name: never rename.
namespace setting {

/// The installation's Ed25519 public key (64 hex digits), delivered by the E-Home enrollment or
/// accepted by the owner after a rotation. While it is stored, every runtime image needs its
/// signature. Not listed by the API, masked on the console. Only the factory reset erases it and
/// only a new enrollment's answer replaces it; "Disconnect" (DELETE /api/ehome) keeps it in force.
constexpr const char* kEhomeKey = "ehome.key";

} // namespace setting

/// Ed25519 (RFC 8032, "pure" Ed25519 with SHA-512), on the vendored Monocypher
/// (third_party/monocypher, crypto_ed25519_*). Plain functions over fixed-size buffers: no
/// allocation, nothing kept.
constexpr size_t kEd25519KeySize = 32;
constexpr size_t kEd25519SignatureSize = 64;

/// Whether `signature` is a valid Ed25519 signature of `message` under `public_key`.
bool ed25519_verify(const uint8_t signature[kEd25519SignatureSize], const uint8_t* message, size_t length,
	const uint8_t public_key[kEd25519KeySize]);

/// The public key of the private key `seed` (the 32-byte RFC 8032 secret). For the tests and tools;
/// the device never holds a private key.
void ed25519_public_key(const uint8_t seed[kEd25519KeySize], uint8_t public_key[kEd25519KeySize]);

/// Signs `message` with the private key `seed`. For the tests and tools.
void ed25519_sign(const uint8_t seed[kEd25519KeySize], const uint8_t* message, size_t length,
	uint8_t signature[kEd25519SignatureSize]);

/// A key's fingerprint as the UI and the log show it: the first 8 bytes of SHA-256 over the 32 key
/// bytes, lower-case hex in four groups of four digits ("1a2b 3c4d 5e6f 7a8b"); 20 bytes with the NUL.
constexpr size_t kFingerprintSize = 20;
void key_fingerprint(const uint8_t public_key[kEd25519KeySize], char out[kFingerprintSize]);

/// Parses a public key: exactly 64 hex digits (either case).
bool parse_public_key(const char* text, uint8_t public_key[kEd25519KeySize]);

/// Parses a detached signature: 128 hex digits (either case), surrounded by nothing but white space
/// (a `.sig` file with a final newline), or exactly 64 raw bytes (a binary `.sig` file). The header
/// X-Image-Signature and the socket's `signature` member are text, so only the hex form occurs there.
bool parse_signature(const char* data, size_t length, uint8_t signature[kEd25519SignatureSize]);

/// The signature construction of a runtime image (spec/api.md "Signed runtime images"): plain
/// Ed25519 over the 52-byte message
///
///     "homecontrol-image-v1" (20 ASCII bytes) || SHA-256(image bytes) (32 bytes)
///
/// The image is hashed as it arrives, so the signed message is short and the memory flat whatever the
/// image's size; the prefix keeps a signature of an image from being anything else the installation
/// key may sign one day.
constexpr char kImageSignatureContext[] = "homecontrol-image-v1";
constexpr size_t kImageSignatureContextLength = sizeof(kImageSignatureContext) - 1;
constexpr size_t kImageMessageSize = kImageSignatureContextLength + 32;

/// The signed message of the image whose SHA-256 is `digest`.
void image_signature_message(const uint8_t digest[32], uint8_t message[kImageMessageSize]);

/// Whether `signature` signs the image `text` (its exact bytes) under `public_key`.
bool image_signature_valid(const char* text, size_t length, const uint8_t signature[kEd25519SignatureSize],
	const uint8_t public_key[kEd25519KeySize]);

/// Signs the image `text` with the private key `seed` (tests and tools; E-Home signs in Java).
void sign_image(const uint8_t seed[kEd25519KeySize], const char* text, size_t length,
	uint8_t signature[kEd25519SignatureSize]);

/// What the device holds as the installation key.
enum class KeyState : uint8_t {
	none,    ///< No key: unsigned images are accepted as before (with the API's authentication).
	ok,      ///< A key: every image needs its signature.
	damaged, ///< ehome.key is stored but unreadable or not 64 hex digits: every image is refused.
};

/// Reads ehome.key.
KeyState load_installation_key(const Settings& settings, uint8_t public_key[kEd25519KeySize]);

} // namespace hc

#endif // HC_SIGNATURE_H
