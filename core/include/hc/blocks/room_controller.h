/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_BLOCKS_ROOM_CONTROLLER_H
#define HC_BLOCKS_ROOM_CONTROLLER_H

#include "hc/blocks/block_type.h"

namespace hc::blocks {

/// The room controller `org.homecontrol.room-controller` 1.0 (spec/blocks.md 14): a time-based
/// PI(D) controller with the D term on the filtered actual value, conditional integration, bumpless
/// gain changes, continuous, PWM and two-point outputs, presets, window, frost, boost, the sensor
/// timeout, valve protection and summer.
class RoomController : public BlockType {
public:
	// Ports, in manifest order.
	enum Port : uint8_t {
		kTemperature, kSetpoint, kMode, kPresence, kHeatingPeriod, kWindow, kEnable, kSummer, kForced,
		kValve, kSwitch, kDemand, kActiveSetpoint, kState, kP, kI, kD, kError, kFiltered, kPortCount
	};
	// Parameters, in manifest order.
	enum Param : uint8_t {
		kSystem, kControlType, kXp, kTn, kTd, kFilter, kCycle, kHysteresis, kOutputMin, kOutputMax, kMinChange,
		kResendInterval, kComfort, kStandby, kNight, kFrost, kWindowDelay, kBoostThreshold, kBoostDuration,
		kSensorTimeout, kFallbackPosition, kForcedPosition, kSummerPosition, kProtectionDay, kProtectionTime,
		kProtectionDuration, kParamCount
	};
	// State fields, in manifest order.
	enum Field : uint8_t {
		kIntegral, kFilteredValue, kHasFiltered, kLastStep, kLastSetpoint, kHasLastSetpoint, kLastKp, kHasLastKp,
		kWindowActive, kBoostUntil, kProtectionUntil, kProtectedOn, kCycleStart, kHasCycle, kLastState, kSentValve, kHasSent,
		kSentAt, kSwitchOn, kDemandOn, kFieldCount
	};
	enum Timer : uint8_t { kWindowTimer, kPwmTimer, kTimerCount };
	/// The values of the output `state`.
	enum State : uint8_t { off, heating, window, frost, boost, fault, protection, summer, forced };
	enum System : uint8_t { radiator, underfloor, convector, custom };
	enum ControlType : uint8_t { continuous, pwm, two_point };

	/// The gains of a preset (spec/blocks.md 14.2).
	struct Gains {
		double xp, tn, td, filter;
	};
	static Gains preset(System system);

	static const RoomController& instance();

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

#endif // HC_BLOCKS_ROOM_CONTROLLER_H
