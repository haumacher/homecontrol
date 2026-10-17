/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/posix/platform.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

void PosixTimeZone::apply(const char* posix_tz) {
	setenv("TZ", posix_tz, 1);
	tzset();
}

bool PosixTimeZone::to_local(int64_t utc_seconds, LocalTime& out) const {
	time_t seconds = static_cast<time_t>(utc_seconds);
	tm local;
	if (localtime_r(&seconds, &local) == nullptr) {
		return false;
	}
	out.year = local.tm_year + 1900;
	out.month = static_cast<uint8_t>(local.tm_mon + 1);
	out.day = static_cast<uint8_t>(local.tm_mday);
	out.hour = static_cast<uint8_t>(local.tm_hour);
	out.minute = static_cast<uint8_t>(local.tm_min);
	out.second = static_cast<uint8_t>(local.tm_sec);
	out.dst = local.tm_isdst > 0;
	if (strftime(out.zone, sizeof(out.zone), "%Z", &local) == 0) {
		out.zone[0] = '\0';
	}
	return true;
}

namespace {

constexpr const char* kNetTag = "hc.posix.net";

} // namespace

void PosixNetworkDriver::bind(EventLoop& loop, Connectivity& connectivity) {
	loop_ = &loop;
	connectivity_ = &connectivity;
}

bool PosixNetworkDriver::primary_address(char* buffer, size_t size) {
	ifaddrs* list = nullptr;
	bool found = false;
	if (getifaddrs(&list) == 0) {
		for (ifaddrs* entry = list; entry != nullptr && !found; entry = entry->ifa_next) {
			if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) {
				continue;
			}
			if ((entry->ifa_flags & IFF_UP) == 0 || (entry->ifa_flags & IFF_RUNNING) == 0 ||
				(entry->ifa_flags & IFF_LOOPBACK) != 0) {
				continue;
			}
			const sockaddr_in* address = reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);
			found = inet_ntop(AF_INET, &address->sin_addr, buffer, static_cast<socklen_t>(size)) != nullptr;
		}
		freeifaddrs(list);
	}
	if (!found) {
		std::snprintf(buffer, size, "127.0.0.1");
	}
	return found;
}

ConnectResult PosixNetworkDriver::connect() {
	if (loop_ == nullptr || connectivity_ == nullptr) {
		Log::error(kNetTag, "refusing connect: the driver is not bound to a controller");
		return ConnectResult::refused;
	}
	if (!primary_address(address_, sizeof(address_))) {
		Log::warn(kNetTag, "no running non-loopback IPv4 interface found, reporting 127.0.0.1");
	}
	if (!pending_) {
		if (!loop_->post(Callback::member<PosixNetworkDriver, &PosixNetworkDriver::report_up>(this))) {
			return ConnectResult::failed;
		}
		pending_ = true;
	}
	return ConnectResult::started;
}

void PosixNetworkDriver::disconnect() {
	// The host's network is not ours to drop; a pending report is discarded.
	pending_ = false;
	Log::info(kNetTag, "disconnect: the host network stays as it is, the controller treats it as down");
}

bool PosixNetworkDriver::start_pairing() {
	Log::warn(kNetTag, "refusing pairing: WPS does not exist on the host build");
	return false;
}

void PosixNetworkDriver::report_up() {
	if (!pending_) {
		return;
	}
	pending_ = false;
	connectivity_->link_up(address_);
}

} // namespace posix
} // namespace hc
