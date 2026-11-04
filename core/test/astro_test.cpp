/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <cstdlib>
#include <string>

#include "doctest.h"
#include "hc/astro.h"
#include "hc/time_service.h"
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

TEST_CASE("Astro.EveryEventLiesWithinTheSunWindow") {
	// Every third day of two years, latitudes from pole to pole, all longitudes, two elevations.
	size_t events = 0;
	for (int latitude = -89; latitude <= 89; latitude += 8) {
		for (int longitude = -180; longitude <= 180; longitude += 20) {
			for (double elevation : {0.0, 4000.0}) {
				GeoLocation where;
				where.latitude = latitude;
				where.longitude = longitude;
				where.elevation_m = elevation;
				int64_t first = hc::days_from_civil(2026, 1, 1);
				for (int64_t days = first; days < first + 730; days += 3) {
					hc::Date date = hc::date_from_days(days);
					int year = date.year;
					unsigned month = date.month;
					unsigned day = date.day;
					SunTimes times = hc::sun_times(year, month, day, where);
					hc::SunWindow window = hc::sun_window(year, month, day, where);
					for (size_t i = 0; i < hc::kSunEventCount; ++i) {
						if (!times.present[i]) {
							continue;
						}
						++events;
						INFO(latitude << "/" << longitude << " " << year << "-" << month << "-" << day << " event " << i);
						CHECK(times.utc[i] >= window.earliest);
						CHECK(times.utc[i] <= window.latest);
					}
				}
			}
		}
	}
	CHECK(events > 100000);

	GeoLocation invalid;
	invalid.latitude = 91;
	hc::SunWindow none = hc::sun_window(2026, 6, 21, invalid);
	CHECK(none.earliest > none.latest);
}

// ---- The sun's position (spec/blocks.md 13.1, 13.2) --------------------------------------------

#include <cmath>

#include "hc/exact_math.h"

