/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <cstdlib>
#include <string>

#include "doctest.h"
#include "hc/astro.h"
#include "hc/tz.h"

using hc::GeoLocation;
using hc::SunEvent;
using hc::SunTimes;

namespace {

/// UTC seconds of a date and a time of day; `day_shift` moves the date by whole days (an event of
/// the date that falls on the neighbouring UTC date).
int64_t utc(int year, unsigned month, unsigned day, int hour, int minute, int day_shift = 0) {
	return (hc::days_from_civil(year, month, day) + day_shift) * 86400 + hour * 3600 + minute * 60;
}

/// One reference: the four events as published by the US Naval Observatory's "Sun and Moon Data
/// for One Day" (aa.usno.navy.mil/api/rstt/oneday, fetched 2026-10-17), rounded by USNO to the
/// minute; 0 marks an event that does not occur.
struct Reference {
	const char* place;
	double latitude;
	double longitude;
	int year;
	unsigned month;
	unsigned day;
	int64_t events[4]; ///< dawn, sunrise, sunset, dusk (UTC seconds), 0: none
};

int64_t worst_difference = 0;

void check_reference(const Reference& r) {
	GeoLocation where;
	where.latitude = r.latitude;
	where.longitude = r.longitude;
	SunTimes times = hc::sun_times(r.year, r.month, r.day, where);
	for (size_t i = 0; i < hc::kSunEventCount; ++i) {
		SunEvent event = static_cast<SunEvent>(i);
		INFO(r.place, " ", r.year, "-", r.month, "-", r.day, " ", hc::sun_event_name(event));
		if (r.events[i] == 0) {
			CHECK_FALSE(times.occurs(event));
			continue;
		}
		REQUIRE(times.occurs(event));
		int64_t difference = times.at(event) - r.events[i];
		INFO("difference ", difference, " s");
		// USNO rounds to the minute: a computation within a minute of the true instant is within
		// 90 s of the rounded reference; "to the minute" asks for at most 60 s.
		CHECK(std::llabs(difference) <= 60);
		if (std::llabs(difference) > worst_difference) {
			worst_difference = std::llabs(difference);
		}
	}
}

} // namespace

TEST_CASE("Astro.MatchesTheUsnoReferencesToTheMinute") {
	const Reference references[] = {
		// Greenwich on the equinoxes.
		{"Greenwich", 51.4769, 0.0, 2026, 3, 20,
			{utc(2026, 3, 20, 5, 30), utc(2026, 3, 20, 6, 3), utc(2026, 3, 20, 18, 13), utc(2026, 3, 20, 18, 46)}},
		{"Greenwich", 51.4769, 0.0, 2026, 9, 23,
			{utc(2026, 9, 23, 5, 14), utc(2026, 9, 23, 5, 48), utc(2026, 9, 23, 17, 56), utc(2026, 9, 23, 18, 29)}},
		// Munich on the solstices.
		{"Munich", 48.137, 11.575, 2026, 6, 21,
			{utc(2026, 6, 21, 2, 32), utc(2026, 6, 21, 3, 13), utc(2026, 6, 21, 19, 18), utc(2026, 6, 21, 19, 59)}},
		{"Munich", 48.137, 11.575, 2026, 12, 21,
			{utc(2026, 12, 21, 6, 25), utc(2026, 12, 21, 7, 1), utc(2026, 12, 21, 15, 22), utc(2026, 12, 21, 15, 59)}},
		// Helsinki: in June civil dawn comes before midnight UTC and dusk barely exists.
		{"Helsinki", 60.17, 24.94, 2026, 6, 21,
			{utc(2026, 6, 21, 23, 2, -1), utc(2026, 6, 21, 0, 54), utc(2026, 6, 21, 19, 50), utc(2026, 6, 21, 21, 42)}},
		{"Helsinki", 60.17, 24.94, 2026, 12, 21,
			{utc(2026, 12, 21, 6, 25), utc(2026, 12, 21, 7, 24), utc(2026, 12, 21, 13, 13), utc(2026, 12, 21, 14, 11)}},
		// Sydney (south of the equator, east of 90 E: the morning lies on the previous UTC date).
		{"Sydney", -33.87, 151.21, 2026, 6, 21,
			{utc(2026, 6, 21, 20, 32, -1), utc(2026, 6, 21, 21, 0, -1), utc(2026, 6, 21, 6, 54), utc(2026, 6, 21, 7, 22)}},
		{"Sydney", -33.87, 151.21, 2026, 12, 21,
			{utc(2026, 12, 21, 18, 11, -1), utc(2026, 12, 21, 18, 41, -1), utc(2026, 12, 21, 9, 5), utc(2026, 12, 21, 9, 35)}},
		// Buenos Aires on the equinox, Quito on the equator.
		{"Buenos Aires", -34.6, -58.38, 2026, 3, 20,
			{utc(2026, 3, 20, 9, 32), utc(2026, 3, 20, 9, 57), utc(2026, 3, 20, 22, 5), utc(2026, 3, 20, 22, 30)}},
		{"Quito", -0.18, -78.47, 2026, 1, 15,
			{utc(2026, 1, 15, 10, 57), utc(2026, 1, 15, 11, 19), utc(2026, 1, 15, 23, 27), utc(2026, 1, 15, 23, 49)}},
		// New York (west of 90 W in effect: the evening lies on the next UTC date).
		{"New York", 40.71, -74.01, 2026, 7, 4,
			{utc(2026, 7, 4, 8, 57), utc(2026, 7, 4, 9, 30), utc(2026, 7, 4, 0, 30, 1), utc(2026, 7, 4, 1, 4, 1)}},
		// Berlin on the EU spring-forward day (the local conversion is tested in TimeService).
		{"Berlin", 52.52, 13.405, 2026, 3, 29,
			{utc(2026, 3, 29, 4, 14), utc(2026, 3, 29, 4, 48), utc(2026, 3, 29, 17, 35), utc(2026, 3, 29, 18, 10)}},
		// Tromsø: polar day in June (no event at all), polar night in December (civil twilight only).
		{"Tromso", 69.65, 18.96, 2026, 6, 21, {0, 0, 0, 0}},
		{"Tromso", 69.65, 18.96, 2026, 12, 21, {utc(2026, 12, 21, 8, 31), 0, 0, utc(2026, 12, 21, 12, 53)}},
	};
	for (const Reference& r : references) {
		check_reference(r);
	}
	MESSAGE("largest difference to USNO: ", worst_difference, " s");
}

