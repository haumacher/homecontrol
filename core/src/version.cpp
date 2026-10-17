/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/version.h"

#ifndef HC_BUILD_ID
#define HC_BUILD_ID "unknown"
#endif

namespace hc {

const char* build_id() {
	return HC_BUILD_ID;
}

} // namespace hc
