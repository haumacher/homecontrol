/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_EXACT_MATH_H
#define HC_EXACT_MATH_H

namespace hc::exact {

/// Trigonometry in degrees that gives the same bits on every platform (spec/blocks.md 13.1): built
/// from +, -, *, /, sqrt and floor alone, evaluated in the order the specification writes, so that
/// E-Home's Java port computes exactly what the device computes. A library's sin() and atan2()
/// differ in the last bits between glibc, newlib and Java. Error against the true functions: below
/// 1e-13 (sine, cosine) and 1e-12 degrees (the arc functions).

/// x - 360 floor(x / 360): 0..360.
double wrap360(double x);

/// sin and cos of `degrees`, both from one reduction.
void sin_cos_deg(double degrees, double& sin_out, double& cos_out);
double sin_deg(double degrees);
double cos_deg(double degrees);

/// The angle of (x, y) in degrees, -180..180; 0 for (0, 0).
double atan2_deg(double y, double x);

/// The arc sine in degrees of `v` limited to -1..1.
double asin_deg(double v);

} // namespace hc::exact

#endif // HC_EXACT_MATH_H
