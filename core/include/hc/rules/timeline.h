/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_RULES_TIMELINE_H
#define HC_RULES_TIMELINE_H

#include <cstddef>
#include <cstdint>

#include "hc/astro.h"
#include "hc/json.h"
#include "hc/json_value.h"
#include "hc/rules/rule_set.h"

namespace hc::rules {

class TimeSource;

/// The limits of a timeline (spec/rules.md 10.2, issue #84).
struct TimelineLimits {
	static constexpr size_t kPlans = 8;
	static constexpr size_t kPointsPerPlan = 16;
	static constexpr size_t kPoints = 64;
	static constexpr size_t kPlanLabelLanguages = 4;
	static constexpr size_t kAssignments = 8;
	/// Over all timelines of an image, and over all effective timelines.
	static constexpr size_t kImagePoints = 256;
	static constexpr size_t kImagePlans = 64;
	/// Dates searched back for the state due now: today and the seven before (spec 7.13).
	static constexpr int kSearchDays = 7;
};

/// A switching point, compiled: 16 bytes.
struct TimelinePoint {
	uint8_t state;   ///< Index of the rule's state.
	uint8_t sun;     ///< kClock, or 1 + hc::SunEvent.
	uint8_t flags;   ///< kNotBefore, kNotAfter.
	uint8_t reserved;
	int32_t seconds; ///< A clock point: the time of day in seconds; a sun point: the offset in seconds.
	int32_t not_before; ///< Seconds of the day.
	int32_t not_after;

	static constexpr uint8_t kClock = 0;
	static constexpr uint8_t kNotBefore = 0x01;
	static constexpr uint8_t kNotAfter = 0x02;

	bool is_sun() const { return sun != kClock; }
	hc::SunEvent event() const { return static_cast<hc::SunEvent>(sun - 1); }
};

/// A day plan: its id and label (offsets into the timeline's text), its points.
struct TimelinePlan {
	uint32_t id;
	uint32_t label; ///< The label as compact JSON (a string or an object of language tags).
	uint16_t first_point;
	uint8_t point_count;
};

/// An assignment: the plan and its weekdays (bit 0 Monday .. bit 6 Sunday; 0: the default).
struct TimelineAssignment {
	uint8_t plan;
	uint8_t days;
};

/// The states a timeline may name: the rule's state ids in their order.
class TimelineStates {
public:
	virtual size_t count() const = 0;
	virtual const char* id(size_t index) const = 0;
	/// The index of `id`; -1 if none.
	int find(const char* id) const;

protected:
	~TimelineStates() = default;
};

/// The states of a rule of a RuleSet.
class RuleSetStates : public TimelineStates {
public:
	RuleSetStates(const RuleSet& set, size_t rule) : set_(set), rule_(rule) {}
	size_t count() const override { return set_.rule(rule_).state_count; }
	const char* id(size_t index) const override {
		return set_.str(set_.state(set_.rule(rule_).first_state + index).id);
	}

private:
	const RuleSet& set_;
	size_t rule_;
};

/// Why a timeline was refused: the reason of spec/rules.md 10.1, the path relative to the timeline
/// ("" for the timeline itself, "plans[0].points[1].state") and the text.
struct TimelineRefusal {
	LoadError error = LoadError::ok;
	char path[64] = {};
	char text[96] = {};
};

/// A compiled timeline (spec/rules.md 2.8): the image's, or the inhabitant's (8.7). One heap block
/// holds the points, plans, assignments and the plans' ids and labels. Movable, not copyable.
class Timeline {
public:
	Timeline() = default;
	~Timeline();
	Timeline(Timeline&& other) noexcept;
	Timeline& operator=(Timeline&& other) noexcept;
	Timeline(const Timeline&) = delete;
	Timeline& operator=(const Timeline&) = delete;

	/// Checks `json` against spec/rules.md 10.1 in the table's order and compiles it into `out`
	/// (nullptr: check only). `image`: the image's timeline (`adjustable` allowed); else the
	/// inhabitant's (`adjustable` is an unknown member). False with `refusal` filled.
	static bool parse(const JsonValue& json, const TimelineStates& states, bool image, Timeline* out,
		TimelineRefusal& refusal);
	/// The same over a JSON text (an override from the store, a request body).
	static bool parse_text(const char* text, size_t length, const TimelineStates& states, bool image, Timeline* out,
		TimelineRefusal& refusal);

