/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/astro.h"

#include <cmath>
#include <cstring>

#include "hc/tz.h"

namespace hc {

namespace {

constexpr double kPi = 3.14159265358979323846;

double rad(double degrees) {
	return degrees * kPi / 180.0;
}

double deg(double radians) {
	return radians * 180.0 / kPi;
}

const char* const kNames[kSunEventCount] = {"dawn", "sunrise", "sunset", "dusk"};

/// The sun's position at a moment, as far as rise and set need it.
struct SolarPosition {
	double declination; ///< Degrees.
	double equation_of_time; ///< Minutes (apparent minus mean solar time).
};

/// NOAA solar calculator (after Meeus, "Astronomical Algorithms"), `jd` a Julian day (UT).
SolarPosition solar_position(double jd) {
	double t = (jd - 2451545.0) / 36525.0; // Julian centuries since J2000.0
	double l0 = std::fmod(280.46646 + t * (36000.76983 + t * 0.0003032), 360.0);
	if (l0 < 0) {
		l0 += 360.0;
	}
	double m = 357.52911 + t * (35999.05029 - 0.0001537 * t);
	double e = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);
	double mr = rad(m);
	double center = std::sin(mr) * (1.914602 - t * (0.004817 + 0.000014 * t)) +
		std::sin(2 * mr) * (0.019993 - 0.000101 * t) + std::sin(3 * mr) * 0.000289;
	double true_longitude = l0 + center;
	double omega = 125.04 - 1934.136 * t;
	double apparent_longitude = true_longitude - 0.00569 - 0.00478 * std::sin(rad(omega));
	double seconds = 21.448 - t * (46.815 + t * (0.00059 - t * 0.001813));
	double mean_obliquity = 23.0 + (26.0 + seconds / 60.0) / 60.0;
	double obliquity = mean_obliquity + 0.00256 * std::cos(rad(omega));

	SolarPosition position;
	position.declination = deg(std::asin(std::sin(rad(obliquity)) * std::sin(rad(apparent_longitude))));

	double y = std::tan(rad(obliquity) / 2);
	y *= y;
	double l0r = rad(l0);
	double eq = y * std::sin(2 * l0r) - 2 * e * std::sin(mr) + 4 * e * y * std::sin(mr) * std::cos(2 * l0r) -
		0.5 * y * y * std::sin(4 * l0r) - 1.25 * e * e * std::sin(2 * mr);
	position.equation_of_time = 4 * deg(eq);
	return position;
}

/// The hour angle (degrees, positive) at which the sun's centre reaches `zenith` degrees from the
/// zenith; false when it never does that day (it stays above or below).
bool hour_angle(double latitude, double declination, double zenith, double& out) {
	double lat = rad(latitude);
	double dec = rad(declination);
	double denominator = std::cos(lat) * std::cos(dec);
	if (std::fabs(denominator) < 1e-12) {
		return false; // at a pole
	}
	double c = std::cos(rad(zenith)) / denominator - std::tan(lat) * std::tan(dec);
	if (c > 1.0 || c < -1.0) {
		return false;
	}
	out = deg(std::acos(c));
	return true;
}

/// One event of the date that starts at the Julian day `jd0` (0h UT): minutes after 0h UT, refined
/// by recomputing the sun's position at the previous estimate.
bool event_minutes(double jd0, const GeoLocation& where, double zenith, bool morning, double& minutes) {
	double estimate = 720.0 - 4.0 * where.longitude; // solar noon, roughly
	for (int iteration = 0; iteration < 3; ++iteration) {
		SolarPosition position = solar_position(jd0 + estimate / 1440.0);
		double ha;
		if (!hour_angle(where.latitude, position.declination, zenith, ha)) {
			return false;
		}
		double delta = morning ? ha : -ha;
		estimate = 720.0 - 4.0 * (where.longitude + delta) - position.equation_of_time;
	}
	minutes = estimate;
	return true;
}

} // namespace

const char* sun_event_name(SunEvent event) {
	size_t index = static_cast<size_t>(event);
	return index < kSunEventCount ? kNames[index] : "?";
}

bool parse_sun_event(const char* name, SunEvent& out) {
	if (name == nullptr) {
		return false;
	}
	for (size_t i = 0; i < kSunEventCount; ++i) {
		if (std::strcmp(name, kNames[i]) == 0) {
			out = static_cast<SunEvent>(i);
			return true;
		}
	}
	return false;
}

bool geo_location_valid(const GeoLocation& where, const char** error) {
	const char* reason = nullptr;
	if (!std::isfinite(where.latitude) || where.latitude < -90.0 || where.latitude > 90.0) {
		reason = "latitude must lie in -90..90 degrees";
	} else if (!std::isfinite(where.longitude) || where.longitude < -180.0 || where.longitude > 180.0) {
		reason = "longitude must lie in -180..180 degrees";
	} else if (!std::isfinite(where.elevation_m) || where.elevation_m < -500.0 || where.elevation_m > 9000.0) {
		reason = "elevation must lie in -500..9000 m";
	}
	if (reason != nullptr && error != nullptr) {
		*error = reason;
	}
	return reason == nullptr;
}

SunTimes sun_times(int32_t year, unsigned month, unsigned day, const GeoLocation& where) {
	SunTimes result;
	if (!geo_location_valid(where)) {
		return result;
	}
	int64_t days = days_from_civil(year, month, day);
	double jd0 = static_cast<double>(days) + 2440587.5;
	double dip = where.elevation_m > 0 ? 2.076 * std::sqrt(where.elevation_m) / 60.0 : 0.0;
	struct Spec {
		double zenith;
		bool morning;
	};
	const Spec specs[kSunEventCount] = {
		{96.0, true},          // dawn: civil twilight begins
		{90.833 + dip, true},  // sunrise
		{90.833 + dip, false}, // sunset
		{96.0, false},         // dusk: civil twilight ends
	};
	for (size_t i = 0; i < kSunEventCount; ++i) {
		double minutes;
		if (event_minutes(jd0, where, specs[i].zenith, specs[i].morning, minutes)) {
			result.utc[i] = days * 86400 + static_cast<int64_t>(std::llround(minutes * 60.0));
			result.present[i] = true;
		}
	}
	return result;
}

} // namespace hc
