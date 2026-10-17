/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <string>

#include "doctest.h"
#include "hc/tz.h"

using hc::posix_tz_valid;

TEST_CASE("Tz.ValidPosixStrings") {
	CHECK(posix_tz_valid("CET-1CEST,M3.5.0,M10.5.0/3"));
	CHECK(posix_tz_valid("UTC0"));
	CHECK(posix_tz_valid("GMT0BST,M3.5.0/1,M10.5.0"));
	CHECK(posix_tz_valid("EST5EDT,M3.2.0/2:00:00,M11.1.0/2:00:00"));
	CHECK(posix_tz_valid("<+0330>-3:30"));
	CHECK(posix_tz_valid("<-03>3"));
	CHECK(posix_tz_valid("NZST-12NZDT-13,M9.5.0,M4.1.0/3"));
	CHECK(posix_tz_valid("XXX3YYY,J60,300/-1"));
	CHECK(posix_tz_valid("AAA+5:30"));
	CHECK(posix_tz_valid("<+01>-1<+02>,M3.5.0/-1,M10.5.0/167"));
}

TEST_CASE("Tz.InvalidStringsAreRefusedWithAReason") {
	const char* error = nullptr;
	CHECK_FALSE(posix_tz_valid("", &error));
	CHECK(std::string(error) == "empty time zone");
	CHECK_FALSE(posix_tz_valid(nullptr));
	CHECK_FALSE(posix_tz_valid("Europe/Berlin", &error));
	CHECK(std::string(error) == "standard offset missing or invalid ([+-]hh[:mm[:ss]], hh <= 24)");
	CHECK_FALSE(posix_tz_valid(":Europe/Berlin", &error));
	CHECK_FALSE(posix_tz_valid("CET", &error));
	CHECK_FALSE(posix_tz_valid("CE-1", &error));
	CHECK(std::string(error) == "standard zone name must be 3+ letters or <...>");
	CHECK_FALSE(posix_tz_valid("CET-25", &error));
	CHECK_FALSE(posix_tz_valid("CET-1:60", &error));
	CHECK_FALSE(posix_tz_valid("CET-1CEST", &error));
	CHECK(std::string(error) == "a zone with daylight saving time needs both transition rules (,start,end)");
	CHECK_FALSE(posix_tz_valid("CET-1CEST,M3.5.0", &error));
	CHECK(std::string(error) == "end rule missing");
	CHECK_FALSE(posix_tz_valid("CET-1CEST,M13.5.0,M10.5.0", &error));
	CHECK_FALSE(posix_tz_valid("CET-1CEST,M3.6.0,M10.5.0"));
	CHECK_FALSE(posix_tz_valid("CET-1CEST,M3.5.7,M10.5.0"));
	CHECK_FALSE(posix_tz_valid("CET-1CEST,J0,M10.5.0"));
	CHECK_FALSE(posix_tz_valid("CET-1CEST,366,M10.5.0"));
	CHECK_FALSE(posix_tz_valid("CET-1CEST,M3.5.0,M10.5.0/168"));
	CHECK_FALSE(posix_tz_valid("CET-1CEST,M3.5.0,M10.5.0x", &error));
	CHECK(std::string(error) == "unexpected characters after the end rule");
	CHECK_FALSE(posix_tz_valid("<+0>1"));
	CHECK_FALSE(posix_tz_valid("<+0330-3:30"));
}

TEST_CASE("Tz.DaysFromCivil") {
	CHECK(hc::days_from_civil(1970, 1, 1) == 0);
	CHECK(hc::days_from_civil(2000, 3, 1) == 11017);
	CHECK(hc::days_from_civil(2026, 10, 17) == 1792224000 / 86400);
	CHECK(hc::days_from_civil(1969, 12, 31) == -1);
	CHECK(hc::days_from_civil(2024, 2, 29) + 1 == hc::days_from_civil(2024, 3, 1));
}

TEST_CASE("Tz.OffsetAndFormatting") {
	// 2026-10-17T08:00:00Z is 10:00 CEST.
	hc::LocalTime cest{2026, 10, 17, 10, 0, 0, true, "CEST"};
	CHECK(hc::utc_offset_seconds(cest, 1792224000) == 7200);
	char text[64];
	hc::format_local_time(cest, 1792224000, text, sizeof(text));
	CHECK(std::string(text) == "2026-10-17 10:00:00 CEST (UTC+02:00)");

	// Across midnight backwards, a negative half-hour offset, no zone name.
	hc::LocalTime west{2026, 10, 16, 20, 30, 5, false, ""};
	CHECK(hc::utc_offset_seconds(west, 1792224005) == -(11 * 3600 + 30 * 60));
	hc::format_local_time(west, 1792224005, text, sizeof(text));
	CHECK(std::string(text) == "2026-10-16 20:30:05 (UTC-11:30)");
}
