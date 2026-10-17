/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_POSIX_PLATFORM_H
#define HC_POSIX_PLATFORM_H

#include "hc/connectivity.h"
#include "hc/event_loop.h"
#include "hc/log.h"
#include "hc/platform/clock.h"
#include "hc/platform/network_driver.h"
#include "hc/platform/system_info.h"
#include "hc/platform/time_zone.h"

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

/// The C library's local time: apply() sets the environment variable TZ and calls tzset(), which
/// also changes the local time of the log stamps.
class PosixTimeZone : public TimeZone {
public:
	void apply(const char* posix_tz) override;
	bool to_local(int64_t utc_seconds, LocalTime& out) const override;
};

/// The host's network as seen by the controller: it is up as soon as the core asks to connect,
/// with the address of the first running non-loopback IPv4 interface (127.0.0.1 if there is none,
/// logged). The report is posted to the event loop, as the NetworkDriver contract requires.
class PosixNetworkDriver : public NetworkDriver {
public:
	/// Connects the driver to the controller's loop and connectivity; until then connect() refuses.
	void bind(EventLoop& loop, Connectivity& connectivity);

	/// The host network needs no credentials: always true.
	bool provisioned() const override { return true; }
	ConnectResult connect() override;
	void disconnect() override;
	/// Pairing (WPS) does not exist on the host: refused with a log line.
	bool start_pairing() override;
	void stop_pairing() override {}

	/// Writes the host's primary IPv4 address into `buffer`; false (and 127.0.0.1) if none is found.
	static bool primary_address(char* buffer, size_t size);

private:
	void report_up();

	EventLoop* loop_ = nullptr;
	Connectivity* connectivity_ = nullptr;
	char address_[Connectivity::kMaxAddress] = {};
	bool pending_ = false;
};

} // namespace posix
} // namespace hc

#endif // HC_POSIX_PLATFORM_H