namespace {

constexpr double kPiTest = 3.14159265358979323846;

double rad(double degrees) { return degrees * kPiTest / 180.0; }
double deg(double radians) { return radians * 180.0 / kPiTest; }

/// Spec 13.2 transcribed on the library's functions: checks the transcription of the formulas,
/// independent of the exact trigonometry.
hc::SunPosition library_sun(int64_t u, double lat, double lon) {
	double d = static_cast<double>(u - 946728000) / 86400.0;
	double T = d / 36525.0;
	double L0 = std::fmod(280.46646 + T * (36000.76983 + T * 0.0003032), 360.0);
	double M = 357.52911 + T * (35999.05029 - 0.0001537 * T);
	double e = 0.016708634 - T * (0.000042037 + 0.0000001267 * T);
	double C = std::sin(rad(M)) * (1.914602 - T * (0.004817 + 0.000014 * T)) +
		std::sin(rad(2 * M)) * (0.019993 - 0.000101 * T) + std::sin(rad(3 * M)) * 0.000289;
	double Om = 125.04 - 1934.136 * T;
	double lam = L0 + C - 0.00569 - 0.00478 * std::sin(rad(Om));
	double eps = 23.0 + (26.0 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0 +
		0.00256 * std::cos(rad(Om));
	double dec = std::asin(std::sin(rad(eps)) * std::sin(rad(lam)));
	double y = std::tan(rad(eps) / 2);
	y *= y;
	double E = 4 * deg(y * std::sin(2 * rad(L0)) - 2 * e * std::sin(rad(M)) +
		4 * e * y * std::sin(rad(M)) * std::cos(2 * rad(L0)) - 0.5 * y * y * std::sin(4 * rad(L0)) -
		1.25 * e * e * std::sin(2 * rad(M)));
	int64_t s = ((u % 86400) + 86400) % 86400;
	double H = rad((static_cast<double>(s) / 60.0 + E + 4.0 * lon) / 4.0 - 180.0);
	double la = rad(lat);
	double up = std::sin(la) * std::sin(dec) + std::cos(la) * std::cos(dec) * std::cos(H);
	double north = std::cos(la) * std::sin(dec) - std::sin(la) * std::cos(dec) * std::cos(H);
	double east = -std::cos(dec) * std::sin(H);
	hc::SunPosition p;
	p.elevation = deg(std::atan2(up, std::hypot(north, east)));
	p.azimuth = std::fmod(deg(std::atan2(east, north)) + 360.0, 360.0);
	return p;
}

/// The Astronomical Almanac's low-precision formula for the sun (accurate to 0.01 degrees between
/// 1950 and 2050): independent of the NOAA algorithm.
hc::SunPosition almanac_sun(int64_t u, double lat, double lon) {
	double n = static_cast<double>(u) / 86400.0 + 2440587.5 - 2451545.0;
	double L = std::fmod(280.460 + 0.9856474 * n, 360.0);
	double g = rad(std::fmod(357.528 + 0.9856003 * n, 360.0));
	double lam = rad(L + 1.915 * std::sin(g) + 0.020 * std::sin(2 * g));
	double eps = rad(23.439 - 0.0000004 * n);
	double ra = std::atan2(std::cos(eps) * std::sin(lam), std::cos(lam));
	double dec = std::asin(std::sin(eps) * std::sin(lam));
	double gmst = std::fmod(18.697374558 + 24.06570982441908 * n, 24.0);
	double ha = rad(gmst * 15 + lon) - ra;
	double la = rad(lat);
	hc::SunPosition p;
	p.elevation = deg(std::asin(std::sin(la) * std::sin(dec) + std::cos(la) * std::cos(dec) * std::cos(ha)));
	p.azimuth = std::fmod(
		deg(std::atan2(-std::sin(ha), std::tan(dec) * std::cos(la) - std::sin(la) * std::cos(ha))) + 720.0, 360.0);
	return p;
}

double angle_difference(double a, double b) {
	double d = std::fmod(a - b + 540.0, 360.0) - 180.0;
	return std::fabs(d);
}

} // namespace

TEST_CASE("ExactMath.SineAndCosineMatchTheLibrary") {
	double worst = 0;
	for (int i = -7200; i <= 7200; ++i) {
		double x = i * 0.137;
		double s, c;
		hc::exact::sin_cos_deg(x, s, c);
		worst = std::fmax(worst, std::fabs(s - std::sin(rad(x))));
		worst = std::fmax(worst, std::fabs(c - std::cos(rad(x))));
		CHECK(s == hc::exact::sin_deg(x));
		CHECK(c == hc::exact::cos_deg(x));
	}
	CHECK(worst < 1e-13);
	// The quadrants are exact.
	CHECK(hc::exact::sin_deg(0) == 0);
	CHECK(hc::exact::sin_deg(90) == 1);
	CHECK(hc::exact::cos_deg(180) == -1);
	CHECK(hc::exact::sin_deg(-90) == -1);
	CHECK(hc::exact::cos_deg(360) == 1);
	CHECK(hc::exact::wrap360(-30) == 330);
	CHECK(hc::exact::wrap360(725) == 5);
}

TEST_CASE("ExactMath.ArcFunctionsMatchTheLibrary") {
	double worst = 0;
	uint32_t state = 12345;
	auto next = [&state]() {
		state = state * 1103515245u + 12345u;
		return static_cast<double>((state >> 8) & 0xffff) / 65535.0 * 10.0 - 5.0;
	};
	for (int i = 0; i < 100000; ++i) {
		double y = next();
		double x = next();
		worst = std::fmax(worst, std::fabs(hc::exact::atan2_deg(y, x) - deg(std::atan2(y, x))));
	}
	for (int i = -1000; i <= 1000; ++i) {
		double v = i / 1000.0;
		worst = std::fmax(worst, std::fabs(hc::exact::asin_deg(v) - deg(std::asin(v))));
	}
	CHECK(worst < 1e-12);
	CHECK(hc::exact::atan2_deg(0, 0) == 0);
	CHECK(std::fabs(hc::exact::atan2_deg(1, 1) - 45) < 1e-12);
	CHECK(hc::exact::atan2_deg(0, -1) == 180);
	CHECK(hc::exact::atan2_deg(-1, 0) == -90);
	CHECK(hc::exact::asin_deg(1) == 90);
	CHECK(hc::exact::asin_deg(1.5) == 90); // limited to -1..1
	CHECK(hc::exact::asin_deg(-1) == -90);
}

TEST_CASE("SunPosition.TheNrelSpaExample") {
	// NREL's Solar Position Algorithm (Reda and Andreas 2003, Table A4.1): 2003-10-17 12:30:30 at
	// UTC-7, 39.742476 N, 105.1786 W: topocentric zenith 50.11162 degrees (refraction at 820 mbar and
	// 11 degrees C: +0.0163 degrees of elevation; parallax: -0.0019), azimuth 194.34024.
	GeoLocation golden;
	golden.latitude = 39.742476;
	golden.longitude = -105.1786;
	int64_t u = utc(2003, 10, 17, 19, 30) + 30;
	hc::SunPosition p = hc::sun_position(u, golden);
	double geometric = (90.0 - 50.11162) - 0.0163 + 0.0019;
	CHECK(std::fabs(p.elevation - geometric) < 0.02);
	CHECK(std::fabs(p.azimuth - 194.34024) < 0.02);
	// The bits of the specification's formulas (a Python transcription of spec/blocks.md 13.1 and
	// 13.2 in binary64 gives exactly these; E-Home's Java gives them too).
	CHECK(p.azimuth == 194.34258286147883);
	CHECK(p.elevation == 39.87207442405636);
}

TEST_CASE("SunPosition.FollowsTheFormulasAndTheAlmanac") {
	double worst_formula = 0;
	double worst_almanac = 0;
	const double latitudes[] = {-60, -33.9, 0, 23, 48.137, 60, 66};
	const double longitudes[] = {-122.4, 0, 11.576, 151.2};
	for (double lat : latitudes) {
		for (double lon : longitudes) {
			GeoLocation where;
			where.latitude = lat;
			where.longitude = lon;
			for (int day = 0; day < 365; day += 9) {
				for (int hour = 0; hour < 24; hour += 2) {
					int64_t u = utc(2026, 1, 1, hour, 17) + day * 86400;
					hc::SunPosition p = hc::sun_position(u, where);
					REQUIRE(p.azimuth >= 0);
					REQUIRE(p.azimuth <= 360);
					hc::SunPosition f = library_sun(u, lat, lon);
					worst_formula = std::fmax(worst_formula, std::fabs(p.elevation - f.elevation));
					worst_formula = std::fmax(worst_formula, angle_difference(p.azimuth, f.azimuth));
					if (p.elevation > -5) {
						hc::SunPosition a = almanac_sun(u, lat, lon);
						worst_almanac = std::fmax(worst_almanac, std::fabs(p.elevation - a.elevation));
						worst_almanac = std::fmax(worst_almanac,
							angle_difference(p.azimuth, a.azimuth) * std::cos(rad(p.elevation)));
					}
				}
			}
		}
	}
	MESSAGE("worst against the library transcription " << worst_formula << ", against the almanac " << worst_almanac);
	CHECK(worst_formula < 1e-9);
	CHECK(worst_almanac < 0.02);
}

TEST_CASE("SunPosition.NoonInMunichAtTheSolstice") {
	// At the June solstice the sun's noon elevation is 90 - latitude + 23.44; it culminates in the
	// south.
	GeoLocation munich;
	munich.latitude = 48.137;
	munich.longitude = 11.576;
	double best = -90;
	double azimuth_at_best = 0;
	for (int minute = 0; minute < 24 * 60; ++minute) {
		hc::SunPosition p = hc::sun_position(utc(2026, 6, 21, 0, 0) + minute * 60, munich);
		if (p.elevation > best) {
			best = p.elevation;
			azimuth_at_best = p.azimuth;
		}
	}
	CHECK(best == doctest::Approx(90 - 48.137 + 23.44).epsilon(0.001));
	CHECK(std::fabs(azimuth_at_best - 180) < 0.5);
}
