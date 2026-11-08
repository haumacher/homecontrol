/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The heating demand of spec/blocks.md 16. Every formula is evaluated in the order the
// specification writes it (spec/blocks.md 13): E-Home's Java port must give the same bits.
#include "hc/blocks/heating_demand.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace hc::blocks {

namespace {

// id, direction, type, label, unit, required, multiple, aggregations, default aggregation, trigger,
// diagnostic, failsafe, has_min, min, has_max, max
constexpr PortDecl kPorts[] = {
	{"valves", Direction::in, PortType::real, "Valve positions", "%", false, 32, aggregation_bit(Aggregation::max),
		Aggregation::max, InputTrigger::update, false, 0, false, 0, false, 0},
	{"demands", Direction::in, PortType::boolean, "Heat demands", "", false, 32, aggregation_bit(Aggregation::any),
		Aggregation::any, InputTrigger::update, false, 0, false, 0, false, 0},
	{"enable", Direction::in, PortType::boolean, "Heating on", "", false, 1, 0, Aggregation::none,
		InputTrigger::change, false, 0, false, 0, false, 0},
	{"summer", Direction::in, PortType::boolean, "Summer", "", false, 1, 0, Aggregation::none, InputTrigger::change,
		false, 0, false, 0, false, 0},
	{"request", Direction::out, PortType::boolean, "Heat request", "", false, 1, 0, Aggregation::none,
		InputTrigger::change, false, 1, false, 0, false, 0},
	{"level", Direction::out, PortType::real, "Demand level", "%", false, 1, 0, Aggregation::none,
		InputTrigger::change, false, 100, true, 0, true, 100},
	{"pump", Direction::out, PortType::boolean, "Circulation pump", "", false, 1, 0, Aggregation::none,
		InputTrigger::change, false, 1, false, 0, false, 0},
	{"flowSetpointOffset", Direction::out, PortType::real, "Flow setpoint offset", "K", false, 1, 0,
		Aggregation::none, InputTrigger::change, false, 0, true, 0, true, 20},
	{"state", Direction::out, PortType::enumeration, "State", "", false, 1, 0, Aggregation::none,
		InputTrigger::change, true, 0, true, 0, true, 5},
	{"staleInputs", Direction::out, PortType::integer, "Inputs not good", "", false, 1, 0, Aggregation::none,
		InputTrigger::change, true, 0, false, 0, false, 0},
};
static_assert(sizeof(kPorts) / sizeof(kPorts[0]) == HeatingDemand::kPortCount, "ports");

constexpr Choice kModes[] = {{0, "max", "Highest valve"}, {1, "topMean", "Mean of the highest"}};
constexpr Choice kStaleInputs[] = {{0, "ignore", "Ignore"}, {1, "full", "Count as fully open"}};

constexpr double kMinute = 60000;

// id, type, label, unit, default, has_min, min, has_max, max, step, choices, count, adjustable
constexpr ParamDecl kParams[] = {
	{"mode", ParamType::enumeration, "Demand mode", "", 0, false, 0, false, 0, 0, kModes, 2, false},
	{"topCount", ParamType::integer, "Valves averaged", "", 3, true, 1, true, 32, 1, nullptr, 0, false},
	{"onThreshold", ParamType::real, "Switch-on level", "%", 20, true, 0, true, 100, 1, nullptr, 0, false},
	{"offThreshold", ParamType::real, "Switch-off level", "%", 5, true, 0, true, 100, 1, nullptr, 0, false},
	{"minOn", ParamType::duration, "Minimum on time", "min", 5 * kMinute, true, 0, true, 60 * kMinute, kMinute,
		nullptr, 0, false},
	{"minOff", ParamType::duration, "Minimum off time", "min", 5 * kMinute, true, 0, true, 60 * kMinute, kMinute,
		nullptr, 0, false},
	{"pumpRunOn", ParamType::duration, "Pump run-on", "min", 5 * kMinute, true, 0, true, 60 * kMinute, kMinute,
		nullptr, 0, false},
	{"targetLevel", ParamType::real, "Target level", "%", 70, true, 10, true, 100, 1, nullptr, 0, false},
	{"offsetGain", ParamType::real, "Offset gain", "K/(% h)", 0.1, true, 0, true, 1, 0.01, nullptr, 0, false},
	{"maxOffset", ParamType::real, "Highest offset", "K", 5, true, 0, true, 20, 0.5, nullptr, 0, false},
	{"staleInput", ParamType::enumeration, "Input not good", "", 0, false, 0, false, 0, 0, kStaleInputs, 2, false},
};
static_assert(sizeof(kParams) / sizeof(kParams[0]) == HeatingDemand::kParamCount, "parameters");

constexpr StateField kFields[] = {
	{"offset", true, 0}, {"lastStep", false, 0}, {"lastLevel", false, 0}, {"heat", false, 0},
	{"switchedAt", false, 0}, {"hasSwitched", false, 0}, {"hasSent", false, 0}, {"sentRequest", false, 0},
	{"sentPump", false, 0}, {"sentLevel", false, 0}, {"sentOffset", false, 0},
};
static_assert(sizeof(kFields) / sizeof(kFields[0]) == HeatingDemand::kFieldCount, "state fields");

constexpr const char* kTimers[] = {"wake"};

constexpr Manifest kManifest = {
	"org.homecontrol.heating-demand", 1, 0, "Heating demand",
	"The rooms' valves and demands into the heat source's request, the demand level, the pump and a flow offset",
	kPorts, HeatingDemand::kPortCount, kParams, HeatingDemand::kParamCount, kFields, HeatingDemand::kFieldCount,
	kTimers, HeatingDemand::kTimerCount, 60000, 600000};

/// Keeps the `n` largest values seen, in descending order (spec/blocks.md 16.4).
struct Highest {
	double values[BlockLimits::kMultiple * 2];
	size_t size = 0;
	size_t limit = 1;

