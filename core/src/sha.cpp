/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/sha.h"

#include <cstring>

namespace hc {

namespace {

constexpr uint32_t kRound[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
inline uint32_t rotl(uint32_t x, unsigned n) { return (x << n) | (x >> (32 - n)); }

inline uint32_t load_be(const uint8_t* p) {
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

inline void store_be(uint8_t* p, uint32_t v) {
	p[0] = uint8_t(v >> 24);
	p[1] = uint8_t(v >> 16);
	p[2] = uint8_t(v >> 8);
	p[3] = uint8_t(v);
}

} // namespace

Sha256::Sha256()
	: state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::block(const uint8_t* data) {
	uint32_t w[64];
	for (int i = 0; i < 16; ++i) {
		w[i] = load_be(data + 4 * i);
	}
	for (int i = 16; i < 64; ++i) {
		uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
	uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
	for (int i = 0; i < 64; ++i) {
		uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t t1 = h + s1 + ch + kRound[i] + w[i];
		uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t t2 = s0 + maj;
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	state_[0] += a;
	state_[1] += b;
	state_[2] += c;
	state_[3] += d;
	state_[4] += e;
	state_[5] += f;
	state_[6] += g;
	state_[7] += h;
}

void Sha256::update(const void* data, size_t length) {
	const uint8_t* p = static_cast<const uint8_t*>(data);
	total_ += length;
	while (length > 0) {
		size_t take = kBlockSize - used_;
		if (take > length) {
			take = length;
		}
		std::memcpy(buffer_ + used_, p, take);
		used_ += take;
		p += take;
		length -= take;
		if (used_ == kBlockSize) {
			block(buffer_);
			used_ = 0;
		}
	}
}

void Sha256::finish(uint8_t digest[kDigestSize]) {
	uint64_t bits = total_ * 8;
	uint8_t pad = 0x80;
	update(&pad, 1);
	uint8_t zero = 0;
	while (used_ != 56) {
		update(&zero, 1);
	}
	uint8_t length[8];
	for (int i = 0; i < 8; ++i) {
		length[i] = uint8_t(bits >> (56 - 8 * i));
	}
	update(length, 8);
	for (int i = 0; i < 8; ++i) {
		store_be(digest + 4 * i, state_[i]);
	}
}

void Sha256::hash(const void* data, size_t length, uint8_t digest[kDigestSize]) {
	Sha256 sha;
	sha.update(data, length);
	sha.finish(digest);
}

namespace {

/// HMAC with the inner and outer pads hashed once (PBKDF2 reuses them for every iteration).
struct HmacKey {
	Sha256 inner;
	Sha256 outer;

	HmacKey(const uint8_t* key, size_t key_length) {
		uint8_t block[Sha256::kBlockSize] = {};
		if (key_length > Sha256::kBlockSize) {
			Sha256::hash(key, key_length, block);
		} else {
			std::memcpy(block, key, key_length);
		}
		uint8_t pad[Sha256::kBlockSize];
		for (size_t i = 0; i < sizeof(pad); ++i) {
			pad[i] = block[i] ^ 0x36;
		}
		inner.update(pad, sizeof(pad));
		for (size_t i = 0; i < sizeof(pad); ++i) {
			pad[i] = block[i] ^ 0x5c;
		}
		outer.update(pad, sizeof(pad));
	}

	void mac(const uint8_t* data, size_t length, const uint8_t* more, size_t more_length,
		uint8_t out[Sha256::kDigestSize]) const {
		Sha256 i = inner;
		i.update(data, length);
		if (more_length > 0) {
			i.update(more, more_length);
		}
		uint8_t inner_digest[Sha256::kDigestSize];
		i.finish(inner_digest);
		Sha256 o = outer;
		o.update(inner_digest, sizeof(inner_digest));
		o.finish(out);
	}
};

} // namespace

void hmac_sha256(const uint8_t* key, size_t key_length, const uint8_t* data, size_t length,
	uint8_t mac[Sha256::kDigestSize]) {
	HmacKey(key, key_length).mac(data, length, nullptr, 0, mac);
}

void pbkdf2_sha256(const uint8_t* password, size_t password_length, const uint8_t* salt, size_t salt_length,
	uint32_t iterations, uint8_t* out, size_t out_length) {
	HmacKey key(password, password_length);
	uint32_t index = 1;
	while (out_length > 0) {
		uint8_t counter[4];
		store_be(counter, index);
		uint8_t u[Sha256::kDigestSize];
		key.mac(salt, salt_length, counter, sizeof(counter), u);
		uint8_t t[Sha256::kDigestSize];
		std::memcpy(t, u, sizeof(t));
		for (uint32_t i = 1; i < iterations; ++i) {
			key.mac(u, sizeof(u), nullptr, 0, u);
			for (size_t k = 0; k < sizeof(t); ++k) {
				t[k] ^= u[k];
			}
		}
		size_t take = out_length < sizeof(t) ? out_length : sizeof(t);
		std::memcpy(out, t, take);
		out += take;
		out_length -= take;
		++index;
	}
}

void sha1(const void* data, size_t length, uint8_t digest[20]) {
	uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
	const uint8_t* p = static_cast<const uint8_t*>(data);
	uint64_t bits = uint64_t(length) * 8;
	size_t total = ((length + 8) / 64 + 1) * 64;
	for (size_t offset = 0; offset < total; offset += 64) {
		uint8_t chunk[64];
		for (size_t i = 0; i < 64; ++i) {
			size_t at = offset + i;
			if (at < length) {
				chunk[i] = p[at];
			} else if (at == length) {
				chunk[i] = 0x80;
			} else if (at >= total - 8) {
				chunk[i] = uint8_t(bits >> (8 * (total - 1 - at)));
			} else {
				chunk[i] = 0;
			}
		}
		uint32_t w[80];
		for (int i = 0; i < 16; ++i) {
			w[i] = load_be(chunk + 4 * i);
		}
		for (int i = 16; i < 80; ++i) {
			w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		}
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
		for (int i = 0; i < 80; ++i) {
			uint32_t f, k;
			if (i < 20) {
				f = (b & c) | (~b & d);
				k = 0x5A827999;
			} else if (i < 40) {
				f = b ^ c ^ d;
				k = 0x6ED9EBA1;
			} else if (i < 60) {
				f = (b & c) | (b & d) | (c & d);
				k = 0x8F1BBCDC;
			} else {
				f = b ^ c ^ d;
				k = 0xCA62C1D6;
			}
			uint32_t t = rotl(a, 5) + f + e + k + w[i];
			e = d;
			d = c;
			c = rotl(b, 30);
			b = a;
			a = t;
		}
		h[0] += a;
		h[1] += b;
		h[2] += c;
		h[3] += d;
		h[4] += e;
	}
	for (int i = 0; i < 5; ++i) {
		store_be(digest + 4 * i, h[i]);
	}
}

void to_hex(const uint8_t* bytes, size_t length, char* out) {
	static const char kDigits[] = "0123456789abcdef";
	for (size_t i = 0; i < length; ++i) {
		out[2 * i] = kDigits[bytes[i] >> 4];
		out[2 * i + 1] = kDigits[bytes[i] & 0x0f];
	}
	out[2 * length] = '\0';
}

namespace {

int hex_digit(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

} // namespace

bool from_hex(const char* text, uint8_t* bytes, size_t length) {
	if (text == nullptr) {
		return false;
	}
	for (size_t i = 0; i < length; ++i) {
		int hi = hex_digit(text[2 * i]);
		if (hi < 0) {
			return false;
		}
		int lo = hex_digit(text[2 * i + 1]);
		if (lo < 0) {
			return false;
		}
		bytes[i] = uint8_t(hi << 4 | lo);
	}
	return text[2 * length] == '\0';
}

bool constant_time_equal(const uint8_t* a, const uint8_t* b, size_t length) {
	uint8_t diff = 0;
	for (size_t i = 0; i < length; ++i) {
		diff |= a[i] ^ b[i];
	}
	return diff == 0;
}

} // namespace hc