TEST_CASE("Astro.PolarDayAndNightHaveNoEvents") {
	GeoLocation north_pole;
	north_pole.latitude = 90;
	SunTimes june = hc::sun_times(2026, 6, 21, north_pole);
	SunTimes december = hc::sun_times(2026, 12, 21, north_pole);
	for (size_t i = 0; i < hc::kSunEventCount; ++i) {
		CHECK_FALSE(june.occurs(static_cast<SunEvent>(i)));
		CHECK_FALSE(december.occurs(static_cast<SunEvent>(i)));
	}
	// Tromsø late May: the midnight sun has begun; in January the sun is back.
	GeoLocation tromso;
	tromso.latitude = 69.65;
	tromso.longitude = 18.96;
	CHECK_FALSE(hc::sun_times(2026, 5, 25, tromso).occurs(SunEvent::sunset));
	CHECK(hc::sun_times(2026, 1, 25, tromso).occurs(SunEvent::sunrise));
}

TEST_CASE("Astro.ElevationLowersSunriseAndRaisesSunset") {
	GeoLocation ground;
	ground.latitude = 47.42; // Zugspitze
	ground.longitude = 10.98;
	GeoLocation summit = ground;
	summit.elevation_m = 2000;
	SunTimes low = hc::sun_times(2026, 6, 21, ground);
	SunTimes high = hc::sun_times(2026, 6, 21, summit);
	// The dip of the horizon from 2000 m is about 1.5 degrees: several minutes earlier and later.
	CHECK(low.at(SunEvent::sunrise) - high.at(SunEvent::sunrise) > 5 * 60);
	CHECK(high.at(SunEvent::sunset) - low.at(SunEvent::sunset) > 5 * 60);
	// Civil twilight is defined by the sun's depression alone.
	CHECK(low.at(SunEvent::dawn) == high.at(SunEvent::dawn));
}

TEST_CASE("Astro.NamesAndValidation") {
	SunEvent event;
	CHECK(hc::parse_sun_event("sunset", event));
	CHECK(event == SunEvent::sunset);
	CHECK(hc::parse_sun_event("dawn", event));
	CHECK(event == SunEvent::dawn);
	CHECK_FALSE(hc::parse_sun_event("noon", event));
	CHECK_FALSE(hc::parse_sun_event(nullptr, event));
	CHECK(std::string(hc::sun_event_name(SunEvent::dusk)) == "dusk");

	const char* error = nullptr;
	GeoLocation where;
	CHECK(hc::geo_location_valid(where));
	where.latitude = 90.5;
	CHECK_FALSE(hc::geo_location_valid(where, &error));
	CHECK(std::string(error) == "latitude must lie in -90..90 degrees");
	where.latitude = 0;
	where.longitude = -181;
	CHECK_FALSE(hc::geo_location_valid(where, &error));
	CHECK(std::string(error) == "longitude must lie in -180..180 degrees");
	where.longitude = 0;
	where.elevation_m = 10000;
	CHECK_FALSE(hc::geo_location_valid(where, &error));
	// An invalid place has no events.
	CHECK_FALSE(hc::sun_times(2026, 6, 21, where).occurs(SunEvent::sunrise));
}
