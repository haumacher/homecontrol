/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/version.h"

// Generated at build time from `git describe --always --dirty` (core/cmake/build_id.cmake, #36).
#include "hc_build_id.h"

namespace hc {

const char* build_id() {
	return HC_BUILD_ID;
}

} // namespace hc
