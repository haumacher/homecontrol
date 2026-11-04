/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The exact trigonometry of spec/blocks.md 13.1. Every expression is written in the order of the
// specification; the build has -ffp-contract=off, so no multiply-add is fused.
#include "hc/exact_math.h"

#include <algorithm>
#include <cmath>

namespace hc::exact {

namespace {

constexpr double kPi = 3.141592653589793;
constexpr double kRad = kPi / 180.0;
constexpr double kDeg = 180.0 / kPi;

constexpr double S3 = -1.0 / 6.0;
constexpr double S5 = 1.0 / 120.0;
constexpr double S7 = -1.0 / 5040.0;
constexpr double S9 = 1.0 / 362880.0;
constexpr double S11 = -1.0 / 39916800.0;
constexpr double S13 = 1.0 / 6227020800.0;

constexpr double C2 = -1.0 / 2.0;
constexpr double C4 = 1.0 / 24.0;
constexpr double C6 = -1.0 / 720.0;
constexpr double C8 = 1.0 / 40320.0;
constexpr double C10 = -1.0 / 3628800.0;
constexpr double C12 = 1.0 / 479001600.0;
constexpr double C14 = -1.0 / 87178291200.0;

constexpr double A3 = -1.0 / 3.0;
constexpr double A5 = 1.0 / 5.0;
constexpr double A7 = -1.0 / 7.0;
constexpr double A9 = 1.0 / 9.0;
constexpr double A11 = -1.0 / 11.0;
constexpr double A13 = 1.0 / 13.0;
constexpr double A15 = -1.0 / 15.0;
constexpr double A17 = 1.0 / 17.0;

} // namespace

double wrap360(double x) {
	return x - 360.0 * std::floor(x / 360.0);
}

void sin_cos_deg(double degrees, double& sin_out, double& cos_out) {
	const double r = wrap360(degrees);
	const double k = std::floor(r / 90.0 + 0.5);
	const double z = (r - 90.0 * k) * kRad;
	const double z2 = z * z;
	const double s = z + z * z2 * (S3 + z2 * (S5 + z2 * (S7 + z2 * (S9 + z2 * (S11 + z2 * S13)))));
	const double c = 1.0 + z2 * (C2 + z2 * (C4 + z2 * (C6 + z2 * (C8 + z2 * (C10 + z2 * (C12 + z2 * C14))))));
	switch (static_cast<int>(k)) {
		case 1: sin_out = c; cos_out = -s; break;
		case 2: sin_out = -s; cos_out = -c; break;
		case 3: sin_out = -c; cos_out = s; break;
		default: sin_out = s; cos_out = c; break; // 0 and 4
	}
}

double sin_deg(double degrees) {
	double s, c;
	sin_cos_deg(degrees, s, c);
	return s;
}

double cos_deg(double degrees) {
	double s, c;
	sin_cos_deg(degrees, s, c);
	return c;
}

double atan2_deg(double y, double x) {
	if (x == 0 && y == 0) {
		return 0;
	}
	const double ax = std::fabs(x);
	const double ay = std::fabs(y);
	const bool swap = ay > ax;
	double t = swap ? ax / ay : ay / ax;
	t = t / (1.0 + std::sqrt(1.0 + t * t));
	t = t / (1.0 + std::sqrt(1.0 + t * t));
	const double t2 = t * t;
	const double a =
		t + t * t2 * (A3 + t2 * (A5 + t2 * (A7 + t2 * (A9 + t2 * (A11 + t2 * (A13 + t2 * (A15 + t2 * A17)))))));
	double d = 4.0 * a * kDeg;
	if (swap) {
		d = 90.0 - d;
	}
	if (x < 0) {
		d = 180.0 - d;
	}
	if (y < 0) {
		d = -d;
	}
	return d;
}

double asin_deg(double v) {
	const double w = std::min(std::max(v, -1.0), 1.0);
	return atan2_deg(w, std::sqrt((1.0 - w) * (1.0 + w)));
}

} // namespace hc::exact
