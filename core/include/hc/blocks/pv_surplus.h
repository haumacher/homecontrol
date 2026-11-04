/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_BLOCKS_PV_SURPLUS_H
#define HC_BLOCKS_PV_SURPLUS_H

#include "hc/blocks/block_type.h"

namespace hc::blocks {

/// The PV surplus control `org.homecontrol.pv-surplus` 1.0 (spec/blocks.md 20): up to two
/// switched or modulated loads in the order of their priority, run from the photovoltaic system's
/// surplus with a hysteresis, minimum times and a daily maximum, forced by a deadline, behind a home
/// battery up to a state of charge; a chain of instances for more loads.
class PvSurplus : public BlockType {
public:
	static constexpr size_t kLoads = 2;

	// Ports, in manifest order: the inputs, then per load its four outputs, then loadPower, surplus.
	enum Port : uint8_t {
		kGridPower, kPvPower, kHousePower, kBatteryPower, kBatterySoc, kLowerPower, kEnable, kReady1,
		kSwitch1 = kReady1 + kLoads, // per load k (0-based): kSwitch1 + 4k .. kEnergy1 + 4k
		kSetpoint1, kState1, kEnergy1,
		kLoadPower = kSwitch1 + 4 * kLoads, kSurplus,
		kPortCount
	};
	static constexpr size_t kPortsPerLoad = 4;
	// Parameters, in manifest order: the common ones, then per load its eight.
	enum Param : uint8_t {
		kGridSign, kBatterySign, kSurplusFilter, kOnMargin, kAllowedImport, kBatteryPriority, kMeterTimeout,
		kSetpointUnit,
		kPower1, kMinPower1, kStep1, kMinOn1, kMinOff1, kMaxPerDay1, kMustRunBy1, kMustRunFor1,
		kParamCount = kPower1 + 8 * kLoads
	};
	static constexpr size_t kParamsPerLoad = 8;
	// State fields, in manifest order: the common ones, then per load its four.
	enum Field : uint8_t {
		kLastStep, kFiltered, kHasFiltered, kDay, kDayStart, kHasSent, kSentLoadPower,
		kLevel1, kSwitchedAt1, kRun1, kEnergyField1,
		kFieldCount = kLevel1 + 4 * kLoads
	};
	static constexpr size_t kFieldsPerLoad = 4;
	/// The values of `state<k>`.
	enum LoadState : uint8_t { off, waiting, running, forced, blocked };

	static const PvSurplus& instance();

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

#endif // HC_BLOCKS_PV_SURPLUS_H
