/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The house mode of spec/blocks.md 19. Every formula is evaluated in the order the specification
// writes it (spec/blocks.md 13): E-Home's Java port must give the same bits.
#include "hc/blocks/house_mode.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "hc/tz.h"

namespace hc::blocks {

namespace {

constexpr uint8_t kAny = aggregation_bit(Aggregation::any);

// The modes as the state record holds them (a double compares with a double, not with an enumerator).
constexpr double kHomeMode = HouseMode::home;
constexpr double kAwayMode = HouseMode::away;
constexpr double kNightMode = HouseMode::night;
constexpr double kVacationMode = HouseMode::vacation;

/// An input of the house mode: boolean, `multiple` datapoints (aggregation `any`), the trigger.
constexpr PortDecl in(const char* id, const char* label, uint8_t multiple, InputTrigger trigger) {
	return PortDecl{id, Direction::in, PortType::boolean, label, "", false, multiple, multiple > 1 ? kAny : uint8_t{0},
		multiple > 1 ? Aggregation::any : Aggregation::none, trigger, false, 0, false, 0, false, 0};
}

/// An output: its type, failsafe (none when diagnostic) and bounds.
constexpr PortDecl out(const char* id, PortType type, const char* label, const char* unit, bool diagnostic,
	double failsafe, bool bounded, double max) {
	return PortDecl{id, Direction::out, type, label, unit, false, 1, 0, Aggregation::none, InputTrigger::change,
		diagnostic, failsafe, bounded, 0, bounded, max};
}

constexpr PortDecl kPorts[] = {
	in("presence", "Presence", 16, InputTrigger::change),
	in("awayButton", "Away button", 4, InputTrigger::update),
	in("nightButton", "Night button", 4, InputTrigger::update),
	in("homeButton", "Home button", 4, InputTrigger::update),
	in("vacation", "Vacation", 1, InputTrigger::change),
	in("morning", "Morning", 1, InputTrigger::change),
	in("evening", "Evening", 1, InputTrigger::change),
	out("mode", PortType::enumeration, "Mode", "", false, 0, true, 3),
	out("hvacMode", PortType::enumeration, "HVAC mode", "", false, 0, true, 4),
	out("home", PortType::boolean, "Home", "", false, 1, false, 0),
	out("away", PortType::boolean, "Away", "", false, 0, false, 0),
	out("night", PortType::boolean, "Night", "", false, 0, false, 0),
	out("onVacation", PortType::boolean, "On vacation", "", false, 0, false, 0),
	out("occupied", PortType::boolean, "Occupied", "", false, 1, false, 0),
	out("since", PortType::integer, "Since", "min", true, 0, false, 0),
};
static_assert(sizeof(kPorts) / sizeof(kPorts[0]) == HouseMode::kPortCount, "ports");

constexpr Choice kHvacModes[] = {{0, "auto", "Auto"}, {1, "comfort", "Comfort"}, {2, "standby", "Standby"},
	{3, "night", "Night"}, {4, "protection", "Frost/building protection"}};

/// A parameter `hvac<Mode>`: an HVAC mode of DPT 20.102.
constexpr ParamDecl hvac(const char* id, const char* label, double value) {
	return ParamDecl{id, ParamType::enumeration, label, "", value, false, 0, false, 0, 0, kHvacModes, 5, false};
}

// id, type, label, unit, default, has_min, min, has_max, max, step, choices, count, adjustable
constexpr ParamDecl kParams[] = {
	{"awayDelay", ParamType::duration, "Away after", "min", 1800000, true, 60000, true, 86400000, 60000, nullptr, 0,
		true},
	{"returnDetection", ParamType::boolean, "Return detection", "", 1, false, 0, false, 0, 0, nullptr, 0, true},
	hvac("hvacHome", "HVAC mode at home", 1),
	hvac("hvacAway", "HVAC mode away", 2),
	hvac("hvacNight", "HVAC mode at night", 3),
	hvac("hvacVacation", "HVAC mode on vacation", 4),
};
static_assert(sizeof(kParams) / sizeof(kParams[0]) == HouseMode::kParamCount, "parameters");

constexpr StateField kFields[] = {
	{"mode", true, 0}, {"since", true, -1}, {"changedAt", false, -1}, {"absentSince", false, -1}, {"armed", false, 1},
	{"awaySeen", false, -1}, {"nightSeen", false, -1}, {"homeSeen", false, -1}, {"lastMorning", false, -1},
	{"lastEvening", false, -1}, {"sentMode", false, -1}, {"sentSince", false, -1},
};
static_assert(sizeof(kFields) / sizeof(kFields[0]) == HouseMode::kFieldCount, "state fields");

constexpr const char* kTimers[] = {"away"};

constexpr Manifest kManifest = {
	"org.homecontrol.house-mode", 1, 0, "House mode",
	"Home, away, night or vacation from presence, buttons and schedules; the HVAC mode for the room controllers",
	kPorts, HouseMode::kPortCount, kParams, HouseMode::kParamCount, kFields, HouseMode::kFieldCount, kTimers, 1, 60000,
	60000};

/// The largest update time of the port's datapoints that are good and true; -1 if none (19.4).
double latest(const BlockContext& cx, size_t port) {
	double result = -1;
	for (size_t k = 0; k < cx.count(port); ++k) {
		const InputValue v = cx.read_at(port, k);
		if (v.is_true()) {
			result = std::max(result, static_cast<double>(v.updated_ms));
		}
	}
	return result;
}

/// The input's value when it is not unknown, else -1 (19.4).
double known(const BlockContext& cx, size_t port) {
	const InputValue v = cx.read(port);
	return v.quality == Quality::unknown ? -1 : v.value;
}

} // namespace

const HouseMode& HouseMode::instance() {
	static const HouseMode type;
	return type;
}

const Manifest& HouseMode::manifest() const { return kManifest; }

void HouseMode::start(BlockContext& cx, StartReason reason, bool restored) const {
	(void)reason;
	double* s = cx.state();
	if (!restored) { // a fresh record holds the initial values already
		s[kChangedAt] = static_cast<double>(cx.now());
	} else if (!(s[kModeField] == kHomeMode || s[kModeField] == kAwayMode || s[kModeField] == kNightMode ||
				   s[kModeField] == kVacationMode)) {
		s[kModeField] = kHomeMode; // a later minor version's mode (D11): a change
		s[kChangedAt] = static_cast<double>(cx.now());
		s[kSinceField] = -1;
	}
	s[kAbsentSince] = -1;
	s[kSentMode] = -1;
	s[kSentSince] = -1;
	s[kAwaySeen] = latest(cx, kAwayButton);
	s[kNightSeen] = latest(cx, kNightButton);
	s[kHomeSeen] = latest(cx, kHomeButton);
	s[kLastMorning] = known(cx, kMorning);
	s[kLastEvening] = known(cx, kEvening);
	cx.cancel_timer(0);
	step(cx, none);
}

void HouseMode::input(BlockContext& cx, size_t port) const {
	double* s = cx.state();
	Event event = none;
	if (port == kAwayButton || port == kNightButton || port == kHomeButton) {
		double& seen = s[kAwaySeen + (port - kAwayButton)];
		const double l = latest(cx, port);
		if (l > seen) {
			event = static_cast<Event>(away_press + (port - kAwayButton));
		}
		seen = std::max(seen, l);
	} else if (port == kMorning || port == kEvening) {
		double& last = s[port == kMorning ? kLastMorning : kLastEvening];
		const InputValue v = cx.read(port);
		if (v.good() && v.value != 0 && last == 0) {
			event = port == kMorning ? morning : evening;
		}
		if (v.quality != Quality::unknown) {
			last = v.value;
		}
	}
	step(cx, event);
}

void HouseMode::timer(BlockContext& cx, size_t timer) const {
	(void)timer;
	step(cx, none);
}

void HouseMode::period(BlockContext& cx) const { step(cx, none); }

void HouseMode::parameter(BlockContext& cx, size_t index) const {
	(void)index;
	step(cx, none);
}

void HouseMode::step(BlockContext& cx, Event event) const {
	double* s = cx.state();
	const double t = static_cast<double>(cx.now());
	auto change = [&](double m) {
		if (m != s[kModeField]) {
			s[kModeField] = m;
			s[kChangedAt] = t;
			s[kSinceField] = -1;
			s[kAbsentSince] = -1;
		}
	};

	const InputValue v = cx.read(kVacation); // D8
	if (v.good() && v.value != 0) {
		change(kVacationMode);
	} else if (s[kModeField] == kVacationMode && (v.good() || !v.bound)) {
		change(kHomeMode);
	}
	if (s[kModeField] != kVacationMode) {
		const double mode = s[kModeField];
		if (event == away_press) { // D5
			change(kAwayMode);
			s[kArmed] = 0;
		} else if (event == night_press) {
			if (mode != kNightMode) {
				change(kNightMode);
			}
		} else if (event == home_press) { // D4: also while home
			change(kHomeMode);
			s[kAbsentSince] = -1;
		} else if (event == evening) { // D7
			if (mode == kHomeMode) {
				change(kNightMode);
			}
		} else if (event == morning) {
			if (mode == kNightMode) {
				change(kHomeMode);
			}
		}
		const InputValue p = cx.read(kPresence); // D2, D5
		const bool present = p.good() && p.value != 0;
		const bool absent = p.good() && p.value == 0;
		if (s[kModeField] == kAwayMode) {
			if (absent) {
				s[kArmed] = 1;
			} else if (present && s[kArmed] == 1 && cx.param(kReturnDetection) != 0) {
				change(kHomeMode);
			}
		}
		if (s[kModeField] == kHomeMode) {
			if (!absent) {
				s[kAbsentSince] = -1;
			} else {
				if (s[kAbsentSince] < 0) {
					s[kAbsentSince] = t;
				}
				if (t - s[kAbsentSince] >= cx.param(kAwayDelay)) {
					change(kAwayMode);
					s[kArmed] = 1;
				}
			}
		}
	}

	DateTime w; // D14
	if (s[kSinceField] < 0 && s[kChangedAt] >= 0 && cx.wall(w)) {
		const double local = static_cast<double>(days_from_civil(w.year, w.month, w.day)) * 86400 + w.hour * 3600 +
			w.minute * 60 + w.second;
		s[kSinceField] = std::floor((local - (t - s[kChangedAt]) / 1000.0) / 60.0);
	}

	if (s[kModeField] == kHomeMode && s[kAbsentSince] >= 0) {
		cx.set_timer(0, static_cast<int64_t>(s[kAbsentSince] + cx.param(kAwayDelay) - t));
	} else {
		cx.cancel_timer(0);
	}

	const double mode = s[kModeField];
	if (mode != s[kSentMode]) { // D13
		cx.write(kMode, mode);
		cx.write(kHvacMode, cx.param(kHvacHome + static_cast<size_t>(mode)));
		for (size_t m = home; m <= vacation; ++m) {
			cx.write(kHome + m, mode == m ? 1 : 0);
		}
		cx.write(kOccupied, mode == kHomeMode || mode == kNightMode ? 1 : 0);
		s[kSentMode] = mode;
	}
	if (s[kSinceField] >= 0 && s[kSinceField] != s[kSentSince]) {
		cx.write(kSince, s[kSinceField]);
		s[kSentSince] = s[kSinceField];
	}
}

} // namespace hc::blocks