	bool empty() const { return block_ == nullptr; }
	bool adjustable() const { return adjustable_; }
	size_t plan_count() const { return plan_count_; }
	size_t point_count() const { return point_count_; }
	size_t assignment_count() const { return assignment_count_; }
	const TimelinePlan& plan(size_t i) const { return plans_[i]; }
	const TimelinePoint& point(size_t i) const { return points_[i]; }
	const TimelineAssignment& assignment(size_t i) const { return assignments_[i]; }
	const char* text(uint32_t offset) const { return text_ + offset; }
	/// Whether any point is a sun point.
	bool has_sun_points() const;

	/// The plan of a date of weekday `weekday` (1 Monday .. 7 Sunday): the matching assignment of the
	/// highest rank (spec 2.8).
	size_t plan_of(unsigned weekday) const;

	/// The canonical form (spec 8.7): `{"plans":[...],"assignments":[...]}`, with `adjustable` first
	/// when `with_adjustable`.
	void write(JsonWriter& json, const TimelineStates& states, bool with_adjustable) const;
	/// One plan in the canonical form.
	void write_plan(JsonWriter& json, const TimelineStates& states, size_t plan) const;
	/// The assignments array in the canonical form.
	void write_assignments(JsonWriter& json) const;
	/// The canonical text into a heap buffer (delete[]); nullptr without memory.
	char* canonical_text(const TimelineStates& states, size_t& length) const;

	/// Equal timelines (spec 8.7: member by member, an absent offset 0, days as sets); the states by
	/// index, so both against the same rule.
	bool equals(const Timeline& other) const;

	/// Bytes of the block.
	size_t memory() const { return block_size_; }

private:
	friend class TimelineParser;
	void* block_ = nullptr;
	size_t block_size_ = 0;
	TimelinePoint* points_ = nullptr;
	TimelinePlan* plans_ = nullptr;
	TimelineAssignment* assignments_ = nullptr;
	char* text_ = nullptr;
	size_t point_count_ = 0;
	size_t plan_count_ = 0;
	size_t assignment_count_ = 0;
	bool adjustable_ = true;
};

/// The outcome of a search over a timeline (spec 7.13).
struct TimelineFound {
	int64_t moment = 0; ///< UTC seconds.
	uint8_t state = 0;
	uint8_t plan = 0;
	uint8_t point = 0; ///< Index within the plan.
	/// A sun point had no moment because the device has no location (sunUnavailable).
	bool no_location = false;
};

/// The state due at the UTC instant `t` (spec 7.13 point 3): false when no point of today and the
/// seven dates before lies at or before `t`. `no_location` is reported in `found` either way.
bool timeline_due(const Timeline& timeline, const TimeSource& time, int64_t t, TimelineFound& found);

/// The first point whose moment lies after `after`, over the eight dates from the local date of
/// `after` (spec 7.13 point 2); false when there is none.
bool timeline_next(const Timeline& timeline, const TimeSource& time, int64_t after, TimelineFound& found);

/// The points with `after < moment <= until`, in order (moments, then plan order), whole dates at a
/// time: up to `capacity` (at least TimelineLimits::kPointsPerPlan) into `out`; returns the count. A
/// caller with more to come calls again after the last moment returned.
size_t timeline_between(const Timeline& timeline, const TimeSource& time, int64_t after, int64_t until,
	TimelineFound* out, size_t capacity, bool& no_location);

/// The points of the plan of the local date `days` (days since the epoch) with their moments,
/// sorted (spec 7.13 point 1): up to TimelineLimits::kPointsPerPlan into `out`; returns the count.
size_t timeline_points_of(const Timeline& timeline, const TimeSource& time, int64_t days, TimelineFound* out,
	bool& no_location);

} // namespace hc::rules

#endif // HC_RULES_TIMELINE_H