	void add(double v) {
		size_t i = size < limit ? size : limit;
		if (i == limit && !(v > values[limit - 1])) {
			return;
		}
		if (size < limit) {
			++size;
		}
		// Shift the smaller ones down by one; the last falls off when full.
		i = size - 1;
		while (i > 0 && values[i - 1] < v) {
			values[i] = values[i - 1];
			--i;
		}
		values[i] = v;
	}
};

} // namespace

const HeatingDemand& HeatingDemand::instance() {
	static const HeatingDemand type;
	return type;
}

const Manifest& HeatingDemand::manifest() const { return kManifest; }

const char* HeatingDemand::check(const double* params, size_t& parameter) const {
	if (!(params[kOffThreshold] < params[kOnThreshold])) {
		parameter = kOffThreshold;
		return "offThreshold must be below onThreshold";
	}
	return nullptr;
}

void HeatingDemand::start(BlockContext& cx, StartReason reason, bool restored) const {
	(void)reason;
	(void)restored; // a fresh record holds the initial values already
	double* s = cx.state();
	s[kLastStep] = static_cast<double>(cx.now());
	s[kHasSent] = 0;
	cx.cancel_timer(kWakeTimer);
	step(cx);
}

void HeatingDemand::input(BlockContext& cx, size_t port) const {
	(void)port;
	step(cx);
}

void HeatingDemand::timer(BlockContext& cx, size_t timer) const {
	(void)timer;
	step(cx);
}

void HeatingDemand::period(BlockContext& cx) const { step(cx); }

void HeatingDemand::parameter(BlockContext& cx, size_t index) const {
	(void)index;
	step(cx);
}

