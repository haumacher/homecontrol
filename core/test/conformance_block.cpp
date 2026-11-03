/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "conformance_block.h"

namespace hc_test {

using namespace hc::blocks;

namespace {

constexpr uint8_t kNumeric = aggregation_bit(Aggregation::mean) | aggregation_bit(Aggregation::min) |
	aggregation_bit(Aggregation::max);

constexpr PortDecl kPorts[] = {
	{"x", Direction::in, PortType::real, "x", "", true, 4, kNumeric, Aggregation::mean, InputTrigger::change, false, 0,
		false, 0, false, 0},
	{"arm", Direction::in, PortType::boolean, "arm", "", false, 1, 0, Aggregation::none, InputTrigger::change, false,
		0, false, 0, false, 0},
	{"tick", Direction::in, PortType::boolean, "tick", "", false, 1, 0, Aggregation::none, InputTrigger::update,
		false, 0, false, 0, false, 0},
	{"y", Direction::out, PortType::real, "y", "", false, 1, 0, Aggregation::none, InputTrigger::change, false, -1,
		false, 0, false, 0},
	{"pulse", Direction::out, PortType::boolean, "pulse", "", false, 1, 0, Aggregation::none, InputTrigger::change,
		false, 0, false, 0, false, 0},
	{"calls", Direction::out, PortType::integer, "calls", "", false, 1, 0, Aggregation::none, InputTrigger::change,
		true, 0, false, 0, false, 0},
	{"ticks", Direction::out, PortType::integer, "ticks", "", false, 1, 0, Aggregation::none, InputTrigger::change,
		true, 0, false, 0, false, 0},
};

constexpr ParamDecl kParams1[] = {
	{"gain", ParamType::real, "gain", "", 1, true, 0, true, 10, 0, nullptr, 0, true},
	{"offset", ParamType::real, "offset", "", 0, true, -100, true, 100, 0, nullptr, 0, false},
	{"faultAbove", ParamType::real, "faultAbove", "", 1000, false, 0, false, 0, 0, nullptr, 0, false},
	{"delay", ParamType::duration, "delay", "ms", 5000, true, 100, true, 60000, 0, nullptr, 0, true},
};

constexpr ParamDecl kParams2[] = {
	{"gain", ParamType::real, "gain", "", 1, true, 0, true, 5, 0, nullptr, 0, true},
	{"offset", ParamType::real, "offset", "", 0, true, -100, true, 100, 0, nullptr, 0, false},
	{"faultAbove", ParamType::real, "faultAbove", "", 1000, false, 0, false, 0, 0, nullptr, 0, false},
	{"level", ParamType::integer, "level", "", 0, true, 0, true, 9, 0, nullptr, 0, true},
};

constexpr StateField kFields[] = {{"count", true, 0}, {"ticks", true, 0}, {"pulseOn", false, 0}};
constexpr const char* kTimers[] = {"t"};

constexpr Manifest kManifest1 = {"org.homecontrol.conformance", 1, 1, "Conformance", "spec/blocks.md 15", kPorts, 7,
	kParams1, 4, kFields, 3, kTimers, 1, 10000, 60000};
constexpr Manifest kManifest2 = {"org.homecontrol.conformance", 2, 0, "Conformance", "spec/blocks.md 15", kPorts, 7,
	kParams2, 4, kFields, 3, kTimers, 1, 10000, 60000};

} // namespace

const ConformanceBlock& ConformanceBlock::v1() {
	static const ConformanceBlock type(kManifest1, false);
	return type;
}

const ConformanceBlock& ConformanceBlock::v2() {
	static const ConformanceBlock type(kManifest2, true);
	return type;
}

void ConformanceBlock::update(BlockContext& cx) const {
	InputValue x = cx.read(kX);
	if (x.good()) {
		if (x.value > cx.param(kFaultAbove)) {
			cx.fault("x above faultAbove");
		} else {
			cx.write(kY, cx.param(kGain) * x.value + cx.param(kOffset));
		}
	}
	cx.write(kCalls, cx.state()[kCount]);
}

void ConformanceBlock::start(BlockContext& cx, StartReason reason, bool restored) const {
	(void)reason;
	(void)restored;
	cx.state()[kCount] += 1;
	update(cx);
}

void ConformanceBlock::input(BlockContext& cx, size_t port) const {
	cx.state()[kCount] += 1;
	if (port == kArm) {
		if (cx.read(kArm).is_true()) {
			cx.set_timer(0, second_ ? 5000 : static_cast<int64_t>(cx.param(kFourth)));
		} else {
			cx.cancel_timer(0);
		}
	}
	update(cx);
}

void ConformanceBlock::timer(BlockContext& cx, size_t timer) const {
	(void)timer;
	double* s = cx.state();
	s[kCount] += 1;
	s[kPulseOn] = 1 - s[kPulseOn];
	cx.write(kPulse, s[kPulseOn]);
	cx.write(kCalls, s[kCount]);
}

void ConformanceBlock::period(BlockContext& cx) const {
	double* s = cx.state();
	s[kCount] += 1;
	s[kTicksField] += 1;
	cx.write(kTicks, s[kTicksField]);
	cx.write(kCalls, s[kCount]);
}

void ConformanceBlock::parameter(BlockContext& cx, size_t index) const {
	(void)index;
	cx.state()[kCount] += 1;
	update(cx);
}

BlockRegistry& corpus_registry() {
	static BlockRegistry registry = [] {
		BlockRegistry r(true);
		r.add(ConformanceBlock::v1());
		r.add(ConformanceBlock::v2());
		return r;
	}();
	return registry;
}

} // namespace hc_test
