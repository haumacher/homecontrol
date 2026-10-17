/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/posix/platform.h"

#include <cstdio>
#include <ctime>

namespace hc {
namespace posix {

uint64_t PosixClock::monotonic_ms() const {
	timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return static_cast<uint64_t>(now.tv_sec) * 1000u + static_cast<uint64_t>(now.tv_nsec) / 1000000u;
}

WallTime PosixClock::wall_time() const {
	timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	return WallTime{static_cast<int64_t>(now.tv_sec), true};
}

void StderrLogSink::write(LogLevel level, const char* tag, const char* message) {
	timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	tm local;
	localtime_r(&now.tv_sec, &local);
	char stamp[32];
	strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
	std::fprintf(stderr, "%s.%03ld %s %s: %s\n", stamp, now.tv_nsec / 1000000L, log_level_letter(level), tag,
		message);
}

MemoryInfo PosixSystemInfo::memory() const {
	return MemoryInfo{0, 0};
}

} // namespace posix
} // namespace hc
