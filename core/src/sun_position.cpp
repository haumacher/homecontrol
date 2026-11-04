/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The sun's position of spec/blocks.md 13.2: the NOAA solar calculator at an instant, on the exact
// trigonometry of 13.1. Written in the order of the specification (compiled with
// -ffp-contract=off): E-Home's Java port must give the same bits.
#include <cmath>
#include <cstdint>

#include "hc/astro.h"
#include "hc/exact_math.h"

namespace hc {

namespace {

constexpr double kPi = 3.141592653589793;
constexpr double kDeg = 180.0 / kPi;

} // namespace

SunPosition sun_position(int64_t utc_seconds, const GeoLocation& where) {
	using exact::asin_deg;
	using exact::atan2_deg;
	using exact::cos_deg;
	using exact::sin_deg;
	using exact::wrap360;

	const int64_t u = utc_seconds;
	const double d = static_cast<double>(u - 946728000) / 86400.0;
	const double T = d / 36525.0;
	const double L0 = wrap360(280.46646 + T * (36000.76983 + T * 0.0003032));
	const double M = 357.52911 + T * (35999.05029 - 0.0001537 * T);
	const double e = 0.016708634 - T * (0.000042037 + 0.0000001267 * T);
	const double sinM = sin_deg(M);
	const double sin2M = sin_deg(2.0 * M);
	const double C = sinM * (1.914602 - T * (0.004817 + 0.000014 * T)) + sin2M * (0.019993 - 0.000101 * T) +
		sin_deg(3.0 * M) * 0.000289;
	const double Om = 125.04 - 1934.136 * T;
	double sinOm, cosOm;
	exact::sin_cos_deg(Om, sinOm, cosOm);
	const double lam = L0 + C - 0.00569 - 0.00478 * sinOm;
	const double eps = 23.0 + (26.0 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0 +
		0.00256 * cosOm;
	const double dec = asin_deg(sin_deg(eps) * sin_deg(lam));
	double sinHalf, cosHalf;
	exact::sin_cos_deg(eps / 2.0, sinHalf, cosHalf);
	double y = sinHalf / cosHalf;
	y = y * y;
	double sin2L0, cos2L0;
	exact::sin_cos_deg(2.0 * L0, sin2L0, cos2L0);
	const double E = 4.0 *
		(y * sin2L0 - 2.0 * e * sinM + 4.0 * e * y * sinM * cos2L0 - 0.5 * y * y * sin_deg(4.0 * L0) -
			1.25 * e * e * sin2M) *
		kDeg;
	int64_t days = u / 86400;
	if (u % 86400 < 0) {
		--days;
	}
	const int64_t s = u - 86400 * days;
	const double H = (static_cast<double>(s) / 60.0 + E + 4.0 * where.longitude) / 4.0 - 180.0;

	double sinLat, cosLat, sinDec, cosDec, sinH, cosH;
	exact::sin_cos_deg(where.latitude, sinLat, cosLat);
	exact::sin_cos_deg(dec, sinDec, cosDec);
	exact::sin_cos_deg(H, sinH, cosH);
	const double up = sinLat * sinDec + cosLat * cosDec * cosH;
	const double north = cosLat * sinDec - sinLat * cosDec * cosH;
	const double east = -(cosDec * sinH);

	SunPosition position;
	position.elevation = atan2_deg(up, std::sqrt(north * north + east * east));
	position.azimuth = wrap360(atan2_deg(east, north));
	return position;
}

} // namespace hc
