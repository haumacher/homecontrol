/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The timeline of a rule with states (spec/rules.md 2.8, 7.13, 8.7; issue #84): the parser with the
// checks of spec 10.1 in the order of its table, the canonical form, and the moments of the points.
#include "hc/rules/timeline.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

#include "hc/json_document.h"
#include "hc/rules/engine.h"

namespace hc::rules {

namespace {

constexpr const char* kSunNames[] = {"dawn", "sunrise", "sunset", "dusk"};

bool valid_plan_id(const char* text) {
	size_t n = std::strlen(text);
	if (n < 1 || n > 24) {
		return false;
	}
	for (size_t i = 0; i < n; ++i) {
		char c = text[i];
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
			return false;
		}
	}
	return true;
}

bool valid_tag(const char* tag, size_t length) {
	if (length < 2 || length > 8) {
		return false;
	}
	for (size_t i = 0; i < length; ++i) {
		char c = tag[i];
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
			return false;
		}
	}
	return true;
}

/// An integer JSON number.
bool integer_of(const JsonValue& value, int64_t& out) {
	double d = 0;
	if (!value.get_number(d) || !std::isfinite(d) || d != std::floor(d) || std::fabs(d) > 9007199254740991.0) {
		return false;
	}
	out = static_cast<int64_t>(d);
	return true;
}

/// A time {"hour", "minute", "second"} (all three, nothing else) as seconds of the day.
bool time_of(const JsonValue& value, int32_t& seconds) {
	if (!value.is(JsonKind::object) || value.size() != 3) {
		return false;
	}
	JsonValue h, m, s;
	int64_t hour = 0, minute = 0, second = 0;
	if (!value.member("hour", h) || !value.member("minute", m) || !value.member("second", s) || !integer_of(h, hour) ||
		!integer_of(m, minute) || !integer_of(s, second) || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
		second < 0 || second > 59) {
		return false;
	}
	seconds = static_cast<int32_t>(hour * 3600 + minute * 60 + second);
	return true;
}

void write_time(JsonWriter& json, int32_t seconds) {
	json.begin_object()
		.member("hour", static_cast<int32_t>(seconds / 3600))
		.member("minute", static_cast<int32_t>(seconds % 3600 / 60))
		.member("second", static_cast<int32_t>(seconds % 60))
		.end_object();
}

const char* const kTimeShape = "must be a time {hour 0..23, minute 0..59, second 0..59}";

} // namespace

