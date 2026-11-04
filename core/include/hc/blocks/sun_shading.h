/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_BLOCKS_SUN_SHADING_H
#define HC_BLOCKS_SUN_SHADING_H

#include "hc/blocks/block_type.h"

namespace hc::blocks {

/// The sun shading `org.homecontrol.sun-shading` 1.0 (spec/blocks.md 18): one facade's blind lowered
/// while the sun is on the facade and it is bright, the slats at the cut-off angle, heat protection
/// while nobody is at home, retracted for wind, rain and frost, suspended after an operation; moves
/// on changes only, no more often than a minimum interval.
class SunShading : public BlockType {
public:
	// Ports, in manifest order.
	enum Port : uint8_t {
		kBrightness, kWind, kRain, kFrost, kOutside, kManual, kPresence, kSummer, kRoomTemperature, kEnable,
		kPosition, kSlat, kUpDown, kState, kSunAzimuth, kSunElevation,
		kPortCount
	};
	// Parameters, in manifest order.
	enum Param : uint8_t {
		kFacadeAzimuth, kOpeningAngle, kMinElevation, kOverhangAngle, kHorizonPoints,
		kHorizonAngle1, kHorizonElevation1, kHorizonAngle2, kHorizonElevation2, kHorizonAngle3, kHorizonElevation3,
		kHorizonAngle4, kHorizonElevation4,
		kBrightnessOn, kBrightnessOff, kBrightnessOnDelay, kBrightnessOffDelay,
		kShadePosition, kSlatMode, kShadeSlat, kSlatRatio, kSlatOpen, kSlatStep,
		kHeatLimit, kWindLimit, kWindTimeout, kFrostLimit, kAlarmRelease, kManualLock, kManualDuration,
		kMinMoveInterval,
		kParamCount
	};
	// State fields, in manifest order.
	enum Field : uint8_t {
		kLastState, kWindOn, kWindCalmAt, kRainOn, kRainDryAt, kFrostOn, kFrostClearAt, kBright, kBrightAt, kHeat,
		kManualAt, kManualSunny, kManualDay, kManualSeen, kHolding, kDue, kMovedAt, kSentPosition, kSentSlat, kSentUpDown,
		kFieldCount
	};
	/// The values of the output `state`.
	enum State : uint8_t { idle, shading, wind, rain, frost, manual, heat_protection, off };
	enum SlatMode : uint8_t { fixed, tracking };
	enum Lock : uint8_t { duration, sun_change, midnight };

	static const SunShading& instance();

	/// The horizon's elevation at `g` degrees from the facade's normal (spec/blocks.md 18.4).
	static double horizon(const double* params, double g);
	/// The slat position of `tracking` at the profile angle `profile` (spec/blocks.md 18.4).
	static double tracked_slat(const double* params, double profile);

	const Manifest& manifest() const override;
	const char* check(const double* params, size_t& parameter) const override;
	void start(BlockContext& cx, StartReason reason, bool restored) const override;
	void input(BlockContext& cx, size_t port) const override;
	void timer(BlockContext& cx, size_t timer) const override;
	void period(BlockContext& cx) const override;
	void parameter(BlockContext& cx, size_t index) const override;

private:
	void step(BlockContext& cx, bool operated) const;
	/// The largest update time of the datapoints of `manual` that are not unknown; -1 if none.
	static double latest_manual(const BlockContext& cx);
};

} // namespace hc::blocks

#endif // HC_BLOCKS_SUN_SHADING_H
