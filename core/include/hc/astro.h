/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ASTRO_H
#define HC_ASTRO_H

#include <cstddef>
#include <cstdint>

namespace hc {

/// The sun events of a day. Dawn and dusk are the begin and end of civil twilight (the sun's centre
/// 6 degrees below the horizon); sunrise and sunset are the moments the upper limb touches the
/// horizon (90.833 degrees from the zenith: refraction and the sun's radius, plus the dip of the
/// horizon for an elevated observer).
enum class SunEvent : uint8_t {
	dawn,
	sunrise,
	sunset,
	dusk,
};

constexpr size_t kSunEventCount = 4;

/// "dawn", "sunrise", "sunset", "dusk" (the spelling of spec/schedules.md and spec/rules.md).
const char* sun_event_name(SunEvent event);

/// The event named `name` (one of the names above); false for any other text.
bool parse_sun_event(const char* name, SunEvent& out);

/// A place on the WGS84 ellipsoid.
struct GeoLocation {
	double latitude = 0;    ///< Degrees, north positive, -90..90.
	double longitude = 0;   ///< Degrees, east positive, -180..180.
	double elevation_m = 0; ///< Metres above the surroundings' horizon; lowers sunrise and raises sunset.
};

/// The sun events of one date. An event that does not occur (polar day or night, or a summer night
/// that never gets dark enough for civil dusk) is absent: occurs() is false.
struct SunTimes {
	int64_t utc[kSunEventCount] = {}; ///< Seconds since the epoch (UTC), valid where occurs().
	bool present[kSunEventCount] = {};

	bool occurs(SunEvent event) const { return present[static_cast<size_t>(event)]; }
	int64_t at(SunEvent event) const { return utc[static_cast<size_t>(event)]; }
};

/// The sun events of the calendar date `year-month-day` at `where`, after the NOAA solar
/// calculator (Meeus' low-precision solar coordinates, refined at the event time). The events of a
/// date are those around its solar noon at the place's longitude: dawn and sunrise before, sunset
/// and dusk after. They are UTC instants and may fall on the neighbouring UTC date (east of 90 E or
/// west of 90 W), and at high latitudes in summer dusk may fall after the following local midnight.
/// Accuracy: about a minute between the polar circles (doc/time.md), worse within minutes of a
/// polar day or night boundary, where the event exists only barely.
SunTimes sun_times(int32_t year, unsigned month, unsigned day, const GeoLocation& where);

/// The span of UTC instants within which every event of a date lies (SunWindow::earliest ..
/// latest, inclusive), however the sun stands: polar day, night or any elevation. Cheap: no
/// trigonometry, so a caller can decide from it whether a date's events are worth computing.
struct SunWindow {
	int64_t earliest = 0;
	int64_t latest = 0;
};

/// The window of the events of `year-month-day` at `where` (the date as for sun_times()). An event
/// of the date is its solar noon moved by an hour angle of 0..180 degrees, and solar noon is 12:00
/// minus 4 minutes per degree of longitude minus the equation of time, whose magnitude the NOAA
/// formula bounds by 18.5 minutes (the sum of its terms' amplitudes): so the window is solar mean
/// noon -12 h -20 min .. +12 h +20 min. For an invalid location the window is empty
/// (earliest > latest), as sun_times() has no events then.
SunWindow sun_window(int32_t year, unsigned month, unsigned day, const GeoLocation& where);

/// The sun's position at an instant.
struct SunPosition {
	double azimuth = 0;   ///< Degrees from north through east, 0..360.
	double elevation = 0; ///< Degrees above the horizon, -90..90; geometric (no refraction).
};

/// The sun's position at the UTC instant `utc_seconds` seen from `where` (its elevation above the
/// ellipsoid is ignored): the NOAA solar calculator (Meeus' low-precision solar coordinates)
/// evaluated at the instant, on the exact trigonometry of hc/exact_math.h, written exactly as
/// spec/blocks.md 13.2 so that E-Home's simulation computes the same bits. Accuracy: within 0.02
/// degrees of the true position between 1900 and 2100 (core/test/astro_test.cpp checks it against
/// the NREL SPA example and the Astronomical Almanac's formula); refraction, which lifts the visible
/// sun by up to 0.6 degrees at the horizon, is not applied.
SunPosition sun_position(int64_t utc_seconds, const GeoLocation& where);

/// Whether the coordinates are in range (latitude -90..90, longitude -180..180, elevation
/// -500..9000 m, all finite). On refusal `error` (if given) names the problem.
bool geo_location_valid(const GeoLocation& where, const char** error = nullptr);

} // namespace hc

#endif // HC_ASTRO_H
