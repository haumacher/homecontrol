/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_BLOCKS_HEATING_DEMAND_H
#define HC_BLOCKS_HEATING_DEMAND_H

#include "hc/blocks/block_type.h"

namespace hc::blocks {

/// The heating demand `org.homecontrol.heating-demand` 1.0 (spec/blocks.md 16): the rooms' valve
/// positions and heat requests into the heat source's request (hysteresis, minimum on and off
/// times), the demand level (the highest, or the mean of the highest), the circulation pump with
/// its run-on, and the flow offset for the heating curve.
class HeatingDemand : public BlockType {
public:
	// Ports, in manifest order.
	enum Port : uint8_t {
		kValves, kDemands, kEnable, kSummer, kRequest, kLevel, kPump, kFlowSetpointOffset, kState, kStaleInputs,
		kPortCount
	};
	// Parameters, in manifest order.
	enum Param : uint8_t {
		kMode, kTopCount, kOnThreshold, kOffThreshold, kMinOn, kMinOff, kPumpRunOn, kTargetLevel, kOffsetGain,
		kMaxOffset, kStaleInput, kParamCount
	};
	// State fields, in manifest order.
	enum Field : uint8_t {
		kOffset, kLastStep, kLastLevel, kHeat, kSwitchedAt, kHasSwitched, kHasSent, kSentRequest, kSentPump,
		kSentLevel, kSentOffset, kFieldCount
	};
	enum Timer : uint8_t { kWakeTimer, kTimerCount };
	/// The values of the output `state`.
	enum State : uint8_t { off, on, held_on, held_off, disabled, summer };
	enum Mode : uint8_t { max, top_mean };
	enum StaleInput : uint8_t { ignore, full };

	static const HeatingDemand& instance();

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

#endif // HC_BLOCKS_HEATING_DEMAND_H
