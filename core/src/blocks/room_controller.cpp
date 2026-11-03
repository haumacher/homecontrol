/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The room controller of spec/blocks.md 14. Every formula is evaluated in the order the
// specification writes it (spec/blocks.md 13): E-Home's Java port must give the same bits.
#include "hc/blocks/room_controller.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "hc/tz.h"

namespace hc::blocks {

namespace {

constexpr uint8_t kNumeric = aggregation_bit(Aggregation::mean) | aggregation_bit(Aggregation::min) |
	aggregation_bit(Aggregation::max);
constexpr uint8_t kAny = aggregation_bit(Aggregation::any);

// id, direction, type, label, unit, required, multiple, aggregations, default aggregation, trigger,
// diagnostic, failsafe, has_min, min, has_max, max
constexpr PortDecl kPorts[] = {
	{"temperature", Direction::in, PortType::real, "Temperature", "°C", true, 4, kNumeric, Aggregation::mean,
		InputTrigger::none, false, 0, false, 0, false, 0},
	{"setpoint", Direction::in, PortType::real, "External setpoint", "°C", false, 1, 0, Aggregation::none,
		InputTrigger::change, false, 0, false, 0, false, 0},
	{"mode", Direction::in, PortType::enumeration, "HVAC mode", "", false, 1, 0, Aggregation::none,
		InputTrigger::change, false, 0, false, 0, false, 0},
	{"presence", Direction::in, PortType::boolean, "Presence", "", false, 1, 0, Aggregation::none,
		InputTrigger::change, false, 0, false, 0, false, 0},
	{"heatingPeriod", Direction::in, PortType::boolean, "Heating period", "", false, 1, 0, Aggregation::none,
		InputTrigger::change, false, 0, false, 0, false, 0},
	{"window", Direction::in, PortType::boolean, "Window open", "", false, 4, kAny, Aggregation::any,
		InputTrigger::change, false, 0, false, 0, false, 0},
	{"enable", Direction::in, PortType::boolean, "Heating on", "", false, 1, 0, Aggregation::none,
		InputTrigger::change, false, 0, false, 0, false, 0},
	{"summer", Direction::in, PortType::boolean, "Summer", "", false, 1, 0, Aggregation::none, InputTrigger::change,
		false, 0, false, 0, false, 0},
	{"forced", Direction::in, PortType::boolean, "Forced position", "", false, 1, 0, Aggregation::none,
		InputTrigger::change, false, 0, false, 0, false, 0},
	{"valve", Direction::out, PortType::real, "Valve", "%", false, 1, 0, Aggregation::none, InputTrigger::change,
		false, 30, true, 0, true, 100},
	{"switch", Direction::out, PortType::boolean, "Switch", "", false, 1, 0, Aggregation::none,
		InputTrigger::change, false, 0, false, 0, false, 0},
	{"demand", Direction::out, PortType::boolean, "Heat demand", "", false, 1, 0, Aggregation::none,
		InputTrigger::change, false, 0, false, 0, false, 0},
	{"activeSetpoint", Direction::out, PortType::real, "Active setpoint", "°C", false, 1, 0, Aggregation::none,
		InputTrigger::change, true, 0, false, 0, false, 0},
	{"state", Direction::out, PortType::enumeration, "State", "", false, 1, 0, Aggregation::none,
		InputTrigger::change, true, 0, true, 0, true, 8},
	{"p", Direction::out, PortType::real, "P term", "%", false, 1, 0, Aggregation::none, InputTrigger::change, true,
		0, false, 0, false, 0},
	{"i", Direction::out, PortType::real, "I term", "%", false, 1, 0, Aggregation::none, InputTrigger::change, true,
		0, false, 0, false, 0},
	{"d", Direction::out, PortType::real, "D term", "%", false, 1, 0, Aggregation::none, InputTrigger::change, true,
		0, false, 0, false, 0},
	{"error", Direction::out, PortType::real, "Control error", "K", false, 1, 0, Aggregation::none,
		InputTrigger::change, true, 0, false, 0, false, 0},
	{"filtered", Direction::out, PortType::real, "Filtered temperature", "°C", false, 1, 0, Aggregation::none,
		InputTrigger::change, true, 0, false, 0, false, 0},
};
static_assert(sizeof(kPorts) / sizeof(kPorts[0]) == RoomController::kPortCount, "ports");

constexpr Choice kSystems[] = {
	{0, "radiator", "Radiator"}, {1, "underfloor", "Underfloor heating"}, {2, "convector", "Convector"},
	{3, "custom", "Custom"}};
constexpr Choice kControlTypes[] = {{0, "continuous", "Continuous"}, {1, "pwm", "PWM"}, {2, "twoPoint", "Two-point"}};

constexpr double kMinute = 60000;

// id, type, label, unit, default, has_min, min, has_max, max, step, choices, count, adjustable
constexpr ParamDecl kParams[] = {
	{"system", ParamType::enumeration, "Heating system", "", 0, false, 0, false, 0, 0, kSystems, 4, false},
	{"controlType", ParamType::enumeration, "Control", "", 0, false, 0, false, 0, 0, kControlTypes, 3, false},
	{"xp", ParamType::real, "Proportional band", "K", 3, true, 0.5, true, 20, 0.1, nullptr, 0, false},
	{"tn", ParamType::real, "Reset time", "min", 90, true, 1, true, 600, 1, nullptr, 0, false},
	{"td", ParamType::real, "Derivative time", "min", 0, true, 0, true, 60, 1, nullptr, 0, false},
	{"filter", ParamType::real, "Filter time constant", "min", 2, true, 0, true, 60, 0.5, nullptr, 0, false},
	{"cycle", ParamType::duration, "PWM cycle", "min", 15 * kMinute, true, 5 * kMinute, true, 60 * kMinute, kMinute,
		nullptr, 0, false},
	{"hysteresis", ParamType::real, "Hysteresis", "K", 0.5, true, 0.1, true, 5, 0.1, nullptr, 0, false},
	{"outputMin", ParamType::real, "Lowest valve position", "%", 0, true, 0, true, 100, 1, nullptr, 0, false},
	{"outputMax", ParamType::real, "Highest valve position", "%", 100, true, 0, true, 100, 1, nullptr, 0, false},
	{"minChange", ParamType::real, "Smallest change sent", "%", 2, true, 0, true, 50, 1, nullptr, 0, false},
	{"resendInterval", ParamType::duration, "Resend interval", "min", 0, true, 0, true, 1440 * kMinute, kMinute,
		nullptr, 0, false},
	{"comfort", ParamType::real, "Comfort", "°C", 21, true, 5, true, 30, 0.5, nullptr, 0, true},
	{"standby", ParamType::real, "Standby", "°C", 19, true, 5, true, 30, 0.5, nullptr, 0, true},
	{"night", ParamType::real, "Night", "°C", 17, true, 5, true, 30, 0.5, nullptr, 0, true},
	{"frost", ParamType::real, "Frost protection", "°C", 7, true, 4, true, 15, 0.5, nullptr, 0, true},
	{"windowDelay", ParamType::duration, "Window delay", "s", kMinute, true, 0, true, 60 * kMinute, 1000, nullptr,
		0, false},
	{"boostThreshold", ParamType::real, "Boost threshold", "K", 0, true, 0, true, 5, 0.1, nullptr, 0, false},
	{"boostDuration", ParamType::duration, "Longest boost", "min", 30 * kMinute, true, kMinute, true, 180 * kMinute,
		kMinute, nullptr, 0, false},
	{"sensorTimeout", ParamType::duration, "Sensor timeout", "min", 30 * kMinute, true, 0, true, 1440 * kMinute,
		kMinute, nullptr, 0, false},
	{"fallbackPosition", ParamType::real, "Valve without sensor", "%", 30, true, 0, true, 100, 1, nullptr, 0, false},
	{"forcedPosition", ParamType::real, "Forced valve position", "%", 100, true, 0, true, 100, 1, nullptr, 0, false},
	{"summerPosition", ParamType::real, "Valve in summer", "%", 0, true, 0, true, 100, 1, nullptr, 0, false},
	{"protectionDay", ParamType::integer, "Valve protection day", "", 0, true, 0, true, 7, 1, nullptr, 0, false},
	{"protectionTime", ParamType::time_of_day, "Valve protection from", "", 36000, true, 0, true, 86399, 60, nullptr,
		0, false},
	{"protectionDuration", ParamType::duration, "Valve protection duration", "min", 5 * kMinute, true, kMinute,
		true, 30 * kMinute, kMinute, nullptr, 0, false},
};
static_assert(sizeof(kParams) / sizeof(kParams[0]) == RoomController::kParamCount, "parameters");

constexpr StateField kFields[] = {
	{"integral", true, 0}, {"filtered", false, 0}, {"hasFiltered", false, 0}, {"lastStep", false, 0},
	{"lastSetpoint", false, 0}, {"hasLastSetpoint", false, 0}, {"lastKp", false, 0}, {"hasLastKp", false, 0},
	{"windowActive", false, 0}, {"boostUntil", false, 0}, {"protectionUntil", false, 0}, {"protectedOn", true, 0},
	{"cycleStart", false, 0}, {"hasCycle", false, 0}, {"lastState", false, 0}, {"sentValve", false, 0},
	{"hasSent", false, 0}, {"sentAt", false, 0}, {"switchOn", false, 0}, {"demandOn", false, 0},
};
static_assert(sizeof(kFields) / sizeof(kFields[0]) == RoomController::kFieldCount, "state fields");

constexpr const char* kTimers[] = {"window", "pwm"};

constexpr Manifest kManifest = {
	"org.homecontrol.room-controller", 1, 0, "Room controller",
	"PI(D) room temperature controller for a valve, a PWM-switched or a two-point heating",
	kPorts, RoomController::kPortCount, kParams, RoomController::kParamCount, kFields, RoomController::kFieldCount,
	kTimers, RoomController::kTimerCount, 60000, 600000};

} // namespace

RoomController::Gains RoomController::preset(System system) {
	switch (system) {
		case underfloor: return Gains{3, 180, 10, 2};
		case convector: return Gains{2, 45, 5, 2};
		default: return Gains{3, 90, 5, 2};
	}
}

const RoomController& RoomController::instance() {
	static const RoomController type;
	return type;
}

const Manifest& RoomController::manifest() const { return kManifest; }

const char* RoomController::check(const double* params, size_t& parameter) const {
	if (!(params[kOutputMin] < params[kOutputMax])) {
		parameter = kOutputMin;
		return "outputMin must be below outputMax";
	}
	return nullptr;
}

void RoomController::start(BlockContext& cx, StartReason reason, bool restored) const {
	(void)restored; // a fresh record holds the initial values already
	double* s = cx.state();
	s[kLastStep] = static_cast<double>(cx.now());
	s[kHasLastSetpoint] = 0;
	s[kHasCycle] = 0;
	s[kHasSent] = 0;
	s[kBoostUntil] = 0;
	if (reason != StartReason::image) {
		s[kHasFiltered] = 0;
	}
	s[kWindowActive] = cx.read(kWindow).is_true() ? 1 : 0;
	cx.cancel_timer(kWindowTimer);
	step(cx);
}

void RoomController::input(BlockContext& cx, size_t port) const {
	if (port == kWindow) {
		double* s = cx.state();
		if (cx.read(kWindow).is_true()) {
			if (s[kWindowActive] == 0) {
				double delay = cx.param(kWindowDelay);
				if (delay == 0) {
					s[kWindowActive] = 1;
				} else if (!cx.timer_running(kWindowTimer)) {
					cx.set_timer(kWindowTimer, static_cast<int64_t>(delay));
				}
			}
		} else {
			s[kWindowActive] = 0;
			cx.cancel_timer(kWindowTimer);
		}
	}
	step(cx);
}

void RoomController::timer(BlockContext& cx, size_t timer) const {
	double* s = cx.state();
	if (timer == kPwmTimer) {
		s[kSwitchOn] = 0;
		cx.write(kSwitch, 0);
		return;
	}
	if (cx.read(kWindow).is_true()) {
		s[kWindowActive] = 1;
	}
	step(cx);
}

void RoomController::period(BlockContext& cx) const { step(cx); }

void RoomController::parameter(BlockContext& cx, size_t index) const {
	(void)index;
	step(cx);
}

void RoomController::step(BlockContext& cx) const {
	double* s = cx.state();
	const uint64_t now = cx.now();
	const double t = static_cast<double>(now);
	const double dt = (t - s[kLastStep]) / 1000.0;
	s[kLastStep] = t;

	System system = static_cast<System>(static_cast<int>(cx.param(kSystem)));
	Gains g = system == custom ? Gains{cx.param(kXp), cx.param(kTn), cx.param(kTd), cx.param(kFilter)} : preset(system);
	const double Kp = 100.0 / g.xp;
	const double Ti = g.tn * 60.0;
	const double Td = g.td * 60.0;
	const double Tf = g.filter * 60.0;

	const InputValue T = cx.read(kTemperature);
	const double timeout = cx.param(kSensorTimeout);
	const bool ok = T.good() && (timeout == 0 || static_cast<double>(now - T.updated_ms) <= timeout);
	double x = 0;
	if (ok) {
		x = T.value;
		if (s[kHasFiltered] == 0 || Tf == 0) {
			s[kFilteredValue] = x;
			s[kHasFiltered] = 1;
		} else {
			s[kFilteredValue] = s[kFilteredValue] + (x - s[kFilteredValue]) * (dt / (Tf + dt));
		}
	}
	const double f = s[kFilteredValue];

	// The setpoint and the regulating state (spec/blocks.md 14.5).
	const InputValue mode = cx.read(kMode);
	const InputValue enable = cx.read(kEnable);
	const InputValue external = cx.read(kSetpoint);
	double sp = 0;
	State reg_state = heating;
	if (s[kWindowActive] == 1) {
		sp = cx.param(kFrost);
		reg_state = window;
	} else if (enable.good() && enable.value == 0) {
		sp = cx.param(kFrost);
		reg_state = off;
	} else if (mode.good() && mode.value == 4) {
		sp = cx.param(kFrost);
		reg_state = frost;
	} else if (external.good()) {
		sp = external.value;
	} else if (mode.good() && (mode.value == 1 || mode.value == 2 || mode.value == 3)) {
		sp = cx.param(mode.value == 1 ? kComfort : (mode.value == 2 ? kStandby : kNight));
	} else {
		const InputValue period = cx.read(kHeatingPeriod);
		const InputValue presence = cx.read(kPresence);
		if (period.good() && period.value == 0) {
			sp = cx.param(kNight);
		} else if (presence.good() && presence.value == 0) {
			sp = cx.param(kStandby);
		} else {
			sp = cx.param(kComfort);
		}
	}

	// Valve protection (the wall clock).
	DateTime w;
	const double protection_day = cx.param(kProtectionDay);
	if (protection_day > 0 && cx.wall(w) && static_cast<double>(w.day_of_week) == protection_day &&
		static_cast<double>(w.hour * 3600 + w.minute * 60 + w.second) >= cx.param(kProtectionTime)) {
		const double day = static_cast<double>(days_from_civil(w.year, w.month, w.day));
		if (day != s[kProtectedOn]) {
			s[kProtectionUntil] = t + cx.param(kProtectionDuration);
			s[kProtectedOn] = day;
		}
	}

	const ControlType control = static_cast<ControlType>(static_cast<int>(cx.param(kControlType)));
	const double out_min = cx.param(kOutputMin);
	const double out_max = cx.param(kOutputMax);
	double P = 0;
	double D = 0;
	double u = 0;
	State state = heating;
	bool regulating = false;
	bool two_point_on = false;
	if (cx.read(kForced).is_true()) {
		state = forced;
		u = cx.param(kForcedPosition);
		s[kBoostUntil] = 0;
	} else if (t < s[kProtectionUntil]) {
		state = protection;
		u = 100;
		s[kBoostUntil] = 0;
	} else if (cx.read(kSummer).is_true()) {
		state = summer;
		u = cx.param(kSummerPosition);
		s[kBoostUntil] = 0;
	} else if (!ok) {
		state = fault;
		u = cx.param(kFallbackPosition);
		s[kBoostUntil] = 0;
	} else {
		state = reg_state;
		regulating = true;
		const double e = sp - f;
		if (control == two_point) {
			const double h = cx.param(kHysteresis) / 2.0;
			if (f <= sp - h) {
				two_point_on = true;
			} else if (f >= sp + h) {
				two_point_on = false;
			} else {
				two_point_on = s[kSwitchOn] != 0;
			}
			u = two_point_on ? out_max : out_min;
			s[kBoostUntil] = 0;
		} else {
			const double threshold = cx.param(kBoostThreshold);
			if (threshold > 0 && s[kHasLastSetpoint] == 1 && sp > s[kLastSetpoint] && e >= threshold) {
				s[kBoostUntil] = t + cx.param(kBoostDuration);
			}
			if (s[kBoostUntil] > t && e >= threshold / 2.0) {
				state = boost;
				u = out_max;
			} else {
				s[kBoostUntil] = 0;
				if (s[kHasLastKp] == 1 && Kp != s[kLastKp]) {
					s[kIntegral] = s[kIntegral] + (s[kLastKp] - Kp) * e; // bumpless
				}
				s[kLastKp] = Kp;
				s[kHasLastKp] = 1;
				P = Kp * e;
				if (Td > 0 && Tf > 0) {
					D = -(Kp * Td * (x - f) / Tf); // on the filtered value: no kick on a setpoint step
				}
				const double v = P + s[kIntegral] + D;
				if (!((v >= out_max && e > 0) || (v <= out_min && e < 0))) {
					s[kIntegral] = s[kIntegral] + Kp * (dt / Ti) * e; // conditional integration
				}
				s[kIntegral] = std::min(std::max(s[kIntegral], out_min), out_max);
				u = std::min(std::max(P + s[kIntegral] + D, out_min), out_max);
			}
		}
	}
	s[kLastSetpoint] = sp;
	s[kHasLastSetpoint] = 1;
	const double valve = std::floor(u + 0.5);

	// Outputs (spec/blocks.md 14.4).
	const bool first = s[kHasSent] == 0;
	bool send = first;
	if (!send && valve != s[kSentValve] &&
		(std::fabs(valve - s[kSentValve]) >= cx.param(kMinChange) || valve == out_min || valve == out_max ||
			valve == 0 || valve == 100)) {
		send = true;
	}
	const double resend = cx.param(kResendInterval);
	if (!send && resend > 0 && t - s[kSentAt] >= resend) {
		send = true;
	}
	if (send) {
		cx.write(kValve, valve);
		s[kSentValve] = valve;
		s[kSentAt] = t;
		s[kHasSent] = 1;
	}

	bool on = false;
	if (control == pwm) {
		const double cycle = cx.param(kCycle);
		if (s[kHasCycle] == 0 || t - s[kCycleStart] >= cycle || static_cast<double>(state) != s[kLastState]) {
			s[kCycleStart] = t;
			s[kHasCycle] = 1;
			const double on_time = (cycle * valve) / 100.0;
			on = on_time > 0;
			if (on && on_time < cycle) {
				cx.set_timer(kPwmTimer, static_cast<int64_t>(std::floor(on_time)));
			} else {
				cx.cancel_timer(kPwmTimer);
			}
		} else {
			on = s[kSwitchOn] != 0;
		}
	} else if (control == two_point && regulating) {
		on = two_point_on;
	} else {
		on = valve > 0;
	}
	if (first || on != (s[kSwitchOn] != 0)) {
		cx.write(kSwitch, on ? 1 : 0);
	}
	s[kSwitchOn] = on ? 1 : 0;

	const bool demand = valve > 0 && state != forced && state != protection && state != summer;
	if (first || demand != (s[kDemandOn] != 0)) {
		cx.write(kDemand, demand ? 1 : 0);
	}
	s[kDemandOn] = demand ? 1 : 0;

	cx.write(kActiveSetpoint, sp);
	cx.write(kState, static_cast<double>(state));
	cx.write(kP, P);
	cx.write(kI, s[kIntegral]);
	cx.write(kD, D);
	cx.write(kError, s[kHasFiltered] != 0 ? sp - f : 0);
	cx.write(kFiltered, f);
	s[kLastState] = static_cast<double>(state);
}

} // namespace hc::blocks
