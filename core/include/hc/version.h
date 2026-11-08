/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_VERSION_H
#define HC_VERSION_H

namespace hc {

/// The firmware version, the same on every platform.
constexpr const char* kVersion = "1.1.0";

/// The build id the build system injects (`git describe --always --dirty` at configure time),
/// or "unknown" if git was not available.
const char* build_id();

} // namespace hc

#endif // HC_VERSION_H
