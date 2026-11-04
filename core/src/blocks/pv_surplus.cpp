/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The PV surplus control of spec/blocks.md 20. Every formula is evaluated in the order the
// specification writes it (spec/blocks.md 13): E-Home's Java port must give the same bits.
#include "hc/blocks/pv_surplus.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "hc/tz.h"

namespace hc::blocks {

namespace {

constexpr size_t kLoads = PvSurplus::kLoads;

/// An input: a real (unit W or %) or a boolean, one datapoint, read at the period (trigger none, D4).
constexpr PortDecl in(const char* id, PortType type, const char* label, const char* unit) {
	return PortDecl{id, Direction::in, type, label, unit, false, 1, 0, Aggregation::none, InputTrigger::none, false, 0,
		false, 0, false, 0};
}

/// An output: its type, failsafe (none when diagnostic) and bounds.
constexpr PortDecl out(const char* id, PortType type, const char* label, const char* unit, bool diagnostic,
	double failsafe, bool has_min, bool has_max, double max) {
	return PortDecl{id, Direction::out, type, label, unit, false, 1, 0, Aggregation::none, InputTrigger::change,
		diagnostic, failsafe, has_min, 0, has_max, max};
}

/// The four outputs of a load.
#define HC_PV_LOAD_OUTPUTS(k) \
	out("switch" #k, PortType::boolean, "Load " #k " on", "", false, 0, false, false, 0), \
	out("setpoint" #k, PortType::real, "Load " #k " setpoint", "W or %", false, 0, true, false, 0), \
	out("state" #k, PortType::enumeration, "Load " #k " state", "", true, 0, true, true, 4), \
	out("energy" #k, PortType::real, "Load " #k " energy today", "Wh", true, 0, false, false, 0)

constexpr PortDecl kPorts[] = {
	in("gridPower", PortType::real, "Grid power", "W"),
	in("pvPower", PortType::real, "PV power", "W"),
	in("housePower", PortType::real, "House consumption", "W"),
	in("batteryPower", PortType::real, "Battery power", "W"),
	in("batterySoc", PortType::real, "Battery state of charge", "%"),
	in("lowerPower", PortType::real, "Power of the lower loads", "W"),
	in("enable", PortType::boolean, "Surplus control on", ""),
	in("ready1", PortType::boolean, "Load 1 ready", ""),
	in("ready2", PortType::boolean, "Load 2 ready", ""),
	HC_PV_LOAD_OUTPUTS(1),
	HC_PV_LOAD_OUTPUTS(2),
	out("loadPower", PortType::real, "Power of the loads", "W", false, 0, false, false, 0),
	out("surplus", PortType::real, "Surplus left", "W", true, 0, false, false, 0),
};
#undef HC_PV_LOAD_OUTPUTS
static_assert(sizeof(kPorts) / sizeof(kPorts[0]) == PvSurplus::kPortCount, "ports");

constexpr Choice kGridSigns[] = {{0, "importPositive", "Import positive"}, {1, "exportPositive", "Export positive"}};
constexpr Choice kBatterySigns[] = {
	{0, "chargePositive", "Charging positive"}, {1, "dischargePositive", "Discharging positive"}};
constexpr Choice kUnits[] = {{0, "watt", "W"}, {1, "percent", "% of the power"}};

constexpr double kMinute = 60000;
constexpr double kDayMs = 86400000;

/// A real parameter in W.
constexpr ParamDecl watt(const char* id, const char* label, double value, double max, double step) {
	return ParamDecl{id, ParamType::real, label, "W", value, true, 0, true, max, step, nullptr, 0, false};
}

/// A load's power: whole W (D6: the levels' quotients are exact).
constexpr ParamDecl load_watt(const char* id, const char* label) {
	return ParamDecl{id, ParamType::integer, label, "W", 0, true, 0, true, 100000, 10, nullptr, 0, false};
}

/// A duration parameter of 0 .. 24 h.
constexpr ParamDecl hours(const char* id, const char* label, double value, bool adjustable) {
	return ParamDecl{id, ParamType::duration, label, "min", value, true, 0, true, kDayMs, kMinute, nullptr, 0,
		adjustable};
}

/// The eight parameters of a load.
#define HC_PV_LOAD_PARAMS(k) \
	load_watt("power" #k, "Load " #k " power"), \
	load_watt("minPower" #k, "Load " #k " lowest power"), \
	load_watt("step" #k, "Load " #k " step"), \
	hours("minOn" #k, "Load " #k " minimum on time", 5 * kMinute, false), \
	hours("minOff" #k, "Load " #k " minimum off time", 5 * kMinute, false), \
	hours("maxPerDay" #k, "Load " #k " longest runtime per day", 0, false), \
	ParamDecl{"mustRunBy" #k, ParamType::time_of_day, "Load " #k " must have run by", "", 64800, true, 0, true, \
		86399, 60, nullptr, 0, true}, \
	hours("mustRunFor" #k, "Load " #k " must have run for", 0, true)

// id, type, label, unit, default, has_min, min, has_max, max, step, choices, count, adjustable
constexpr ParamDecl kParams[] = {
	{"gridSign", ParamType::enumeration, "Grid meter's sign", "", 0, false, 0, false, 0, 0, kGridSigns, 2, false},
	{"batterySign", ParamType::enumeration, "Battery's sign", "", 0, false, 0, false, 0, 0, kBatterySigns, 2, false},
	{"surplusFilter", ParamType::duration, "Surplus filter", "s", 120000, true, 0, true, 1800000, 10000, nullptr, 0,
		false},
	watt("onMargin", "Switch-on margin", 100, 10000, 10),
	watt("allowedImport", "Allowed import", 100, 10000, 10),
	{"batteryPriority", ParamType::real, "Battery first below", "%", 100, true, 0, true, 100, 1, nullptr, 0, true},
	{"meterTimeout", ParamType::duration, "Meter timeout", "s", 300000, true, 10000, true, 3600000, 10000, nullptr, 0,
		false},
	{"setpointUnit", ParamType::enumeration, "Setpoint unit", "", 0, false, 0, false, 0, 0, kUnits, 2, false},
	HC_PV_LOAD_PARAMS(1),
	HC_PV_LOAD_PARAMS(2),
};
#undef HC_PV_LOAD_PARAMS
static_assert(sizeof(kParams) / sizeof(kParams[0]) == PvSurplus::kParamCount, "parameters");

#define HC_PV_LOAD_FIELDS(k) \
	{"level" #k, false, 0}, {"switchedAt" #k, false, -1}, {"run" #k, true, 0}, {"energy" #k, true, 0}

constexpr StateField kFields[] = {
	{"lastStep", false, 0}, {"filtered", false, 0}, {"hasFiltered", false, 0}, {"day", true, -1},
	{"dayStart", false, 0}, {"hasSent", false, 0}, {"sentLoadPower", false, 0},
	HC_PV_LOAD_FIELDS(1), HC_PV_LOAD_FIELDS(2),
};
#undef HC_PV_LOAD_FIELDS
static_assert(sizeof(kFields) / sizeof(kFields[0]) == PvSurplus::kFieldCount, "state fields");

constexpr Manifest kManifest = {
	"org.homecontrol.pv-surplus", 1, 0, "PV surplus control",
	"Up to two loads in the order of their priority from the PV surplus, behind the battery; forced by a deadline",
	kPorts, PvSurplus::kPortCount, kParams, PvSurplus::kParamCount, kFields, PvSurplus::kFieldCount, nullptr, 0,
	30000, 600000};

// min and max as spec/blocks.md 13 defines them (std::min/std::max: the first argument on a tie).
inline double min_of(double a, double b) { return std::min(a, b); }
inline double max_of(double a, double b) { return std::max(a, b); }

/// The parameters and fields of load k (0-based).
struct Load {
	const BlockContext& cx;
	double* s;
	size_t k;

	double param(size_t j) const { return cx.param(PvSurplus::kPower1 + PvSurplus::kParamsPerLoad * k + j); }
	double& field(size_t j) const { return s[PvSurplus::kLevel1 + PvSurplus::kFieldsPerLoad * k + j]; }

	double power() const { return param(0); }
	double min_power() const { return param(1); }
	double step() const { return param(2); }
	double lo() const { return step() == 0 ? power() : min_power(); }
	double top() const {
		return step() == 0 ? power() : min_power() + step() * std::floor((power() - min_power()) / step());
	}
	/// The highest level at or below y, 0 if none (spec/blocks.md 20.5 `fit`).
	double fit(double y) const {
		if (step() == 0) {
			return y >= power() ? power() : 0;
		}
		if (y < min_power()) {
			return 0;
		}
		return min_of(min_power() + step() * std::floor((y - min_power()) / step()), top());
	}
	/// The minimum times (20.5 `held`).
	double held(double want, double t) const {
		const double c = field(0);
		const double at = field(1);
		if (c == 0 && want > 0 && at >= 0 && t - at < param(4)) {
			return 0;
		}
		if (c > 0 && want == 0 && at >= 0 && t - at < param(3)) {
			return min_of(c, lo());
		}
		return want;
	}
};

} // namespace

const PvSurplus& PvSurplus::instance() {
	static const PvSurplus type;
	return type;
}

const Manifest& PvSurplus::manifest() const { return kManifest; }

const char* PvSurplus::check(const double* params, size_t& parameter) const {
	for (size_t k = 0; k < kLoads; ++k) {
		const size_t base = kPower1 + kParamsPerLoad * k;
		if (params[base + 2] > 0 && params[base] > 0 && !(params[base + 1] > 0 && params[base + 1] <= params[base])) {
			parameter = base + 1;
			return "a modulated load's minPower must be above 0 and at most its power";
		}
		if (params[base + 5] > 0 && !(params[base + 7] <= params[base + 5])) {
			parameter = base + 7;
			return "mustRunFor must be at most maxPerDay";
		}
	}
	return nullptr;
}

void PvSurplus::start(BlockContext& cx, StartReason reason, bool restored) const {
	double* s = cx.state(); // a fresh record holds the initial values already
	const double now = static_cast<double>(cx.now());
	if (!(restored && (reason == StartReason::image || reason == StartReason::enable))) {
		s[kLastStep] = now; // a record from flash: its monotonic times are gone (D11)
		s[kDayStart] = now;
	}
	s[kHasSent] = 0;
	s[kHasFiltered] = 0;
	if (reason == StartReason::enable) { // the failsafe switched the loads off (D7)
		for (size_t k = 0; k < kLoads; ++k) {
			s[kLevel1 + kFieldsPerLoad * k] = 0;
			s[kSwitchedAt1 + kFieldsPerLoad * k] = -1;
		}
	}
	step(cx);
}

// Every input has the trigger none and the type no timer; a parameter takes effect at the next
// period (D4).
void PvSurplus::input(BlockContext& cx, size_t port) const {
	(void)cx;
	(void)port;
}

void PvSurplus::timer(BlockContext& cx, size_t timer) const {
	(void)cx;
	(void)timer;
}

void PvSurplus::period(BlockContext& cx) const { step(cx); }

void PvSurplus::parameter(BlockContext& cx, size_t index) const {
	(void)cx;
	(void)index;
}

void PvSurplus::step(BlockContext& cx) const {
	double* s = cx.state();
	const double t = static_cast<double>(cx.now());

	const double dt = (t - s[kLastStep]) / 1000.0; // D11: the interval just ended
	for (size_t k = 0; k < kLoads; ++k) {
		const Load load{cx, s, k};
		if (load.field(0) > 0) {
			load.field(2) = load.field(2) + (t - s[kLastStep]);
			load.field(3) = load.field(3) + load.field(0) * (dt / 3600.0);
		}
	}
	s[kLastStep] = t;
	auto restart_day = [&]() {
		for (size_t k = 0; k < kLoads; ++k) {
			const Load load{cx, s, k};
			load.field(2) = 0;
			load.field(3) = 0;
		}
	};
	DateTime w;
	const bool wall = cx.wall(w);
	if (wall) {
		const double day = static_cast<double>(days_from_civil(w.year, w.month, w.day));
		if (s[kDay] < 0) {
			s[kDay] = day;
		} else if (day != s[kDay]) {
			restart_day();
			s[kDay] = day;
			s[kDayStart] = t;
		}
	} else if (t - s[kDayStart] >= kDayMs) {
		restart_day();
		s[kDay] = -1;
		s[kDayStart] = t;
	}

	const InputValue g = cx.read(kGridPower); // D3, D8
	const InputValue p = cx.read(kPvPower);
	const InputValue h = cx.read(kHousePower);
	const InputValue b = cx.read(kBatteryPower);
	const InputValue* meters[3] = {};
	size_t n = 0;
	if (g.bound) {
		meters[n++] = &g;
	} else if (p.bound && h.bound) {
		meters[n++] = &p;
		meters[n++] = &h;
	}
	if (n > 0 && b.bound) {
		meters[n++] = &b;
	}
	const double timeout = cx.param(kMeterTimeout);
	bool measured = n > 0;
	bool lost = n == 0;
	for (size_t i = 0; i < n; ++i) {
		const bool fresh = t - static_cast<double>(meters[i]->updated_ms) <= timeout;
		if (!(meters[i]->good() && fresh)) {
			measured = false;
		}
		if (meters[i]->quality == Quality::unknown || !fresh) {
			lost = true;
		}
	}
	const InputValue l = cx.read(kLowerPower);
	const double low = l.quality != Quality::unknown ? l.value : 0;
	if (lost) {
		s[kHasFiltered] = 0;
	}
	if (measured) {
		double exported = g.bound ? (cx.param(kGridSign) == 0 ? -g.value : g.value) : p.value - h.value;
		double battery = 0;
		if (b.bound) { // D9
			const double bc = cx.param(kBatterySign) == 0 ? b.value : -b.value;
			if (!g.bound) {
				exported = exported - bc;
			}
			const InputValue soc = cx.read(kBatterySoc);
			const bool first =
				soc.good() ? soc.value < cx.param(kBatteryPriority) : cx.param(kBatteryPriority) > 0;
			battery = first ? min_of(bc, 0.0) : bc;
		}
		const double x = exported + battery + s[kLevel1] + s[kLevel1 + kFieldsPerLoad] + low; // D2: the sample
		const double tf = cx.param(kSurplusFilter) / 1000.0;
		if (s[kHasFiltered] == 0 || tf == 0) {
			s[kFiltered] = x;
			s[kHasFiltered] = 1;
		} else {
			s[kFiltered] = s[kFiltered] + (x - s[kFiltered]) * (dt / (tf + dt));
		}
	}

	const InputValue e = cx.read(kEnable);
	const bool enabled = !(e.good() && e.value == 0);
	const bool known = s[kHasFiltered] == 1; // a lost surplus has cleared it
	bool allowed[kLoads];
	bool blocked[kLoads];
	bool forced[kLoads];
	double fresh_level[kLoads];
	for (size_t k = 0; k < kLoads; ++k) {
		const Load load{cx, s, k};
		const InputValue r = cx.read(kReady1 + k);
		allowed[k] = load.power() > 0 && enabled && (!r.bound || (r.good() && r.value != 0));
		const double max_per_day = load.param(5);
		blocked[k] = max_per_day > 0 && load.field(2) >= max_per_day;
		const double must_run_for = load.param(7);
		const double need = must_run_for - load.field(2);
		forced[k] = allowed[k] && !blocked[k] && must_run_for > 0 && need > 0 && wall &&
			w.hour * 3600 + w.minute * 60 + w.second + need / 1000.0 >= load.param(6);
	}
	double budget = s[kFiltered];
	for (size_t k = 0; k < kLoads; ++k) { // forced loads first (D10)
		if (forced[k]) {
			const Load load{cx, s, k};
			fresh_level[k] = load.held(load.top(), t);
			budget = budget - fresh_level[k];
		}
	}
	for (size_t k = 0; k < kLoads; ++k) { // then by priority (D2)
		if (forced[k]) {
			continue;
		}
		const Load load{cx, s, k};
		double want = 0;
		if (allowed[k] && !blocked[k] && known) {
			const double up = measured ? load.fit(budget - cx.param(kOnMargin)) : 0;
			const double c = load.field(0);
			const double down = c > 0 ? load.fit(min_of(c, budget + cx.param(kAllowedImport))) : 0;
			want = max_of(up, down);
		}
		fresh_level[k] = load.held(want, t);
		budget = budget - fresh_level[k];
	}

	const bool first = s[kHasSent] == 0; // D14
	const bool percent = cx.param(kSetpointUnit) != 0;
	for (size_t k = 0; k < kLoads; ++k) {
		const Load load{cx, s, k};
		const double level = fresh_level[k];
		const bool changed = level != load.field(0);
		if ((load.field(0) > 0) != (level > 0)) {
			load.field(1) = t;
		}
		load.field(0) = level;
		const double state = level > 0 ? (forced[k] ? PvSurplus::forced : PvSurplus::running)
			: !allowed[k]                ? PvSurplus::off
			: blocked[k]                 ? PvSurplus::blocked
										 : PvSurplus::waiting;
		const size_t port = kSwitch1 + kPortsPerLoad * k;
		if (first || changed) {
			cx.write(port, level > 0 ? 1 : 0);
			const double power = load.power();
			cx.write(port + 1, !percent ? level : power > 0 ? (level / power) * 100.0 : 0);
		}
		cx.write(port + 2, state);
		cx.write(port + 3, load.field(3));
	}
	const double load_power = s[kLevel1] + s[kLevel1 + kFieldsPerLoad] + low;
	if (first || load_power != s[kSentLoadPower]) {
		cx.write(kLoadPower, load_power);
		s[kSentLoadPower] = load_power;
	}
	if (s[kHasFiltered] == 1) { // what the loads leave: the next instance's meter (20.3)
		cx.write(kSurplus, s[kFiltered] - load_power);
	}
	s[kHasSent] = 1;
}

} // namespace hc::blocks
