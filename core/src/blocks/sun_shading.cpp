/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The sun shading of spec/blocks.md 18. Every formula is evaluated in the order the specification
// writes it (spec/blocks.md 13), on the exact trigonometry of 13.1: E-Home's Java port must give the
// same bits.
#include "hc/blocks/sun_shading.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "hc/exact_math.h"
#include "hc/tz.h"

namespace hc::blocks {

namespace {

constexpr uint8_t kMaxMean = aggregation_bit(Aggregation::max) | aggregation_bit(Aggregation::mean);
constexpr uint8_t kMax = aggregation_bit(Aggregation::max);
constexpr uint8_t kAny = aggregation_bit(Aggregation::any);

// id, direction, type, label, unit, required, multiple, aggregations, default aggregation, trigger,
// diagnostic, failsafe, has_min, min, has_max, max
constexpr PortDecl kPorts[] = {
	{"brightness", Direction::in, PortType::real, "Brightness", "lx", false, 4, kMaxMean, Aggregation::max,
		InputTrigger::change, false, 0, false, 0, false, 0},
	{"wind", Direction::in, PortType::real, "Wind speed", "m/s", false, 4, kMax, Aggregation::max,
		InputTrigger::update, false, 0, false, 0, false, 0},
	{"rain", Direction::in, PortType::boolean, "Rain", "", false, 4, kAny, Aggregation::any, InputTrigger::update,
		false, 0, false, 0, false, 0},
	{"frost", Direction::in, PortType::boolean, "Frost alarm", "", false, 1, 0, Aggregation::none,
		InputTrigger::change, false, 0, false, 0, false, 0},
	{"outside", Direction::in, PortType::real, "Outside temperature", "°C", false, 1, 0, Aggregation::none,
		InputTrigger::change, false, 0, false, 0, false, 0},
	{"manual", Direction::in, PortType::boolean, "Manual operation", "", false, 8, kAny, Aggregation::any,
		InputTrigger::update, false, 0, false, 0, false, 0},
	{"presence", Direction::in, PortType::boolean, "Presence", "", false, 1, 0, Aggregation::none,
		InputTrigger::change, false, 0, false, 0, false, 0},
	{"summer", Direction::in, PortType::boolean, "Summer", "", false, 1, 0, Aggregation::none, InputTrigger::change,
		false, 0, false, 0, false, 0},
	{"roomTemperature", Direction::in, PortType::real, "Room temperature", "°C", false, 4, kMaxMean,
		Aggregation::max, InputTrigger::change, false, 0, false, 0, false, 0},
	{"enable", Direction::in, PortType::boolean, "Automatic on", "", false, 1, 0, Aggregation::none,
		InputTrigger::change, false, 0, false, 0, false, 0},
	{"position", Direction::out, PortType::real, "Position", "%", false, 1, 0, Aggregation::none,
		InputTrigger::change, false, 0, true, 0, true, 100},
	{"slat", Direction::out, PortType::real, "Slat position", "%", false, 1, 0, Aggregation::none,
		InputTrigger::change, false, 0, true, 0, true, 100},
	{"upDown", Direction::out, PortType::boolean, "Up/down", "", false, 1, 0, Aggregation::none,
		InputTrigger::change, false, 0, false, 0, false, 0},
	{"state", Direction::out, PortType::enumeration, "State", "", false, 1, 0, Aggregation::none,
		InputTrigger::change, true, 0, true, 0, true, 7},
	{"sunAzimuth", Direction::out, PortType::real, "Sun azimuth", "°", false, 1, 0, Aggregation::none,
		InputTrigger::change, true, 0, false, 0, false, 0},
	{"sunElevation", Direction::out, PortType::real, "Sun elevation", "°", false, 1, 0, Aggregation::none,
		InputTrigger::change, true, 0, false, 0, false, 0},
};
static_assert(sizeof(kPorts) / sizeof(kPorts[0]) == SunShading::kPortCount, "ports");

constexpr Choice kSlatModes[] = {{0, "fixed", "Fixed"}, {1, "tracking", "Following the sun"}};
constexpr Choice kLocks[] = {
	{0, "duration", "For a duration"}, {1, "sunChange", "Until the sun changes"}, {2, "midnight", "Until midnight"}};

constexpr double kMinute = 60000;
constexpr double kHour = 60 * kMinute;

#define HC_HORIZON(n, a) \
	{"horizonAngle" #n, ParamType::real, "Horizon direction " #n, "°", a, true, -90, true, 90, 1, nullptr, 0, false}, \
	{"horizonElevation" #n, ParamType::real, "Horizon elevation " #n, "°", 0, true, 0, true, 90, 1, nullptr, 0, false}

// id, type, label, unit, default, has_min, min, has_max, max, step, choices, count, adjustable
constexpr ParamDecl kParams[] = {
	{"facadeAzimuth", ParamType::real, "Facade direction", "°", 180, true, 0, true, 360, 1, nullptr, 0, false},
	{"openingAngle", ParamType::real, "Opening angle", "°", 180, true, 10, true, 180, 1, nullptr, 0, false},
	{"minElevation", ParamType::real, "Lowest sun elevation", "°", 5, true, 0, true, 90, 1, nullptr, 0, false},
	{"overhangAngle", ParamType::real, "Overhang angle", "°", 90, true, 10, true, 90, 1, nullptr, 0, false},
	{"horizonPoints", ParamType::integer, "Horizon points", "", 0, true, 0, true, 4, 1, nullptr, 0, false},
	HC_HORIZON(1, -60),
	HC_HORIZON(2, -20),
	HC_HORIZON(3, 20),
	HC_HORIZON(4, 60),
	{"brightnessOn", ParamType::real, "Bright from", "lx", 30000, true, 0, true, 200000, 1000, nullptr, 0, false},
	{"brightnessOff", ParamType::real, "Dark below", "lx", 20000, true, 0, true, 200000, 1000, nullptr, 0, false},
	{"brightnessOnDelay", ParamType::duration, "Bright delay", "min", 2 * kMinute, true, 0, true, kHour, kMinute,
		nullptr, 0, false},
	{"brightnessOffDelay", ParamType::duration, "Dark delay", "min", 15 * kMinute, true, 0, true, 2 * kHour,
		kMinute, nullptr, 0, false},
	{"shadePosition", ParamType::real, "Shading position", "%", 100, true, 0, true, 100, 5, nullptr, 0, true},
	{"slatMode", ParamType::enumeration, "Slats while shading", "", 0, false, 0, false, 0, 0, kSlatModes, 2, false},
	{"shadeSlat", ParamType::real, "Slat position", "%", 75, true, 0, true, 100, 5, nullptr, 0, false},
	{"slatRatio", ParamType::real, "Slat spacing to width", "", 0.9, true, 0.2, true, 1, 0.01, nullptr, 0, false},
	{"slatOpen", ParamType::real, "Slat position when horizontal", "%", 0, true, 0, true, 90, 5, nullptr, 0, false},
	{"slatStep", ParamType::real, "Slat step", "%", 10, true, 1, true, 50, 1, nullptr, 0, false},
	{"heatLimit", ParamType::real, "Heat protection above", "°C", 26, true, 18, true, 40, 0.5, nullptr, 0, true},
	{"windLimit", ParamType::real, "Wind alarm from", "m/s", 10, true, 1, true, 40, 0.5, nullptr, 0, false},
	{"windTimeout", ParamType::duration, "Wind sensor timeout", "min", 0, true, 0, true, 24 * kHour, kMinute, nullptr,
		0, false},
	{"frostLimit", ParamType::real, "Frost alarm below", "°C", 2, true, -20, true, 10, 0.5, nullptr, 0, false},
	{"alarmRelease", ParamType::duration, "Alarm release delay", "min", 10 * kMinute, true, 0, true, 2 * kHour,
		kMinute, nullptr, 0, false},
	{"manualLock", ParamType::enumeration, "After an operation", "", 0, false, 0, false, 0, 0, kLocks, 3, false},
	{"manualDuration", ParamType::duration, "Suspension after an operation", "min", 2 * kHour, true, kMinute, true,
		24 * kHour, kMinute, nullptr, 0, false},
	{"minMoveInterval", ParamType::duration, "Shortest time between moves", "min", 5 * kMinute, true, 0, true, kHour,
		kMinute, nullptr, 0, false},
};
#undef HC_HORIZON
static_assert(sizeof(kParams) / sizeof(kParams[0]) == SunShading::kParamCount, "parameters");

constexpr StateField kFields[] = {
	{"lastState", false, -1}, {"windOn", false, 0}, {"windCalmAt", false, -1}, {"rainOn", false, 0},
	{"rainDryAt", false, -1}, {"frostOn", false, 0}, {"frostClearAt", false, -1}, {"bright", false, 0},
	{"brightAt", false, -1}, {"heat", false, 0}, {"manualAt", false, -1}, {"manualSunny", false, 0},
	{"manualDay", false, -1}, {"manualSeen", false, -1}, {"holding", false, 0}, {"due", false, 0}, {"movedAt", false, -1},
	{"sentPosition", false, -1}, {"sentSlat", false, -1}, {"sentUpDown", false, -1},
};
static_assert(sizeof(kFields) / sizeof(kFields[0]) == SunShading::kFieldCount, "state fields");

const char* const kTimers[] = {"wake"};

constexpr Manifest kManifest = {
	"org.homecontrol.sun-shading", 1, 0, "Sun shading",
	"Shading of a facade by the sun's position and the brightness, with slat tracking, heat protection, wind, "
	"rain and frost alarms and a lock after manual operation",
	kPorts, SunShading::kPortCount, kParams, SunShading::kParamCount, kFields, SunShading::kFieldCount,
	kTimers, 1, 60000, 0};

constexpr size_t angle_of(size_t i) { return SunShading::kHorizonAngle1 + 2 * i; }
constexpr size_t elevation_of(size_t i) { return SunShading::kHorizonElevation1 + 2 * i; }

bool is_alarm(double state) {
	return state == static_cast<double>(SunShading::wind) || state == static_cast<double>(SunShading::rain) ||
		state == static_cast<double>(SunShading::frost);
}

bool is_active(double state) {
	return state == static_cast<double>(SunShading::shading) || state == static_cast<double>(SunShading::heat_protection);
}

/// The alarm of spec/blocks.md 18.4: on at once, off when the condition has not held for `release`.
void alarm(double& on, double& at, bool cond, double t, double release) {
	if (cond) {
		on = 1;
		at = -1;
	} else if (on == 1) {
		if (at < 0) {
			at = t;
		}
		if (t - at >= release) {
			on = 0;
			at = -1;
		}
	}
}

/// The earlier of a deadline so far (-1: none) and `at`.
void earliest(double& wake, double at) {
	if (wake < 0 || at < wake) {
		wake = at;
	}
}

} // namespace

const SunShading& SunShading::instance() {
	static const SunShading type;
	return type;
}

const Manifest& SunShading::manifest() const { return kManifest; }

double SunShading::horizon(const double* params, double g) {
	const size_t n = static_cast<size_t>(params[kHorizonPoints]);
	if (g <= params[angle_of(0)]) {
		return params[elevation_of(0)];
	}
	if (g >= params[angle_of(n - 1)]) {
		return params[elevation_of(n - 1)];
	}
	for (size_t i = 0; i + 1 < n; ++i) {
		const double x0 = params[angle_of(i)];
		const double x1 = params[angle_of(i + 1)];
		if (g <= x1) {
			const double y0 = params[elevation_of(i)];
			const double y1 = params[elevation_of(i + 1)];
			return y0 + (y1 - y0) * ((g - x0) / (x1 - x0));
		}
	}
	return params[elevation_of(n - 1)];
}

double SunShading::tracked_slat(const double* params, double profile) {
	double beta = exact::asin_deg(params[kSlatRatio] * exact::cos_deg(profile)) - profile;
	beta = std::min(std::max(beta, 0.0), 90.0);
	const double open = params[kSlatOpen];
	const double q = open + (100.0 - open) * (beta / 90.0);
	const double step = params[kSlatStep];
	return std::min(open + step * -std::floor(-((q - open) / step)), 100.0);
}

const char* SunShading::check(const double* params, size_t& parameter) const {
	if (!(params[kBrightnessOff] < params[kBrightnessOn])) {
		parameter = kBrightnessOff;
		return "brightnessOff must be below brightnessOn";
	}
	const size_t n = static_cast<size_t>(params[kHorizonPoints]);
	for (size_t i = 0; i + 1 < n; ++i) {
		if (!(params[angle_of(i + 1)] > params[angle_of(i)])) {
			parameter = angle_of(i + 1);
			return "the horizon points' directions must rise";
		}
	}
	return nullptr;
}

double SunShading::latest_manual(const BlockContext& cx) {
	double latest = -1;
	for (size_t k = 0; k < cx.count(kManual); ++k) {
		const InputValue v = cx.read_at(kManual, k);
		if (v.quality != Quality::unknown) {
			latest = std::max(latest, static_cast<double>(v.updated_ms));
		}
	}
	return latest;
}

void SunShading::start(BlockContext& cx, StartReason reason, bool restored) const {
	(void)reason;
	(void)restored; // a fresh record holds the initial values already
	double* s = cx.state();
	s[kLastState] = -1;
	s[kMovedAt] = -1;
	s[kManualSeen] = latest_manual(cx);
	cx.cancel_timer(0);
	step(cx, false);
}

void SunShading::input(BlockContext& cx, size_t port) const {
	bool operated = false;
	if (port == kManual) {
		double* s = cx.state();
		const double latest = latest_manual(cx);
		operated = latest > s[kManualSeen];
		s[kManualSeen] = std::max(s[kManualSeen], latest);
	}
	step(cx, operated);
}

void SunShading::timer(BlockContext& cx, size_t timer) const {
	(void)timer;
	step(cx, false);
}

void SunShading::period(BlockContext& cx) const { step(cx, false); }

void SunShading::parameter(BlockContext& cx, size_t index) const {
	(void)index;
	step(cx, false);
}

void SunShading::step(BlockContext& cx, bool operated) const {
	double* s = cx.state();
	double p[kParamCount];
	for (size_t i = 0; i < kParamCount; ++i) {
		p[i] = cx.param(i);
	}
	const uint64_t now = cx.now();
	const double t = static_cast<double>(now);
	const double release = p[kAlarmRelease];

	// The alarms (D3, D4).
	const double wind_timeout = p[kWindTimeout];
	const size_t winds = cx.count(kWind);
	bool wind_cond = false;
	for (size_t k = 0; k < winds; ++k) {
		const InputValue W = cx.read_at(kWind, k);
		const bool known = W.quality != Quality::unknown;
		if ((known && W.value >= p[kWindLimit]) ||
			(wind_timeout > 0 && (!known || static_cast<double>(now - W.updated_ms) >= wind_timeout))) {
			wind_cond = true;
		}
	}
	alarm(s[kWindOn], s[kWindCalmAt], wind_cond, t, release);
	bool rain_cond = false;
	for (size_t k = 0; k < cx.count(kRain); ++k) {
		const InputValue R = cx.read_at(kRain, k);
		if (R.quality != Quality::unknown && R.value != 0) {
			rain_cond = true;
		}
	}
	alarm(s[kRainOn], s[kRainDryAt], rain_cond, t, release);
	const InputValue F = cx.read(kFrost);
	const InputValue O = cx.read(kOutside);
	alarm(s[kFrostOn], s[kFrostClearAt],
		(F.quality != Quality::unknown && F.value != 0) || (O.quality != Quality::unknown && O.value < p[kFrostLimit]),
		t, release);

	// The brightness (D9).
	const InputValue B = cx.read(kBrightness);
	if (!B.bound) {
		s[kBright] = 1;
		s[kBrightAt] = -1;
	} else {
		bool change;
		double delay;
		if (s[kBright] == 0) {
			change = B.good() && B.value >= p[kBrightnessOn];
			delay = p[kBrightnessOnDelay];
		} else {
			change = !B.good() || B.value < p[kBrightnessOff];
			delay = p[kBrightnessOffDelay];
		}
		if (!change) {
			s[kBrightAt] = -1;
		} else {
			if (s[kBrightAt] < 0) {
				s[kBrightAt] = t;
			}
			if (t - s[kBrightAt] >= delay) {
				s[kBright] = 1 - s[kBright];
				s[kBrightAt] = -1;
			}
		}
	}

	// The sun on the facade (D1, D8).
	SunPosition sun;
	const bool sun_known = cx.sun(sun);
	bool sun_on = false;
	double profile = 0;
	if (sun_known) {
		const double az = sun.azimuth;
		const double el = sun.elevation;
		double g = az - p[kFacadeAzimuth];
		g = g - 360.0 * std::floor((g + 180.0) / 360.0);
		double sin_el, cos_el;
		exact::sin_cos_deg(el, sin_el, cos_el);
		profile = exact::atan2_deg(sin_el, cos_el * exact::cos_deg(g));
		sun_on = std::fabs(g) < p[kOpeningAngle] / 2.0 && el > p[kMinElevation] &&
			(p[kHorizonPoints] == 0 || el > horizon(p, g)) && (p[kOverhangAngle] == 90 || profile < p[kOverhangAngle]);
	}
	const double sunny = sun_on && s[kBright] == 1 ? 1 : 0;

	// Heat protection (D11).
	const InputValue P = cx.read(kPresence);
	const InputValue S = cx.read(kSummer);
	const InputValue RT = cx.read(kRoomTemperature);
	if (P.good() && P.value == 0 && (!S.bound || (S.good() && S.value != 0)) && RT.good()) {
		if (RT.value > p[kHeatLimit]) {
			s[kHeat] = 1;
		} else if (RT.value < p[kHeatLimit] - 1.0) {
			s[kHeat] = 0;
		}
	} else {
		s[kHeat] = 0;
	}

	// The lock after an operation (D6, D7).
	const bool alarmed = s[kWindOn] == 1 || s[kRainOn] == 1 || s[kFrostOn] == 1;
	DateTime w;
	const bool w_valid = cx.wall(w);
	const double today = w_valid ? static_cast<double>(days_from_civil(w.year, w.month, w.day)) : -1;
	const Lock lock = static_cast<Lock>(static_cast<int>(p[kManualLock]));
	if (operated && !alarmed) {
		s[kManualAt] = t;
		s[kManualSunny] = sunny;
		s[kManualDay] = today;
		s[kHolding] = 0;
	}
	if (s[kManualAt] >= 0) {
		bool end;
		if (lock == midnight && s[kManualDay] >= 0 && w_valid) {
			end = today != s[kManualDay];
		} else {
			end = t - s[kManualAt] >= p[kManualDuration] || (lock == sun_change && sunny != s[kManualSunny]);
		}
		if (end) {
			s[kManualAt] = -1;
			s[kManualDay] = -1;
		}
	}

	const InputValue E = cx.read(kEnable);
	State state;
	if (s[kWindOn] == 1) {
		state = wind;
	} else if (s[kRainOn] == 1) {
		state = rain;
	} else if (s[kFrostOn] == 1) {
		state = frost;
	} else if (s[kManualAt] >= 0) {
		state = manual;
	} else if (E.good() && E.value == 0) {
		state = off;
	} else if (s[kHeat] == 1) {
		state = heat_protection;
	} else if (sunny == 1) {
		state = shading;
	} else {
		state = idle;
	}

	// The target and the moves (D2, D3, D10, D11, D12).
	bool has_target = false;
	double tp = 0, tq = 0, tu = 0;
	if (is_alarm(state)) {
		if (!is_alarm(s[kLastState]) || operated) {
			cx.write(kPosition, 0);
			cx.write(kSlat, 0);
			cx.write(kUpDown, 0);
			s[kSentPosition] = 0;
			s[kSentSlat] = 0;
			s[kSentUpDown] = 0;
			s[kMovedAt] = t;
			s[kHolding] = 0;
			s[kDue] = 0;
		}
	} else if (is_active(state)) {
		if (!is_active(s[kLastState])) {
			s[kDue] = 1;
		}
		has_target = true;
		if (state == heat_protection) {
			tp = 100;
			tq = 100;
			tu = 1;
		} else {
			tp = p[kShadePosition];
			tq = static_cast<SlatMode>(static_cast<int>(p[kSlatMode])) == tracking ? tracked_slat(p, profile)
																				 : p[kShadeSlat];
			tu = tp > 0 ? 1 : 0;
		}
	} else if (state == idle && s[kHolding] == 1) {
		has_target = true;
		s[kDue] = 1;
	} else {
		s[kDue] = 0;
	}
	double held_until = -1;
	if (has_target &&
		(s[kDue] == 1 || tp != s[kSentPosition] || tq != s[kSentSlat] || tu != s[kSentUpDown])) {
		if (s[kMovedAt] < 0 || t - s[kMovedAt] >= p[kMinMoveInterval]) {
			const bool all = s[kDue] == 1;
			if (all || tp != s[kSentPosition]) {
				cx.write(kPosition, tp);
				s[kSentPosition] = tp;
			}
			if (all || tq != s[kSentSlat]) {
				cx.write(kSlat, tq);
				s[kSentSlat] = tq;
			}
			if (all || tu != s[kSentUpDown]) {
				cx.write(kUpDown, tu);
				s[kSentUpDown] = tu;
			}
			s[kMovedAt] = t;
			s[kDue] = 0;
			s[kHolding] = is_active(state) ? 1 : 0;
		} else {
			held_until = s[kMovedAt] + p[kMinMoveInterval];
		}
	}

	// The timer.
	double wake = -1;
	if (s[kWindOn] == 1 && s[kWindCalmAt] >= 0) {
		earliest(wake, s[kWindCalmAt] + release);
	}
	if (s[kRainOn] == 1 && s[kRainDryAt] >= 0) {
		earliest(wake, s[kRainDryAt] + release);
	}
	if (s[kFrostOn] == 1 && s[kFrostClearAt] >= 0) {
		earliest(wake, s[kFrostClearAt] + release);
	}
	for (size_t k = 0; wind_timeout > 0 && k < winds; ++k) {
		const InputValue W = cx.read_at(kWind, k);
		if (W.quality != Quality::unknown && static_cast<double>(now - W.updated_ms) < wind_timeout) {
			earliest(wake, static_cast<double>(W.updated_ms) + wind_timeout);
		}
	}
	if (s[kBrightAt] >= 0) {
		earliest(wake, s[kBrightAt] + (s[kBright] == 0 ? p[kBrightnessOnDelay] : p[kBrightnessOffDelay]));
	}
	if (s[kManualAt] >= 0 && !(lock == midnight && s[kManualDay] >= 0 && w_valid)) {
		earliest(wake, s[kManualAt] + p[kManualDuration]);
	}
	if (held_until >= 0) {
		earliest(wake, held_until);
	}
	if (wake >= 0) {
		cx.set_timer(0, static_cast<int64_t>(wake - t));
	} else {
		cx.cancel_timer(0);
	}

	cx.write(kState, static_cast<double>(state));
	if (sun_known) {
		cx.write(kSunAzimuth, sun.azimuth);
		cx.write(kSunElevation, sun.elevation);
	}
	s[kLastState] = static_cast<double>(state);
}

} // namespace hc::blocks