int TimelineStates::find(const char* id) const {
	for (size_t i = 0; i < count(); ++i) {
		if (std::strcmp(this->id(i), id) == 0) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

// ------------------------------------------------------------------------------------------
// The parser: two passes over the JSON with the same code, the first checks and counts, the
// second writes into the block sized by the first.

class TimelineParser {
public:
	TimelineParser(const TimelineStates& states, bool image, TimelineRefusal& refusal)
		: states_(states), image_(image), refusal_(refusal) {}

	bool run(const JsonValue& json, Timeline* out);

	size_t points = 0, plans = 0, assignments = 0, text = 0;

private:
	bool fail(LoadError error, const char* fmt, ...) HC_PRINTF_FORMAT(3, 4);
	bool only(const JsonValue& object, const char* const* allowed);
	bool plan(const JsonValue& json, size_t index);
	bool label(const JsonValue& json);
	bool point(const JsonValue& json, TimelinePoint& out);
	bool assignment(const JsonValue& json, size_t index, int& default_index, uint8_t& taken, uint8_t* owner);
	uint32_t add_text(const char* data, size_t length);

	struct Scope {
		Scope(TimelineParser& p, const char* fmt, ...) HC_PRINTF_FORMAT(3, 4) : parser(p), saved(std::strlen(p.path_)) {
			va_list args;
			va_start(args, fmt);
			std::vsnprintf(p.path_ + saved, sizeof(p.path_) - saved, fmt, args);
			va_end(args);
		}
		~Scope() { parser.path_[saved] = '\0'; }
		TimelineParser& parser;
		size_t saved;
	};

	const TimelineStates& states_;
	bool image_;
	TimelineRefusal& refusal_;
	Timeline* out_ = nullptr;
	char path_[sizeof(TimelineRefusal::path)] = {};
	char ids_[TimelineLimits::kPlans][25] = {};
};

bool TimelineParser::fail(LoadError error, const char* fmt, ...) {
	if (refusal_.error == LoadError::ok) {
		refusal_.error = error;
		// The path relative to the timeline, without a leading dot.
		std::snprintf(refusal_.path, sizeof(refusal_.path), "%s", path_[0] == '.' ? path_ + 1 : path_);
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(refusal_.text, sizeof(refusal_.text), fmt, args);
		va_end(args);
	}
	return false;
}

bool TimelineParser::only(const JsonValue& object, const char* const* allowed) {
	size_t position = 0;
	char name[40];
	size_t length = 0;
	JsonValue value;
	while (object.next_member(position, name, sizeof(name), length, value)) {
		bool known = length < sizeof(name) && (std::strcmp(name, "description") == 0 || std::strcmp(name, "comment") == 0);
		for (const char* const* a = allowed; !known && *a; ++a) {
			known = length < sizeof(name) && std::strcmp(name, *a) == 0;
		}
		if (!known) {
			return fail(LoadError::malformed, "unknown member '%s'", name);
		}
	}
	return true;
}

uint32_t TimelineParser::add_text(const char* data, size_t length) {
	uint32_t offset = static_cast<uint32_t>(text);
	if (out_ != nullptr) {
		std::memcpy(out_->text_ + offset, data, length);
		out_->text_[offset + length] = '\0';
	}
	text += length + 1;
	return offset;
}

bool TimelineParser::run(const JsonValue& json, Timeline* out) {
	out_ = out;
	points = plans = assignments = text = 0;
	path_[0] = '\0';
	if (!json.is(JsonKind::object)) {
		return fail(LoadError::malformed, "an object expected");
	}
	static const char* const kImageMembers[] = {"adjustable", "plans", "assignments", nullptr};
	static const char* const kInhabitantMembers[] = {"plans", "assignments", nullptr};
	if (!only(json, image_ ? kImageMembers : kInhabitantMembers)) {
		return false;
	}
	bool adjustable = true;
	JsonValue member;
	if (image_ && json.member("adjustable", member)) {
		Scope scope(*this, ".adjustable");
		if (!member.get_bool(adjustable)) {
			return fail(LoadError::malformed, "'adjustable' must be a boolean");
		}
	}
	if (out_ != nullptr) {
		out_->adjustable_ = adjustable;
	}
	JsonValue plan_list;
	if (!json.member("plans", plan_list)) {
		return fail(LoadError::malformed, "member 'plans' missing");
	}
	{
		Scope scope(*this, ".plans");
		if (!plan_list.is(JsonKind::array)) {
			return fail(LoadError::malformed, "'plans' must be an array");
		}
		size_t count = plan_list.size();
		if (count == 0) {
			return fail(LoadError::malformed, "a timeline needs at least one plan");
		}
		if (count > TimelineLimits::kPlans) {
			return fail(LoadError::limit_exceeded, "more than %u plans", static_cast<unsigned>(TimelineLimits::kPlans));
		}
		size_t position = 0;
		JsonValue item;
		size_t i = 0;
		while (plan_list.next_item(position, item)) {
			Scope item_scope(*this, "[%u]", static_cast<unsigned>(i));
			if (!plan(item, i)) {
				return false;
			}
			++i;
		}
	}
	if (points > TimelineLimits::kPoints) {
		return fail(LoadError::limit_exceeded, "more than %u points in a timeline",
			static_cast<unsigned>(TimelineLimits::kPoints));
	}
	JsonValue assignment_list;
	if (!json.member("assignments", assignment_list)) {
		return fail(LoadError::malformed, "member 'assignments' missing");
	}
	Scope scope(*this, ".assignments");
	if (!assignment_list.is(JsonKind::array)) {
		return fail(LoadError::malformed, "'assignments' must be an array");
	}
	if (assignment_list.size() > TimelineLimits::kAssignments) {
		return fail(LoadError::limit_exceeded, "more than %u assignments",
			static_cast<unsigned>(TimelineLimits::kAssignments));
	}
	int default_index = -1;
	uint8_t taken = 0;
	uint8_t owner[7] = {};
	size_t position = 0;
	JsonValue item;
	size_t i = 0;
	while (assignment_list.next_item(position, item)) {
		Scope item_scope(*this, "[%u]", static_cast<unsigned>(i));
		if (!assignment(item, i, default_index, taken, owner)) {
			return false;
		}
		++i;
	}
	if (default_index < 0) {
		return fail(LoadError::malformed, "no default assignment (one without 'days')");
	}
	return true;
}

bool TimelineParser::plan(const JsonValue& json, size_t index) {
	if (!json.is(JsonKind::object)) {
		return fail(LoadError::malformed, "an object expected");
	}
	static const char* const kMembers[] = {"id", "label", "points", nullptr};
	if (!only(json, kMembers)) {
		return false;
	}
	JsonValue member;
	if (!json.member("id", member)) {
		return fail(LoadError::malformed, "member 'id' missing");
	}
	char id[64];
	{
		Scope scope(*this, ".id");
		size_t length = 0;
		if (!member.is(JsonKind::string)) {
			return fail(LoadError::malformed, "'id' must be a string");
		}
		if (!member.get_string(id, sizeof(id), &length)) {
			return fail(LoadError::malformed, "plan id is not 1..24 characters of A-Z a-z 0-9 _ -");
		}
		if (!valid_plan_id(id)) {
			return fail(LoadError::malformed, "plan id '%s' is not 1..24 characters of A-Z a-z 0-9 _ -", id);
		}
		for (size_t k = 0; k < index; ++k) {
			if (std::strcmp(ids_[k], id) == 0) {
				return fail(LoadError::duplicate_id, "plan id '%s' is used twice", id);
			}
		}
		std::memcpy(ids_[index], id, std::strlen(id) + 1);
	}
	TimelinePlan p{};
	p.id = add_text(id, std::strlen(id));
	uint32_t label_offset = static_cast<uint32_t>(text);
	if (!label(json)) {
		return false;
	}
	p.label = label_offset;
	JsonValue list;
	if (!json.member("points", list)) {
		return fail(LoadError::malformed, "member 'points' missing");
	}
	Scope scope(*this, ".points");
	if (!list.is(JsonKind::array)) {
		return fail(LoadError::malformed, "'points' must be an array");
	}
	if (list.size() > TimelineLimits::kPointsPerPlan) {
		return fail(LoadError::limit_exceeded, "more than %u points in a plan",
			static_cast<unsigned>(TimelineLimits::kPointsPerPlan));
	}
	p.first_point = static_cast<uint16_t>(points);
	size_t position = 0;
	JsonValue item;
	size_t k = 0;
	while (list.next_item(position, item)) {
		Scope item_scope(*this, "[%u]", static_cast<unsigned>(k));
		TimelinePoint point_value{};
		if (!point(item, point_value)) {
			return false;
		}
		if (out_ != nullptr && points < out_->point_count_) {
			out_->points_[points] = point_value;
		}
		++points;
		++k;
	}
	p.point_count = static_cast<uint8_t>(k);
	if (out_ != nullptr) {
		out_->plans_[plans] = p;
	}
	++plans;
	return true;
}

bool TimelineParser::label(const JsonValue& json) {
	JsonValue member;
	if (!json.member("label", member)) {
		return fail(LoadError::malformed, "member 'label' missing");
	}
	Scope scope(*this, ".label");
	char buffer[Limits::kLabel + 1];
	size_t length = 0;
	// The canonical JSON of the label, written by JsonWriter (counting in the first pass).
	char* target = out_ != nullptr ? out_->text_ + text : nullptr;
	size_t room = out_ != nullptr ? out_->block_size_ : 0; // bounded by the first pass's count
	JsonWriter writer = target != nullptr ? JsonWriter(target, room) : JsonWriter::counter();
	if (member.is(JsonKind::string)) {
		if (!member.get_string(buffer, sizeof(buffer), &length) || length < 1 || length > Limits::kLabel) {
			return fail(LoadError::malformed, "'label' must have 1..%u bytes", static_cast<unsigned>(Limits::kLabel));
		}
		writer.string(buffer, length);
	} else if (member.is(JsonKind::object)) {
		size_t position = 0;
		char tag[16];
		size_t tag_length = 0;
		JsonValue value;
		size_t entries = 0;
		writer.begin_object();
		while (member.next_member(position, tag, sizeof(tag), tag_length, value)) {
			if (tag_length >= sizeof(tag) || !valid_tag(tag, tag_length)) {
				return fail(LoadError::malformed, "a language tag of 'label' must have 2..8 characters of A-Z a-z 0-9 -");
			}
			Scope tag_scope(*this, ".%s", tag);
			if (entries >= TimelineLimits::kPlanLabelLanguages) {
				return fail(LoadError::limit_exceeded, "more than %u languages in a plan's label",
					static_cast<unsigned>(TimelineLimits::kPlanLabelLanguages));
			}
			if (!value.is(JsonKind::string) || !value.get_string(buffer, sizeof(buffer), &length) || length < 1 ||
				length > Limits::kLabel) {
				return fail(LoadError::malformed, "a label text must be a string of 1..%u bytes",
					static_cast<unsigned>(Limits::kLabel));
			}
			writer.key(tag).string(buffer, length);
			++entries;
		}
		if (entries == 0) {
			return fail(LoadError::malformed, "'label' names no language");
		}
		writer.end_object();
	} else {
		return fail(LoadError::malformed, "'label' must be a string or an object of language tags to strings");
	}
	size_t written = writer.length();
	if (target != nullptr) {
		target[written] = '\0';
	}
	text += written + 1;
	return true;
}

bool TimelineParser::point(const JsonValue& json, TimelinePoint& out) {
	if (!json.is(JsonKind::object)) {
		return fail(LoadError::malformed, "an object expected");
	}
	static const char* const kMembers[] = {"time", "sun", "offset", "notBefore", "notAfter", "state", nullptr};
	if (!only(json, kMembers)) {
		return false;
	}
	JsonValue time, sun, member;
	bool has_time = json.member("time", time);
	bool has_sun = json.member("sun", sun);
	if (!has_time && !has_sun) {
		return fail(LoadError::malformed, "a point needs 'time' or 'sun'");
	}
	if (has_time && has_sun) {
		return fail(LoadError::malformed, "a point has 'time' or 'sun', not both");
	}
	out = TimelinePoint{};
	if (has_time) {
		Scope scope(*this, ".time");
		if (!time_of(time, out.seconds)) {
			return fail(LoadError::malformed, "'time' %s", kTimeShape);
		}
	} else {
		Scope scope(*this, ".sun");
		char name[16];
		size_t length = 0;
		if (!sun.is(JsonKind::string)) {
			return fail(LoadError::malformed, "'sun' must be a string");
		}
		if (!sun.get_string(name, sizeof(name), &length)) {
			return fail(LoadError::malformed, "unknown sun event (dawn, sunrise, sunset, dusk)");
		}
		int found = -1;
		for (size_t i = 0; i < 4; ++i) {
			if (std::strcmp(name, kSunNames[i]) == 0) {
				found = static_cast<int>(i);
			}
		}
		if (found < 0) {
			return fail(LoadError::malformed, "unknown sun event '%s' (dawn, sunrise, sunset, dusk)", name);
		}
		out.sun = static_cast<uint8_t>(found + 1);
	}
	static const char* const kSunOnly[] = {"offset", "notBefore", "notAfter"};
	if (has_time) {
		for (const char* name : kSunOnly) {
			if (json.member(name, member)) {
				Scope scope(*this, ".%s", name);
				return fail(LoadError::malformed, "'%s' is only for a sun point", name);
			}
		}
	} else {
		if (json.member("offset", member)) {
			Scope scope(*this, ".offset");
			int64_t ms = 0;
			if (!integer_of(member, ms) || ms % 1000 != 0 || ms < -43200000 || ms > 43200000) {
				return fail(LoadError::malformed, "an offset must be whole seconds within -12 h..12 h");
			}
			out.seconds = static_cast<int32_t>(ms / 1000);
		}
		static const char* const kClamps[] = {"notBefore", "notAfter"};
		for (size_t c = 0; c < 2; ++c) {
			if (json.member(kClamps[c], member)) {
				Scope scope(*this, ".%s", kClamps[c]);
				int32_t seconds = 0;
				if (!time_of(member, seconds)) {
					return fail(LoadError::malformed, "'%s' %s", kClamps[c], kTimeShape);
				}
				if (c == 0) {
					out.not_before = seconds;
					out.flags |= TimelinePoint::kNotBefore;
				} else {
					out.not_after = seconds;
					out.flags |= TimelinePoint::kNotAfter;
				}
			}
		}
		if ((out.flags & TimelinePoint::kNotBefore) != 0 && (out.flags & TimelinePoint::kNotAfter) != 0 &&
			out.not_before > out.not_after) {
			return fail(LoadError::malformed, "notBefore %02d:%02d:%02d is after notAfter %02d:%02d:%02d",
				static_cast<int>(out.not_before / 3600), static_cast<int>(out.not_before % 3600 / 60),
				static_cast<int>(out.not_before % 60), static_cast<int>(out.not_after / 3600),
				static_cast<int>(out.not_after % 3600 / 60), static_cast<int>(out.not_after % 60));
		}
	}
	if (!json.member("state", member)) {
		return fail(LoadError::malformed, "member 'state' missing");
	}
	Scope scope(*this, ".state");
	char state[40];
	if (!member.is(JsonKind::string)) {
		return fail(LoadError::malformed, "'state' must be a string");
	}
	if (!member.get_string(state, sizeof(state))) {
		return fail(LoadError::unknown_reference, "no such state in the rule");
	}
	int s = states_.find(state);
	if (s < 0) {
		return fail(LoadError::unknown_reference, "no state '%s' in the rule", state);
	}
	out.state = static_cast<uint8_t>(s);
	return true;
}

bool TimelineParser::assignment(const JsonValue& json, size_t index, int& default_index, uint8_t& taken,
	uint8_t* owner) {
	if (!json.is(JsonKind::object)) {
		return fail(LoadError::malformed, "an object expected");
	}
	static const char* const kMembers[] = {"days", "plan", nullptr};
	if (!only(json, kMembers)) {
		return false;
	}
	JsonValue member;
	if (!json.member("plan", member)) {
		return fail(LoadError::malformed, "member 'plan' missing");
	}
	TimelineAssignment a{};
	{
		Scope scope(*this, ".plan");
		char id[40];
		if (!member.is(JsonKind::string)) {
			return fail(LoadError::malformed, "'plan' must be a string");
		}
		if (!member.get_string(id, sizeof(id))) {
			return fail(LoadError::unknown_reference, "no such plan in the timeline");
		}
		int found = -1;
		for (size_t k = 0; k < plans && found < 0; ++k) {
			if (std::strcmp(ids_[k], id) == 0) {
				found = static_cast<int>(k);
			}
		}
		if (found < 0) {
			return fail(LoadError::unknown_reference, "no plan '%s' in the timeline", id);
		}
		a.plan = static_cast<uint8_t>(found);
	}
	JsonValue days;
	if (json.member("days", days)) {
		Scope scope(*this, ".days");
		if (!days.is(JsonKind::array) || days.size() == 0) {
			return fail(LoadError::malformed, "'days' must be a non-empty array of weekdays 1..7");
		}
		size_t position = 0;
		JsonValue item;
		size_t d = 0;
		uint8_t mine = 0;
		while (days.next_item(position, item)) {
			Scope item_scope(*this, "[%u]", static_cast<unsigned>(d++));
			int64_t day = 0;
			double number = 0;
			if (!integer_of(item, day) || day < 1 || day > 7) {
				if (item.get_number(number)) {
					return fail(LoadError::malformed, "weekday %g is not 1..7", number);
				}
				return fail(LoadError::malformed, "weekday is not 1..7");
			}
			uint8_t bit = static_cast<uint8_t>(1u << (day - 1));
			if ((mine & bit) != 0) {
				return fail(LoadError::malformed, "weekday %d is listed twice", static_cast<int>(day));
			}
			if ((taken & bit) != 0) {
				return fail(LoadError::malformed, "weekday %d is assigned by assignments[%u] already", static_cast<int>(day),
					static_cast<unsigned>(owner[day - 1]));
			}
			mine = static_cast<uint8_t>(mine | bit);
		}
		for (int k = 0; k < 7; ++k) {
			if ((mine & (1u << k)) != 0) {
				owner[k] = static_cast<uint8_t>(index);
			}
		}
		taken = static_cast<uint8_t>(taken | mine);
		a.days = mine;
	} else {
		if (default_index >= 0) {
			return fail(LoadError::malformed, "a second default assignment (the first is assignments[%d])", default_index);
		}
		default_index = static_cast<int>(index);
	}
	if (out_ != nullptr) {
		out_->assignments_[assignments] = a;
	}
	++assignments;
	return true;
}

// ------------------------------------------------------------------------------------------
// Timeline

Timeline::~Timeline() { ::operator delete(block_, std::nothrow); }

Timeline::Timeline(Timeline&& other) noexcept { *this = std::move(other); }

Timeline& Timeline::operator=(Timeline&& other) noexcept {
	if (this != &other) {
		::operator delete(block_, std::nothrow);
		block_ = other.block_;
		block_size_ = other.block_size_;
		points_ = other.points_;
		plans_ = other.plans_;
		assignments_ = other.assignments_;
		text_ = other.text_;
		point_count_ = other.point_count_;
		plan_count_ = other.plan_count_;
		assignment_count_ = other.assignment_count_;
		adjustable_ = other.adjustable_;
		other.block_ = nullptr;
		other.block_size_ = 0;
		other.points_ = nullptr;
		other.plans_ = nullptr;
		other.assignments_ = nullptr;
		other.text_ = nullptr;
		other.point_count_ = other.plan_count_ = other.assignment_count_ = 0;
		other.adjustable_ = true;
	}
	return *this;
}

bool Timeline::parse(const JsonValue& json, const TimelineStates& states, bool image, Timeline* out,
	TimelineRefusal& refusal) {
	refusal = TimelineRefusal();
	TimelineParser parser(states, image, refusal);
	if (!parser.run(json, nullptr)) {
		return false;
	}
	if (out == nullptr) {
		return true;
	}
	size_t points_bytes = parser.points * sizeof(TimelinePoint);
	size_t plans_bytes = parser.plans * sizeof(TimelinePlan);
	size_t assignments_bytes = parser.assignments * sizeof(TimelineAssignment);
	// Align the parts for their types: points (4), plans (4), assignments (1), text (1).
	size_t size = points_bytes + plans_bytes + assignments_bytes + parser.text;
	void* block = ::operator new(size == 0 ? 1 : size, std::nothrow);
	if (block == nullptr) {
		refusal.error = LoadError::out_of_memory;
		std::snprintf(refusal.text, sizeof(refusal.text), "no memory for the timeline (%u bytes)",
			static_cast<unsigned>(size));
		return false;
	}
	std::memset(block, 0, size == 0 ? 1 : size);
	Timeline next;
	next.block_ = block;
	next.block_size_ = size;
	char* p = static_cast<char*>(block);
	next.points_ = reinterpret_cast<TimelinePoint*>(p);
	next.plans_ = reinterpret_cast<TimelinePlan*>(p + points_bytes);
	next.assignments_ = reinterpret_cast<TimelineAssignment*>(p + points_bytes + plans_bytes);
	next.text_ = p + points_bytes + plans_bytes + assignments_bytes;
	next.point_count_ = parser.points;
	next.plan_count_ = parser.plans;
	next.assignment_count_ = parser.assignments;
	// The label writer may use what the first pass counted: the room is the text's.
	next.block_size_ = parser.text;
	TimelineRefusal second;
	TimelineParser filler(states, image, second);
	bool ok = filler.run(json, &next);
	next.block_size_ = size;
	if (!ok) {
		refusal = second;
		return false;
	}
	*out = std::move(next);
	return true;
}

bool Timeline::parse_text(const char* text, size_t length, const TimelineStates& states, bool image, Timeline* out,
	TimelineRefusal& refusal) {
	JsonSpan span;
	const char* error = nullptr;
	if (!json_parse(text, length, span, &error, 16)) {
		refusal = TimelineRefusal();
		refusal.error = LoadError::malformed;
		std::snprintf(refusal.text, sizeof(refusal.text), "not JSON: %s", error != nullptr ? error : "?");
		return false;
	}
	JsonSpanDocument document;
	return parse(document.value(span), states, image, out, refusal);
}

bool Timeline::has_sun_points() const {
	for (size_t i = 0; i < point_count_; ++i) {
		if (points_[i].is_sun()) {
			return true;
		}
	}
	return false;
}

size_t Timeline::plan_of(unsigned weekday) const {
	size_t fallback = 0;
	for (size_t i = 0; i < assignment_count_; ++i) {
		const TimelineAssignment& a = assignments_[i];
		if (a.days == 0) {
			fallback = a.plan;
		} else if (weekday >= 1 && weekday <= 7 && (a.days & (1u << (weekday - 1))) != 0) {
			return a.plan; // rank 1 beats the default; no overlap within the rank
		}
	}
	return fallback;
}

void Timeline::write_plan(JsonWriter& json, const TimelineStates& states, size_t index) const {
	const TimelinePlan& p = plans_[index];
	json.begin_object().member("id", text(p.id)).key("label");
	json.raw(text(p.label), std::strlen(text(p.label)));
	json.key("points").begin_array();
	for (size_t k = 0; k < p.point_count; ++k) {
		const TimelinePoint& pt = points_[p.first_point + k];
		json.begin_object();
		if (!pt.is_sun()) {
			json.key("time");
			write_time(json, pt.seconds);
		} else {
			json.member("sun", kSunNames[pt.sun - 1]);
			if (pt.seconds != 0) {
				json.member("offset", static_cast<int64_t>(pt.seconds) * 1000);
			}
			if ((pt.flags & TimelinePoint::kNotBefore) != 0) {
				json.key("notBefore");
				write_time(json, pt.not_before);
			}
			if ((pt.flags & TimelinePoint::kNotAfter) != 0) {
				json.key("notAfter");
				write_time(json, pt.not_after);
			}
		}
		json.member("state", pt.state < states.count() ? states.id(pt.state) : "?");
		json.end_object();
	}
	json.end_array().end_object();
}

void Timeline::write_assignments(JsonWriter& json) const {
	json.begin_array();
	for (size_t i = 0; i < assignment_count_; ++i) {
		const TimelineAssignment& a = assignments_[i];
		json.begin_object();
		if (a.days != 0) {
			json.key("days").begin_array();
			for (int d = 0; d < 7; ++d) {
				if ((a.days & (1u << d)) != 0) {
					json.number(static_cast<int32_t>(d + 1));
				}
			}
			json.end_array();
		}
		json.member("plan", text(plans_[a.plan].id));
		json.end_object();
	}
	json.end_array();
}

void Timeline::write(JsonWriter& json, const TimelineStates& states, bool with_adjustable) const {
	json.begin_object();
	if (with_adjustable) {
		json.member("adjustable", adjustable_);
	}
	json.key("plans").begin_array();
	for (size_t i = 0; i < plan_count_; ++i) {
		write_plan(json, states, i);
	}
	json.end_array();
	json.key("assignments");
	write_assignments(json);
	json.end_object();
}

char* Timeline::canonical_text(const TimelineStates& states, size_t& length) const {
	JsonWriter counter = JsonWriter::counter();
	write(counter, states, false);
	length = counter.length();
	char* buffer = new (std::nothrow) char[length + 1];
	if (buffer == nullptr) {
		return nullptr;
	}
	JsonWriter json(buffer, length + 1);
	write(json, states, false);
	buffer[length] = '\0';
	return buffer;
}

bool Timeline::equals(const Timeline& other) const {
	if (plan_count_ != other.plan_count_ || point_count_ != other.point_count_ ||
		assignment_count_ != other.assignment_count_) {
		return false;
	}
	for (size_t i = 0; i < plan_count_; ++i) {
		const TimelinePlan& a = plans_[i];
		const TimelinePlan& b = other.plans_[i];
		if (a.point_count != b.point_count || std::strcmp(text(a.id), other.text(b.id)) != 0 ||
			std::strcmp(text(a.label), other.text(b.label)) != 0) {
			return false;
		}
		for (size_t k = 0; k < a.point_count; ++k) {
			const TimelinePoint& p = points_[a.first_point + k];
			const TimelinePoint& q = other.points_[b.first_point + k];
			if (p.state != q.state || p.sun != q.sun || p.flags != q.flags || p.seconds != q.seconds ||
				((p.flags & TimelinePoint::kNotBefore) != 0 && p.not_before != q.not_before) ||
				((p.flags & TimelinePoint::kNotAfter) != 0 && p.not_after != q.not_after)) {
				return false;
			}
		}
	}
	for (size_t i = 0; i < assignment_count_; ++i) {
		if (assignments_[i].plan != other.assignments_[i].plan || assignments_[i].days != other.assignments_[i].days) {
			return false;
		}
	}
	return true;
}

// ------------------------------------------------------------------------------------------
// Moments (spec 7.13)

size_t timeline_points_of(const Timeline& timeline, const TimeSource& time, int64_t days, TimelineFound* out,
	bool& no_location) {
	if (timeline.plan_count() == 0) {
		return 0;
	}
	int32_t year = 0;
	unsigned month = 0, day = 0;
	civil_of(days, year, month, day);
	int32_t next_year = 0;
	unsigned next_month = 0, next_day = 0;
	civil_of(days + 1, next_year, next_month, next_day);
	size_t plan_index = timeline.plan_of(weekday_of(year, month, day));
	const TimelinePlan& plan = timeline.plan(plan_index);
	int64_t day_start = time.civil_to_utc(year, month, day, 0, 0, 0);
	int64_t day_end = time.civil_to_utc(next_year, next_month, next_day, 0, 0, 0) - 1;
	size_t n = 0;
	for (size_t k = 0; k < plan.point_count; ++k) {
		const TimelinePoint& p = timeline.point(plan.first_point + k);
		int64_t moment = 0;
		if (!p.is_sun()) {
			moment = time.civil_to_utc(year, month, day, static_cast<unsigned>(p.seconds / 3600),
				static_cast<unsigned>(p.seconds % 3600 / 60), static_cast<unsigned>(p.seconds % 60));
		} else {
			int64_t event = 0;
			TimeSource::SunLookup lookup = time.sun_event_utc(year, month, day, p.event(), event);
			if (lookup == TimeSource::SunLookup::no_location) {
				no_location = true;
				continue;
			}
			if (lookup != TimeSource::SunLookup::ok) {
				continue;
			}
			moment = event + p.seconds;
			if ((p.flags & TimelinePoint::kNotBefore) != 0) {
				int64_t low = time.civil_to_utc(year, month, day, static_cast<unsigned>(p.not_before / 3600),
					static_cast<unsigned>(p.not_before % 3600 / 60), static_cast<unsigned>(p.not_before % 60));
				if (moment < low) {
					moment = low;
				}
			}
			if ((p.flags & TimelinePoint::kNotAfter) != 0) {
				int64_t high = time.civil_to_utc(year, month, day, static_cast<unsigned>(p.not_after / 3600),
					static_cast<unsigned>(p.not_after % 3600 / 60), static_cast<unsigned>(p.not_after % 60));
				if (moment > high) {
					moment = high;
				}
			}
		}
		// Kept within its date (spec 7.13 point 1).
		if (moment < day_start) {
			moment = day_start;
		}
		if (moment > day_end) {
			moment = day_end;
		}
		// Insertion in moment order; equal moments keep plan order.
		size_t at = n;
		while (at > 0 && out[at - 1].moment > moment) {
			out[at] = out[at - 1];
			--at;
		}
		out[at] = TimelineFound{};
		out[at].moment = moment;
		out[at].state = p.state;
		out[at].plan = static_cast<uint8_t>(plan_index);
		out[at].point = static_cast<uint8_t>(k);
		++n;
	}
	return n;
}

namespace {

bool local_days(const TimeSource& time, int64_t utc, int64_t& days) {
	DateTime local;
	if (!time.utc_to_local(utc, local)) {
		return false;
	}
	days = days_of(local.year, local.month, local.day);
	return true;
}

} // namespace

bool timeline_due(const Timeline& timeline, const TimeSource& time, int64_t t, TimelineFound& found) {
	int64_t today = 0;
	found = TimelineFound{};
	if (!local_days(time, t, today)) {
		return false;
	}
	TimelineFound day[TimelineLimits::kPointsPerPlan];
	for (int k = 0; k <= TimelineLimits::kSearchDays; ++k) {
		bool no_location = false;
		size_t n = timeline_points_of(timeline, time, today - k, day, no_location);
		found.no_location = found.no_location || no_location;
		for (size_t i = n; i > 0; --i) {
			if (day[i - 1].moment <= t) {
				bool flag = found.no_location;
				found = day[i - 1];
				found.no_location = flag;
				return true;
			}
		}
	}
	return false;
}

bool timeline_next(const Timeline& timeline, const TimeSource& time, int64_t after, TimelineFound& found) {
	int64_t today = 0;
	found = TimelineFound{};
	if (!local_days(time, after, today)) {
		return false;
	}
	TimelineFound day[TimelineLimits::kPointsPerPlan];
	for (int k = 0; k <= TimelineLimits::kSearchDays; ++k) {
		bool no_location = false;
		size_t n = timeline_points_of(timeline, time, today + k, day, no_location);
		found.no_location = found.no_location || no_location;
		for (size_t i = 0; i < n; ++i) {
			if (day[i].moment > after) {
				bool flag = found.no_location;
				found = day[i];
				found.no_location = flag;
				return true;
			}
		}
	}
	return false;
}

size_t timeline_between(const Timeline& timeline, const TimeSource& time, int64_t after, int64_t until,
	TimelineFound* out, size_t capacity, bool& no_location) {
	int64_t first = 0, last = 0;
	if (until <= after || !local_days(time, after, first) || !local_days(time, until, last)) {
		return 0;
	}
	size_t count = 0;
	TimelineFound day[TimelineLimits::kPointsPerPlan];
	for (int64_t d = first; d <= last && d - first < 400; ++d) {
		size_t n = timeline_points_of(timeline, time, d, day, no_location);
		size_t take = 0;
		for (size_t i = 0; i < n; ++i) {
			if (day[i].moment > after && day[i].moment <= until) {
				++take;
			}
		}
		if (count + take > capacity) {
			break; // whole dates only: the caller continues after the last moment
		}
		for (size_t i = 0; i < n; ++i) {
			if (day[i].moment > after && day[i].moment <= until) {
				out[count++] = day[i];
			}
		}
	}
	return count;
}

} // namespace hc::rules
