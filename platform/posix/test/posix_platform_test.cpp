/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <cstdlib>
#include <ctime>
#include <string>

#include "doctest.h"
#include "hc/connectivity.h"
#include "hc/event_loop.h"
#include "hc/posix/platform.h"
#include "test_support.h"

using hc_test::ScopedCapture;

namespace {

/// Restores the TZ environment variable after a test.
struct TzGuard {
	TzGuard() {
		const char* tz = std::getenv("TZ");
		had = tz != nullptr;
		if (had) {
			previous = tz;
		}
	}
	~TzGuard() {
		if (had) {
			setenv("TZ", previous.c_str(), 1);
		} else {
			unsetenv("TZ");
		}
		tzset();
	}
	bool had = false;
	std::string previous;
};

} // namespace

TEST_CASE("PosixTimeZone.AppliesThePosixRule") {
	TzGuard guard;
	hc::posix::PosixTimeZone zone;
	hc::LocalTime local;

	zone.apply("CET-1CEST,M3.5.0,M10.5.0/3");
	REQUIRE(zone.to_local(1792224000, local)); // 2026-10-17T08:00:00Z
	CHECK(local.hour == 10);
	CHECK(local.dst);
	CHECK(std::string(local.zone) == "CEST");
	CHECK(hc::utc_offset_seconds(local, 1792224000) == 7200);

	REQUIRE(zone.to_local(1792224000 + 14 * 86400, local)); // 2026-10-31, after the switch on the 25th
	CHECK(local.hour == 9);
	CHECK_FALSE(local.dst);
	CHECK(std::string(local.zone) == "CET");

	zone.apply("UTC0");
	REQUIRE(zone.to_local(1792224000, local));
	CHECK(local.hour == 8);
	CHECK(local.year == 2026);
	CHECK(local.month == 10);
	CHECK(local.day == 17);
}

TEST_CASE("PosixNetworkDriver.ReportsUpThroughTheLoop") {
	ScopedCapture capture;
	hc_test::FakeClock clock;
	hc::EventLoop loop;
	hc::posix::PosixNetworkDriver driver;
	hc::Connectivity connectivity(loop, clock, driver);

	CHECK(driver.connect() == hc::ConnectResult::refused); // not bound yet
	CHECK(capture.sink.contains("refusing connect: the driver is not bound"));

	driver.bind(loop, connectivity);
	connectivity.start();
	CHECK(connectivity.state() == hc::LinkState::connecting);
	loop.run_once(clock.now_ms);
	CHECK(connectivity.state() == hc::LinkState::up);
	char expected[hc::Connectivity::kMaxAddress];
	hc::posix::PosixNetworkDriver::primary_address(expected, sizeof(expected));
	CHECK(std::string(connectivity.ip()) == expected);
	CHECK(loop.timer_count() == 0);
}

TEST_CASE("PosixNetworkDriver.DisconnectDiscardsAPendingReport") {
	ScopedCapture capture;
	hc_test::FakeClock clock;
	hc::EventLoop loop;
	hc::posix::PosixNetworkDriver driver;
	hc::Connectivity connectivity(loop, clock, driver);
	driver.bind(loop, connectivity);
	connectivity.start();
	driver.disconnect();
	loop.run_once(clock.now_ms);
	CHECK(connectivity.state() == hc::LinkState::connecting); // the report was discarded
	CHECK_FALSE(driver.start_pairing());
	CHECK(capture.sink.contains("refusing pairing: WPS does not exist on the host build"));
}
