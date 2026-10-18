/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/signature.h"

#include <cstring>

#include "hc/sha.h"
#include "monocypher-ed25519.h"
#include "monocypher.h"

namespace hc {

namespace {

bool is_space(char c) {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

} // namespace

bool ed25519_verify(const uint8_t signature[kEd25519SignatureSize], const uint8_t* message, size_t length,
	const uint8_t public_key[kEd25519KeySize]) {
	return crypto_ed25519_check(signature, public_key, message, length) == 0;
}

void ed25519_public_key(const uint8_t seed[kEd25519KeySize], uint8_t public_key[kEd25519KeySize]) {
	uint8_t copy[32];
	uint8_t secret[64];
	std::memcpy(copy, seed, sizeof(copy)); // crypto_ed25519_key_pair wipes the seed it is given
	crypto_ed25519_key_pair(secret, public_key, copy);
	crypto_wipe(secret, sizeof(secret));
}

void ed25519_sign(const uint8_t seed[kEd25519KeySize], const uint8_t* message, size_t length,
	uint8_t signature[kEd25519SignatureSize]) {
	uint8_t copy[32];
	uint8_t secret[64];
	uint8_t public_key[32];
	std::memcpy(copy, seed, sizeof(copy));
	crypto_ed25519_key_pair(secret, public_key, copy);
	crypto_ed25519_sign(signature, secret, message, length);
	crypto_wipe(secret, sizeof(secret));
}

void key_fingerprint(const uint8_t public_key[kEd25519KeySize], char out[kFingerprintSize]) {
	uint8_t digest[Sha256::kDigestSize];
	Sha256::hash(public_key, kEd25519KeySize, digest);
	char hex[17];
	to_hex(digest, 8, hex);
	size_t n = 0;
	for (size_t i = 0; i < 16; ++i) {
		if (i > 0 && i % 4 == 0) {
			out[n++] = ' ';
		}
		out[n++] = hex[i];
	}
	out[n] = '\0';
}

bool parse_public_key(const char* text, uint8_t public_key[kEd25519KeySize]) {
	return text != nullptr && std::strlen(text) == 2 * kEd25519KeySize && from_hex(text, public_key, kEd25519KeySize);
}

bool parse_signature(const char* data, size_t length, uint8_t signature[kEd25519SignatureSize]) {
	if (data == nullptr) {
		return false;
	}
	if (length == kEd25519SignatureSize) {
		std::memcpy(signature, data, kEd25519SignatureSize);
		return true;
	}
	size_t begin = 0;
	while (begin < length && is_space(data[begin])) {
		++begin;
	}
	size_t end = length;
	while (end > begin && is_space(data[end - 1])) {
		--end;
	}
	if (end - begin != 2 * kEd25519SignatureSize) {
		return false;
	}
	char hex[2 * kEd25519SignatureSize + 1];
	std::memcpy(hex, data + begin, 2 * kEd25519SignatureSize);
	hex[2 * kEd25519SignatureSize] = '\0';
	return from_hex(hex, signature, kEd25519SignatureSize);
}

void image_signature_message(const uint8_t digest[32], uint8_t message[kImageMessageSize]) {
	std::memcpy(message, kImageSignatureContext, kImageSignatureContextLength);
	std::memcpy(message + kImageSignatureContextLength, digest, 32);
}

bool image_signature_valid(const char* text, size_t length, const uint8_t signature[kEd25519SignatureSize],
	const uint8_t public_key[kEd25519KeySize]) {
	uint8_t digest[Sha256::kDigestSize];
	Sha256::hash(text, length, digest);
	uint8_t message[kImageMessageSize];
	image_signature_message(digest, message);
	return ed25519_verify(signature, message, sizeof(message), public_key);
}

void sign_image(const uint8_t seed[kEd25519KeySize], const char* text, size_t length,
	uint8_t signature[kEd25519SignatureSize]) {
	uint8_t digest[Sha256::kDigestSize];
	Sha256::hash(text, length, digest);
	uint8_t message[kImageMessageSize];
	image_signature_message(digest, message);
	ed25519_sign(seed, message, sizeof(message), signature);
}

KeyState load_installation_key(const Settings& settings, uint8_t public_key[kEd25519KeySize]) {
	char text[Settings::kMaxStringLength + 1];
	SettingsStatus status = settings.get_string(setting::kEhomeKey, text, sizeof(text));
	if (status == SettingsStatus::not_found) {
		return KeyState::none;
	}
	if (status != SettingsStatus::ok || !parse_public_key(text, public_key)) {
		return KeyState::damaged;
	}
	return KeyState::ok;
}

} // namespace hc
