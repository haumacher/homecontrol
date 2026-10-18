/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/update_check.h"

#include "hc/sha.h"
#include "hc_release_key.h"

namespace hc {

namespace {

struct ReleaseKey {
	uint8_t bytes[32] = {};
	bool ok = false;
	ReleaseKey() { ok = from_hex(HC_RELEASE_KEY_HEX, bytes, sizeof(bytes)); }
};

} // namespace

const uint8_t* release_public_key() {
	static const ReleaseKey key; // the build checked the 64 hex digits
	return key.ok ? key.bytes : nullptr;
}

} // namespace hc
