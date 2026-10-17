/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_POSIX_PLATFORM_H
#define HC_POSIX_PLATFORM_H

#include "hc/log.h"
#include "hc/platform/clock.h"
#include "hc/platform/system_info.h"

namespace hc {
namespace posix {

/// Monotonic time from CLOCK_MONOTONIC, wall time from CLOCK_REALTIME. The host OS keeps the wall
/// clock synchronized, so it is reported as synchronized.
class PosixClock : public Clock {
public:
	uint64_t monotonic_ms() const override;
	WallTime wall_time() const override;
};

/// Writes "<local time with ms> <level letter> <tag>: <message>" lines to stderr.
class StderrLogSink : public LogSink {
public:
	void write(LogLevel level, const char* tag, const char* message) override;
};

/// Free-memory figures on the host. A heap figure of a Linux process is not meaningful for the
/// device budget (the C6 has a fixed heap, the host grows on demand), so this reports 0 for both.
class PosixSystemInfo : public SystemInfo {
public:
	MemoryInfo memory() const override;
};

} // namespace posix
} // namespace hc

#endif // HC_POSIX_PLATFORM_H