void HeatingDemand::step(BlockContext& cx) const {
	double* s = cx.state();
	const double t = static_cast<double>(cx.now());
	const double dt = (t - s[kLastStep]) / 1000.0;
	s[kLastStep] = t;

	// The offset over the interval just ended, with the level and request that held during it (D5).
	const double e = (s[kHeat] == 1 ? s[kLastLevel] : 0) - cx.param(kTargetLevel);
	const double max_offset = cx.param(kMaxOffset);
	s[kOffset] = s[kOffset] + cx.param(kOffsetGain) * e * (dt / 3600.0);
	s[kOffset] = std::min(std::max(s[kOffset], 0.0), max_offset);

	// The level over the valves and the demands, one by one (D1, D2, D3).
	const bool count_full = static_cast<int>(cx.param(kStaleInput)) == full;
	const bool by_max = static_cast<int>(cx.param(kMode)) == max;
	Highest highest;
	highest.limit = by_max ? 1 : static_cast<size_t>(cx.param(kTopCount));
	size_t stale = 0;
	for (size_t port : {static_cast<size_t>(kValves), static_cast<size_t>(kDemands)}) {
		const size_t n = cx.count(port);
		for (size_t k = 0; k < n; ++k) {
			const InputValue v = cx.read_at(port, k);
			if (v.good()) {
				highest.add(port == kValves ? std::min(std::max(v.value, 0.0), 100.0) : (v.value != 0 ? 100.0 : 0.0));
			} else {
				++stale;
				if (count_full) {
					highest.add(100);
				}
			}
		}
	}
	double level = 0;
	if (highest.size > 0) {
		if (by_max) {
			level = highest.values[0];
		} else {
			double sum = highest.values[0];
			for (size_t i = 1; i < highest.size; ++i) {
				sum = sum + highest.values[i];
			}
			level = sum / static_cast<double>(highest.size);
		}
	}

	const InputValue enable = cx.read(kEnable);
	const bool is_off = enable.good() && enable.value == 0;
	const bool is_summer = cx.read(kSummer).is_true();
	double want = 0;
	if (is_off || is_summer) {
		want = 0;
	} else if (level >= cx.param(kOnThreshold)) {
		want = 1;
	} else if (level <= cx.param(kOffThreshold)) {
		want = 0;
	} else {
		want = s[kHeat];
	}

	// The minimum times (D4).
	bool held = false;
	double wake_at = 0;
	bool wake = false;
	if (want != s[kHeat]) {
		const double hold = s[kHeat] == 1 ? cx.param(kMinOn) : cx.param(kMinOff);
		if (s[kHasSwitched] == 0 || t - s[kSwitchedAt] >= hold) {
			s[kHeat] = want;
			s[kSwitchedAt] = t;
			s[kHasSwitched] = 1;
		} else {
			held = true;
			wake = true;
			wake_at = s[kSwitchedAt] + hold;
		}
	}

	const double run_on = cx.param(kPumpRunOn);
	const bool pump = s[kHeat] == 1 || (s[kHasSwitched] == 1 && t - s[kSwitchedAt] < run_on);
	if (s[kHeat] == 0 && pump) {
		const double end = s[kSwitchedAt] + run_on;
		wake_at = wake ? std::min(wake_at, end) : end;
		wake = true;
	}
	if (wake) {
		cx.set_timer(kWakeTimer, static_cast<int64_t>(wake_at - t));
	} else {
		cx.cancel_timer(kWakeTimer);
	}
	s[kLastLevel] = level;

	const State state = held ? (s[kHeat] == 1 ? held_on : held_off)
		: is_off ? disabled : is_summer ? summer : (s[kHeat] == 1 ? on : off);

	// Outputs (spec/blocks.md 16.4).
	const bool first = s[kHasSent] == 0;
	if (first || s[kHeat] != s[kSentRequest]) {
		cx.write(kRequest, s[kHeat]);
	}
	s[kSentRequest] = s[kHeat];
	if (first || level != s[kSentLevel]) {
		cx.write(kLevel, level);
	}
	s[kSentLevel] = level;
	const double pump_on = pump ? 1 : 0;
	if (first || pump_on != s[kSentPump]) {
		cx.write(kPump, pump_on);
	}
	s[kSentPump] = pump_on;
	const double offset = s[kOffset];
	if (first || (offset != s[kSentOffset] &&
					 (std::fabs(offset - s[kSentOffset]) >= 0.1 || offset == 0 || offset == max_offset))) {
		cx.write(kFlowSetpointOffset, offset);
		s[kSentOffset] = offset;
	}
	cx.write(kState, static_cast<double>(state));
	cx.write(kStaleInputs, static_cast<double>(stale));
	s[kHasSent] = 1;
}

} // namespace hc::blocks
