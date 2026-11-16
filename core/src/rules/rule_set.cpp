/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The rules of the loaded binary image (spec/runtime-image.md 9.13, 9.14; issue #87): the records,
// nodes and constants are read in place; bind() derives what the engine needs beyond them.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

#include "hc/log.h"
#include "hc/rules/parameter_store.h"
#include "hc/rules/rule_set.h"
#include "hc/rules/timeline.h"

namespace hc::rules {

namespace {

constexpr const char* kTag = "hc.rule";

constexpr const char* kFunctionNames[] = {
	"if", "known", "coalesce", "between", "hysteresis",
	"abs", "min", "max", "clamp", "round", "floor", "ceil", "trunc", "roundTo", "toInt",
	"now", "today", "time", "weekday", "hour", "minute", "second", "year", "month", "day", "dateOf", "timeOf",
	"inWindow", "sunrise", "sunset", "dawn", "dusk", "sunAzimuth", "sunElevation",
	"sinceUpdate", "sinceChange", "changedWithin", "lastKnown", "quality",
	"running", "remaining", "state",
	"length", "contains", "startsWith", "endsWith", "toString",
	"rgb", "rgbw", "red", "green", "blue", "white", "controlled", "controlBit", "controlValue",
};
static_assert(sizeof(kFunctionNames) / sizeof(kFunctionNames[0]) == static_cast<size_t>(Fn::count),
	"a name for every function");

constexpr const char* kOperatorTexts[] = {"-", "not", "or", "and", "==", "!=", "<", "<=", ">", ">=", "+", "-", "*",
	"/", "%"};

constexpr const char* kLoadErrorNames[] = {"ok", "malformed", "duplicateId", "unknownReference", "typeError",
	"invalidLiteral", "invalidParameter", "notWritable", "notReadable", "forbidden", "limitExceeded", "outOfMemory"};

/// Checks the storage keys of every parameter and rule state (ParameterStore, spec 8.4, 8.5): two
/// that hash alike refuse the set (spec/runtime-image.md 10.8).
bool check_storage_keys(const RuleSet& set, LoadResult& result) {
	struct Entry {
		uint64_t hash;
		uint16_t rule;
		uint16_t parameter; ///< kNone: the rule's state.
	};
	size_t count = set.parameter_count();
	for (size_t r = 0; r < set.rule_count(); ++r) {
		count += set.rule(r).state_count > 0 ? 1 : 0;
	}
	if (count < 2) {
		return true;
	}
	Entry* entries = new (std::nothrow) Entry[count];
	if (entries == nullptr) {
		result.error = LoadError::out_of_memory;
		std::snprintf(result.text, sizeof(result.text), "no memory to check the storage keys");
		Log::error(kTag, "rules refused: %s", result.text);
		return false;
	}
	size_t n = 0;
	for (size_t r = 0; r < set.rule_count(); ++r) {
		const Rule& rule = set.rule(r);
		for (size_t p = 0; p < rule.parameter_count; ++p) {
			entries[n++] = Entry{ParameterStore::hash(set.str(rule.id), set.str(set.parameter(rule.first_parameter + p).id)),
				static_cast<uint16_t>(r), static_cast<uint16_t>(rule.first_parameter + p)};
		}
		if (rule.state_count > 0) {
			entries[n++] = Entry{ParameterStore::hash(set.str(rule.id), ParameterStore::kStateKey), static_cast<uint16_t>(r),
				kNone};
		}
	}
	std::sort(entries, entries + n, [](const Entry& a, const Entry& b) { return a.hash < b.hash; });
	bool ok = true;
	for (size_t i = 1; i < n && ok; ++i) {
		if (entries[i].hash == entries[i - 1].hash) {
			const Entry& a = entries[i - 1];
			const Entry& b = entries[i];
			auto key = [&set](const Entry& e) {
				return e.parameter == kNone ? ParameterStore::kStateKey : set.str(set.parameter(e.parameter).id);
			};
			result.error = LoadError::limit_exceeded;
			std::snprintf(result.rule, sizeof(result.rule), "%s", set.str(set.rule(b.rule).id));
			std::snprintf(result.path, sizeof(result.path), "rules[%u]", static_cast<unsigned>(b.rule));
			std::snprintf(result.text, sizeof(result.text), "the persistence keys of '%s/%s' and '%s/%s' collide in the device's store",
				set.str(set.rule(a.rule).id), key(a), set.str(set.rule(b.rule).id), key(b));
			ok = false;
		}
	}
	delete[] entries;
	if (!ok) {
		Log::error(kTag, "rules refused: rule '%s' at %s: %s: %s", result.rule, result.path,
			load_error_name(result.error), result.text);
	}
	return ok;
}

} // namespace

const char* function_name(Fn fn) {
	return static_cast<size_t>(fn) < static_cast<size_t>(Fn::count) ? kFunctionNames[static_cast<size_t>(fn)] : "?";
}

const char* operator_text(Op op) {
	return static_cast<size_t>(op) < sizeof(kOperatorTexts) / sizeof(kOperatorTexts[0])
		? kOperatorTexts[static_cast<size_t>(op)]
		: "?";
}

const char* load_error_name(LoadError error) {
	return static_cast<size_t>(error) < sizeof(kLoadErrorNames) / sizeof(kLoadErrorNames[0])
		? kLoadErrorNames[static_cast<size_t>(error)]
		: "?";
}

bool parameter_datapoint_name(const char* rule, const char* parameter, char* out, size_t size) {
	int n = std::snprintf(out, size, "%s%s/%s", kParameterDatapointPrefix, rule, parameter);
	return n > 0 && static_cast<size_t>(n) < size && static_cast<size_t>(n) <= Datapoint::kMaxName;
}

ValueType parameter_datapoint_type(Type type) {
	switch (type) {
		case Type::boolean: return ValueType::boolean;
		case Type::integer: return ValueType::signed_int;
		case Type::real: return ValueType::real;
		case Type::string: return ValueType::string;
		case Type::duration: return ValueType::unsigned_int;
		case Type::time_of_day: return ValueType::time_of_day;
		case Type::enumeration: return ValueType::enumeration;
		case Type::color: return ValueType::color;
		default: return ValueType::none;
	}
}

// ------------------------------------------------------------------------------------------
// RuleSet

RuleSet::~RuleSet() { clear(); }

RuleSet::RuleSet(RuleSet&& other) noexcept { *this = std::move(other); }

RuleSet& RuleSet::operator=(RuleSet&& other) noexcept {
	if (this != &other) {
		clear();
		std::memcpy(static_cast<void*>(this), static_cast<const void*>(&other), sizeof(RuleSet));
		new (&other) RuleSet();
	}
	return *this;
}

void RuleSet::clear() {
	delete[] timelines_;
	::operator delete(block_, std::nothrow);
	new (this) RuleSet();
}

LoadResult RuleSet::bind(const image::bin::View& view, const char* language) {
	using namespace image::bin;
	LoadResult result;
	RuleSet next;
	next.view_ = view;
	std::snprintf(next.language_, sizeof(next.language_), "%s", language != nullptr ? language : "en");
	next.rules_ = view.table(kRules);
	next.reactions_ = view.table(kRuleReactions);
	next.triggers_ = view.table(kRuleTriggers);
	next.actions_ = view.table(kRuleActions);
	next.log_parts_ = view.table(kRuleLogParts);
	next.parameters_ = view.table(kRuleParameters);
	next.choices_ = view.table(kRuleChoices);
	next.timers_ = view.table(kRuleTimers);
	next.states_ = view.table(kRuleStates);
	next.nodes_ = view.table(kNodes);
	next.constants_ = view.table(kConstants);
	next.rule_count_ = next.rules_.count;

	// Count what the derived tables need: the matches and silences by trigger, the tracked datapoints
	// by the history functions' first argument (an upper bound; duplicates are dropped below).
	size_t matches = 0, silences = 0, tracked = 0;
	for (size_t t = 0; t < next.triggers_.count; ++t) {
		TriggerKind kind = next.trigger(t).kind;
		matches += kind == TriggerKind::match ? 1 : 0;
		silences += kind == TriggerKind::silence ? 1 : 0;
	}
	for (size_t n = 0; n < next.nodes_.count; ++n) {
		const Node& node = next.node(n);
		if (node.kind == NodeKind::call &&
			(static_cast<Fn>(node.op) == Fn::since_change || static_cast<Fn>(node.op) == Fn::changed_within)) {
			++tracked;
		}
	}
	size_t tracked_bytes = (tracked * sizeof(uint16_t) + 7) & ~size_t(7);
	size_t size = tracked_bytes + matches * sizeof(MatchRef) + silences * sizeof(SilenceRef);
	if (size > 0) {
		next.block_ = ::operator new(size, std::nothrow);
		if (next.block_ == nullptr) {
			result.error = LoadError::out_of_memory;
			std::snprintf(result.text, sizeof(result.text), "no memory for the rules' tables (%u bytes)",
				static_cast<unsigned>(size));
			Log::error(kTag, "rules refused: %s", result.text);
			return result;
		}
		std::memset(next.block_, 0, size);
		next.block_size_ = size;
		uint8_t* p = static_cast<uint8_t*>(next.block_);
		next.matches_ = reinterpret_cast<MatchRef*>(p);
		next.silences_ = reinterpret_cast<SilenceRef*>(p + matches * sizeof(MatchRef));
		next.tracked_ = reinterpret_cast<uint16_t*>(p + matches * sizeof(MatchRef) + silences * sizeof(SilenceRef));
	}
	// The rule of each trigger, by the reactions' runs.
	for (size_t r = 0; r < next.rule_count_; ++r) {
		const Rule& rule = next.rule(r);
		for (size_t k = 0; k < rule.reaction_count; ++k) {
			const Reaction& reaction = next.reaction(rule.first_reaction + k);
			for (size_t i = 0; i < reaction.trigger_count; ++i) {
				size_t t = reaction.first_trigger + i;
				const Trigger& trigger = next.trigger(t);
				if (trigger.kind == TriggerKind::match) {
					next.matches_[next.match_count_++] = MatchRef{static_cast<uint16_t>(t), static_cast<uint8_t>(r)};
				} else if (trigger.kind == TriggerKind::silence) {
					next.silences_[next.silence_count_++] =
						SilenceRef{static_cast<uint16_t>(t), trigger.ref, trigger.ref2, static_cast<uint8_t>(r)};
				}
			}
		}
	}
	// In trigger order (the engine's queues rely on it, spec 7.11).
	std::sort(next.matches_, next.matches_ + next.match_count_,
		[](const MatchRef& a, const MatchRef& b) { return a.trigger < b.trigger; });
	std::sort(next.silences_, next.silences_ + next.silence_count_,
		[](const SilenceRef& a, const SilenceRef& b) { return a.trigger < b.trigger; });
	for (size_t n = 0; n < next.nodes_.count; ++n) {
		const Node& node = next.node(n);
		if (node.kind == NodeKind::call &&
			(static_cast<Fn>(node.op) == Fn::since_change || static_cast<Fn>(node.op) == Fn::changed_within)) {
			uint16_t datapoint = next.node(node.a).a;
			if (next.find_tracked(datapoint) == kNone) {
				next.tracked_[next.tracked_count_++] = datapoint;
			}
		}
	}

	// The timelines (spec 2.8): compiled into Timeline objects, which the inhabitant's replace.
	bool any = false;
	for (size_t r = 0; r < next.rule_count_ && !any; ++r) {
		any = next.rule(r).timeline != kNone;
	}
	if (any) {
		next.timelines_ = new (std::nothrow) Timeline[next.rule_count_];
		if (next.timelines_ == nullptr) {
			result.error = LoadError::out_of_memory;
			std::snprintf(result.text, sizeof(result.text), "no memory for the timelines");
			Log::error(kTag, "rules refused: %s", result.text);
			return result;
		}
		for (size_t r = 0; r < next.rule_count_; ++r) {
			const Rule& rule = next.rule(r);
			if (rule.timeline == kNone) {
				continue;
			}
			RuleSetStates states(next, r);
			TimelineRefusal refusal;
			if (!Timeline::from_image(view, rule.timeline, states, next.timelines_[r], refusal)) {
				result.error = refusal.error;
				std::snprintf(result.rule, sizeof(result.rule), "%s", next.str(rule.id));
				std::snprintf(result.path, sizeof(result.path), "rules[%u].timeline", static_cast<unsigned>(r));
				std::snprintf(result.text, sizeof(result.text), "%s", refusal.text);
				Log::error(kTag, "rules refused: rule '%s' at %s: %s: %s", result.rule, result.path,
					load_error_name(result.error), result.text);
				return result;
			}
		}
	}
	if (!check_storage_keys(next, result)) {
		return result;
	}
	*this = std::move(next);
	return result;
}

size_t RuleSet::schedule_count() const {
	return view_.count(image::bin::kSchedules);
}

const char* RuleSet::schedule(size_t i) const {
	return view_.str(view_.at<image::bin::ScheduleRec>(image::bin::kSchedules, i).id);
}

uint16_t RuleSet::find_schedule(const char* id) const {
	size_t count = schedule_count();
	for (size_t i = 0; i < count; ++i) {
		if (std::strcmp(schedule(i), id) == 0) {
			return static_cast<uint16_t>(i);
		}
	}
	return kNone;
}

uint16_t RuleSet::find_tracked(uint16_t datapoint) const {
	for (size_t i = 0; i < tracked_count_; ++i) {
		if (tracked_[i] == datapoint) {
			return static_cast<uint16_t>(i);
		}
	}
	return kNone;
}

uint16_t RuleSet::silence_of(size_t trigger) const {
	for (size_t i = 0; i < silence_count_; ++i) {
		if (silences_[i].trigger == trigger) {
			return static_cast<uint16_t>(i);
		}
	}
	return kNone;
}

const Timeline* RuleSet::timeline(size_t rule) const {
	return timelines_ != nullptr && rule < rule_count_ && !timelines_[rule].empty() ? &timelines_[rule] : nullptr;
}

int RuleSet::find_rule(const char* id) const {
	for (size_t i = 0; i < rule_count_; ++i) {
		if (std::strcmp(str(rule(i).id), id) == 0) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

int RuleSet::find_parameter(size_t r, const char* id) const {
	const Rule& rr = rule(r);
	for (size_t i = 0; i < rr.parameter_count; ++i) {
		if (std::strcmp(str(parameter(rr.first_parameter + i).id), id) == 0) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

int RuleSet::find_timer(size_t r, const char* id) const {
	const Rule& rr = rule(r);
	for (size_t i = 0; i < rr.timer_count; ++i) {
		if (std::strcmp(str(timer(rr.first_timer + i).id), id) == 0) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

int RuleSet::find_state(size_t r, const char* id) const {
	const Rule& rr = rule(r);
	for (size_t i = 0; i < rr.state_count; ++i) {
		if (std::strcmp(str(state(rr.first_state + i).id), id) == 0) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

RuleSetMemory RuleSet::memory() const {
	RuleSetMemory m{};
	m.rules = rule_count_ * sizeof(Rule);
	m.reactions = reactions_.count * sizeof(Reaction);
	m.triggers = triggers_.count * sizeof(Trigger);
	m.actions = actions_.count * sizeof(Action);
	m.log_parts = log_parts_.count * sizeof(LogPart);
	m.parameters = parameters_.count * sizeof(Parameter);
	m.choices = choices_.count * sizeof(Choice);
	m.timers = timers_.count * sizeof(Timer);
	m.states = states_.count * sizeof(State);
	m.nodes = nodes_.count * sizeof(Node);
	m.constants = constants_.count * sizeof(Val);
	m.tracked = tracked_count_ * sizeof(uint16_t);
	m.matches = match_count_ * sizeof(MatchRef);
	m.silences = silence_count_ * sizeof(SilenceRef);
	size_t timelines = 0;
	for (size_t r = 0; timelines_ != nullptr && r < rule_count_; ++r) {
		timelines += timelines_[r].memory();
	}
	m.timelines = timelines;
	m.block = block_size_ + timelines;
	return m;
}

} // namespace hc::rules
