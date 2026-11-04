/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_BLOCKS_HEATING_CURVE_H
#define HC_BLOCKS_HEATING_CURVE_H

#include "hc/blocks/block_type.h"

namespace hc::blocks {

/// The heating curve `org.homecontrol.heating-curve` 1.0 (spec/blocks.md 17): the flow setpoint over
/// the filtered outside temperature (slope and level, a design point or 2..6 points), shifted by the
/// room setpoint and an offset, limited; the summer/winter switch from the filtered temperature;
/// frost protection; a missing outside temperature holds the filter.
class HeatingCurve : public BlockType {
public:
	// Ports, in manifest order.
	enum Port : uint8_t {
		kOutside, kRoomSetpoint, kOffset, kEnable, kSummerSwitch, kFlowSetpoint, kSummer, kOutsideFiltered, kState,
		kPortCount
	};
	// Parameters, in manifest order.
	enum Param : uint8_t {
		kCurve, kSlope, kLevel, kRoomTemperature, kDesignOutside, kDesignFlow, kPointCount,
		kOutside1, kFlow1, kOutside2, kFlow2, kOutside3, kFlow3, kOutside4, kFlow4, kOutside5, kFlow5, kOutside6, kFlow6,
		kMinFlow, kMaxFlow, kMinChange, kOutsideFilter, kSummerOn, kSummerOff, kSummerDays, kFrostOutside, kOffFlow,
		kSensorTimeout, kParamCount
	};
	// State fields, in manifest order.
	enum Field : uint8_t {
		kFiltered, kHasFiltered, kAboveFor, kDerivedSummer, kLastStep, kFrostOn, kHasSent, kSentFlow, kSentSummer,
		kFieldCount
	};
	/// The values of the output `state`.
	enum State : uint8_t { heating, summer, off, frost, fault };
	enum Form : uint8_t { slope, design, points };

	static const HeatingCurve& instance();

	/// The flow of the points of `params` at `x` (spec/blocks.md 17.4 `interpolate`).
	static double interpolate(const double* params, double x);

	const Manifest& manifest() const override;
	const char* check(const double* params, size_t& parameter) const override;
	void start(BlockContext& cx, StartReason reason, bool restored) const override;
	void input(BlockContext& cx, size_t port) const override;
	void timer(BlockContext& cx, size_t timer) const override;
	void period(BlockContext& cx) const override;
	void parameter(BlockContext& cx, size_t index) const override;

private:
	void step(BlockContext& cx) const;
};

} // namespace hc::blocks

#endif // HC_BLOCKS_HEATING_CURVE_H
