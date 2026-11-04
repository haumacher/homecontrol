/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The heating curve of spec/blocks.md 17. Every formula is evaluated in the order the
// specification writes it (spec/blocks.md 13): E-Home's Java port must give the same bits.
#include "hc/blocks/heating_curve.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace hc::blocks {

namespace {

constexpr uint8_t kMeanMin = aggregation_bit(Aggregation::mean) | aggregation_bit(Aggregation::min);

// id, direction, type, label, unit, required, multiple, aggregations, default aggregation, trigger,
// diagnostic, failsafe, has_min, min, has_max, max
constexpr PortDecl kPorts[] = {
	{"outside", Direction::in, PortType::real, "Outside temperature", "°C", true, 4, kMeanMin, Aggregation::mean,
		InputTrigger::none, false, 0, false, 0, false, 0},
	{"roomSetpoint", Direction::in, PortType::real, "Room setpoint", "°C", false, 1, 0, Aggregation::none,
		InputTrigger::change, false, 0, false, 0, false, 0},
	{"offset", Direction::in, PortType::real, "Flow offset", "K", false, 1, 0, Aggregation::none,
		InputTrigger::change, false, 0, false, 0, false, 0},
	{"enable", Direction::in, PortType::boolean, "Heating on", "", false, 1, 0, Aggregation::none,
		InputTrigger::change, false, 0, false, 0, false, 0},
	{"summerSwitch", Direction::in, PortType::boolean, "Summer switch", "", false, 1, 0, Aggregation::none,
		InputTrigger::change, false, 0, false, 0, false, 0},
	{"flowSetpoint", Direction::out, PortType::real, "Flow setpoint", "°C", false, 1, 0, Aggregation::none,
		InputTrigger::change, false, 40, false, 0, false, 0},
	{"summer", Direction::out, PortType::boolean, "Summer", "", false, 1, 0, Aggregation::none,
		InputTrigger::change, false, 0, false, 0, false, 0},
	{"outsideFiltered", Direction::out, PortType::real, "Filtered outside temperature", "°C", false, 1, 0,
		Aggregation::none, InputTrigger::change, true, 0, false, 0, false, 0},
	{"state", Direction::out, PortType::enumeration, "State", "", false, 1, 0, Aggregation::none,
		InputTrigger::change, true, 0, true, 0, true, 4},
};
static_assert(sizeof(kPorts) / sizeof(kPorts[0]) == HeatingCurve::kPortCount, "ports");

constexpr Choice kForms[] = {{0, "slope", "Slope and level"}, {1, "design", "Design point"}, {2, "points", "Points"}};

constexpr double kHour = 3600000;

#define HC_POINT(n, x, y) \
	{"outside" #n, ParamType::real, "Outside temperature " #n, "°C", x, true, -40, true, 40, 1, nullptr, 0, false}, \
	{"flow" #n, ParamType::real, "Flow " #n, "°C", y, true, 10, true, 90, 1, nullptr, 0, false}

// id, type, label, unit, default, has_min, min, has_max, max, step, choices, count, adjustable
constexpr ParamDecl kParams[] = {
	{"curve", ParamType::enumeration, "Curve", "", 0, false, 0, false, 0, 0, kForms, 3, false},
	{"slope", ParamType::real, "Slope", "", 1.2, true, 0.2, true, 4, 0.1, nullptr, 0, true},
	{"level", ParamType::real, "Level", "K", 0, true, -15, true, 15, 0.5, nullptr, 0, true},
	{"roomTemperature", ParamType::real, "Room temperature of the curve", "°C", 20, true, 10, true, 30, 0.5, nullptr,
		0, false},
	{"designOutside", ParamType::real, "Design outside temperature", "°C", -12, true, -30, true, 10, 1, nullptr, 0,
		false},
	{"designFlow", ParamType::real, "Design flow temperature", "°C", 55, true, 20, true, 90, 1, nullptr, 0, false},
	{"pointCount", ParamType::integer, "Points", "", 6, true, 2, true, 6, 1, nullptr, 0, false},
	HC_POINT(1, -15, 60),
	HC_POINT(2, -10, 55),
	HC_POINT(3, -5, 49),
	HC_POINT(4, 0, 43),
	HC_POINT(5, 10, 32),
	HC_POINT(6, 20, 20),
	{"minFlow", ParamType::real, "Lowest flow", "°C", 25, true, 10, true, 60, 1, nullptr, 0, false},
	{"maxFlow", ParamType::real, "Highest flow", "°C", 60, true, 20, true, 90, 1, nullptr, 0, false},
	{"minChange", ParamType::real, "Smallest change sent", "K", 0.5, true, 0, true, 5, 0.1, nullptr, 0, false},
	{"outsideFilter", ParamType::duration, "Outside filter time constant", "h", 12 * kHour, true, 0, true, 48 * kHour,
		kHour, nullptr, 0, false},
	{"summerOn", ParamType::real, "Summer above", "°C", 18, true, 5, true, 30, 0.5, nullptr, 0, false},
	{"summerOff", ParamType::real, "Winter below", "°C", 15, true, 5, true, 30, 0.5, nullptr, 0, false},
	{"summerDays", ParamType::integer, "Days to summer", "d", 3, true, 0, true, 14, 1, nullptr, 0, false},
	{"frostOutside", ParamType::real, "Frost protection below", "°C", 3, true, -20, true, 10, 0.5, nullptr, 0, false},
	{"offFlow", ParamType::real, "Flow when off", "°C", 0, true, 0, true, 40, 1, nullptr, 0, false},
	{"sensorTimeout", ParamType::duration, "Sensor timeout", "min", 3 * kHour, true, 10 * 60000.0, true, 24 * kHour,
		60000, nullptr, 0, false},
};
#undef HC_POINT
static_assert(sizeof(kParams) / sizeof(kParams[0]) == HeatingCurve::kParamCount, "parameters");

constexpr StateField kFields[] = {
	{"filtered", true, 0}, {"hasFiltered", true, 0}, {"aboveFor", true, 0}, {"derivedSummer", true, 0},
	{"lastStep", false, 0}, {"frostOn", false, 0}, {"hasSent", false, 0}, {"sentFlow", false, 0},
	{"sentSummer", false, 0},
};
static_assert(sizeof(kFields) / sizeof(kFields[0]) == HeatingCurve::kFieldCount, "state fields");

constexpr Manifest kManifest = {
	"org.homecontrol.heating-curve", 1, 0, "Heating curve",
	"Weather-compensated flow setpoint with the summer/winter switch and frost protection",
	kPorts, HeatingCurve::kPortCount, kParams, HeatingCurve::kParamCount, kFields, HeatingCurve::kFieldCount,
	nullptr, 0, 60000, 1800000};

constexpr size_t outside_of(size_t i) { return HeatingCurve::kOutside1 + 2 * i; }
constexpr size_t flow_of(size_t i) { return HeatingCurve::kFlow1 + 2 * i; }

} // namespace

const HeatingCurve& HeatingCurve::instance() {
	static const HeatingCurve type;
	return type;
}

const Manifest& HeatingCurve::manifest() const { return kManifest; }

double HeatingCurve::interpolate(const double* params, double x) {
	const size_t n = static_cast<size_t>(params[kPointCount]);
	if (x <= params[outside_of(0)]) {
		return params[flow_of(0)];
	}
	if (x >= params[outside_of(n - 1)]) {
		return params[flow_of(n - 1)];
	}
	for (size_t i = 0; i + 1 < n; ++i) {
		const double x0 = params[outside_of(i)];
		const double x1 = params[outside_of(i + 1)];
		if (x <= x1) {
			const double y0 = params[flow_of(i)];
			const double y1 = params[flow_of(i + 1)];
			return y0 + (y1 - y0) * ((x - x0) / (x1 - x0));
		}
	}
	return params[flow_of(n - 1)];
}

const char* HeatingCurve::check(const double* params, size_t& parameter) const {
	if (!(params[kMinFlow] < params[kMaxFlow])) {
		parameter = kMinFlow;
		return "minFlow must be below maxFlow";
	}
	if (!(params[kSummerOff] < params[kSummerOn])) {
		parameter = kSummerOff;
		return "summerOff must be below summerOn";
	}
	const Form form = static_cast<Form>(static_cast<int>(params[kCurve]));
	if (form == design) {
		if (!(params[kDesignOutside] < params[kRoomTemperature])) {
			parameter = kDesignOutside;
			return "designOutside must be below roomTemperature";
		}
		if (!(params[kDesignFlow] > params[kRoomTemperature])) {
			parameter = kDesignFlow;
			return "designFlow must be above roomTemperature";
		}
	} else if (form == points) {
		const size_t n = static_cast<size_t>(params[kPointCount]);
		for (size_t i = 0; i + 1 < n; ++i) {
			if (!(params[outside_of(i + 1)] > params[outside_of(i)])) {
				parameter = outside_of(i + 1);
				return "the points' outside temperatures must rise";
			}
		}
	}
	return nullptr;
}

void HeatingCurve::start(BlockContext& cx, StartReason reason, bool restored) const {
	(void)reason;
	(void)restored; // a fresh record holds the initial values already
	double* s = cx.state();
	s[kLastStep] = static_cast<double>(cx.now());
	s[kHasSent] = 0;
	step(cx);
}

void HeatingCurve::input(BlockContext& cx, size_t port) const {
	(void)port;
	step(cx);
}

void HeatingCurve::timer(BlockContext& cx, size_t timer) const {
	(void)timer;
	step(cx);
}

void HeatingCurve::period(BlockContext& cx) const { step(cx); }

void HeatingCurve::parameter(BlockContext& cx, size_t index) const {
	(void)index;
	step(cx);
}

void HeatingCurve::step(BlockContext& cx) const {
	double* s = cx.state();
	const uint64_t now = cx.now();
	const double t = static_cast<double>(now);
	const double dt = (t - s[kLastStep]) / 1000.0;
	s[kLastStep] = t;
	const double Tf = cx.param(kOutsideFilter) / 1000.0;

	const InputValue X = cx.read(kOutside);
	const bool fresh = X.quality != Quality::unknown && static_cast<double>(now - X.updated_ms) <= cx.param(kSensorTimeout);
	if (fresh && X.good()) {
		const double x = X.value;
		if (s[kHasFiltered] == 0 || Tf == 0) {
			s[kFiltered] = x;
			s[kHasFiltered] = 1;
		} else {
			s[kFiltered] = s[kFiltered] + (x - s[kFiltered]) * (dt / (Tf + dt));
		}
		const double summer_on = cx.param(kSummerOn);
		const double days = cx.param(kSummerDays) * 86400.0;
		if (s[kFiltered] > summer_on) {
			s[kAboveFor] = std::min(s[kAboveFor] + dt, days);
		} else {
			s[kAboveFor] = 0;
		}
		if (s[kDerivedSummer] == 0 && s[kFiltered] > summer_on && s[kAboveFor] >= days) {
			s[kDerivedSummer] = 1;
		} else if (s[kDerivedSummer] == 1 && s[kFiltered] < cx.param(kSummerOff)) {
			s[kDerivedSummer] = 0;
		}
	}
	const double filtered = s[kFiltered];
	const double fx = fresh ? X.value : filtered; // frost on the momentary value (D5)
	const double frost_outside = cx.param(kFrostOutside);
	if (fx < frost_outside) {
		s[kFrostOn] = 1;
	} else if (fx >= frost_outside + 1.0) {
		s[kFrostOn] = 0;
	}

	const InputValue S = cx.read(kSummerSwitch);
	const bool summer_now = S.good() ? S.value != 0 : s[kDerivedSummer] == 1;

	const double R = cx.param(kRoomTemperature);
	const InputValue RS = cx.read(kRoomSetpoint);
	const double Tr = RS.good() ? RS.value : R;
	const double level = cx.param(kLevel);
	double flow = 0;
	switch (static_cast<Form>(static_cast<int>(cx.param(kCurve)))) {
		case design: {
			const double sl = (cx.param(kDesignFlow) - R) / (R - cx.param(kDesignOutside));
			flow = Tr + level + sl * (Tr - filtered);
			break;
		}
		case points: {
			double params[kParamCount];
			for (size_t i = 0; i < kParamCount; ++i) {
				params[i] = cx.param(i);
			}
			const double d = Tr - R;
			flow = interpolate(params, filtered - d) + d + level;
			break;
		}
		default: flow = Tr + level + cx.param(kSlope) * (Tr - filtered); break;
	}
	const InputValue O = cx.read(kOffset);
	if (O.good()) {
		flow = flow + O.value;
	}
	const double min_flow = cx.param(kMinFlow);
	const double max_flow = cx.param(kMaxFlow);
	flow = std::min(std::max(flow, min_flow), max_flow);

	const InputValue enable = cx.read(kEnable);
	const bool is_off = enable.good() && enable.value == 0;
	const double off_flow = cx.param(kOffFlow);
	double out = flow;
	State state = heating;
	if (is_off || summer_now) {
		if (s[kFrostOn] == 1) {
			out = min_flow;
			state = frost;
		} else {
			out = off_flow;
			state = is_off ? off : summer;
		}
	}
	if (!fresh) {
		state = fault;
	}

	// Outputs (spec/blocks.md 17.4).
	const bool first = s[kHasSent] == 0;
	if (first || (out != s[kSentFlow] && (std::fabs(out - s[kSentFlow]) >= cx.param(kMinChange) || out == off_flow ||
										  out == min_flow || out == max_flow))) {
		cx.write(kFlowSetpoint, out);
		s[kSentFlow] = out;
	}
	const double summer_value = summer_now ? 1 : 0;
	if (first || summer_value != s[kSentSummer]) {
		cx.write(kSummer, summer_value);
	}
	s[kSentSummer] = summer_value;
	cx.write(kOutsideFiltered, filtered);
	cx.write(kState, static_cast<double>(state));
	s[kHasSent] = 1;
}

} // namespace hc::blocks
