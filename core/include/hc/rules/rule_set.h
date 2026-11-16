/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_RULES_RULE_SET_H
#define HC_RULES_RULE_SET_H

#include <cstddef>
#include <cstdint>

#include "hc/datapoint.h"
#include "hc/image/binary.h"
#include "hc/rules/value.h"

namespace hc::rules {

class Timeline;

/// The limits of spec/rules.md 10.2.
struct Limits {
	static constexpr size_t kRules = 128;
	static constexpr size_t kReactions = 16;
	static constexpr size_t kTriggers = 8;
	static constexpr size_t kActions = 16;
	static constexpr size_t kParameters = 8; ///< With `enabled`.
	static constexpr size_t kTimers = 8;
	static constexpr size_t kStates = 16;
	static constexpr size_t kNodesPerExpression = 64;
	static constexpr size_t kDepth = 12;
	static constexpr size_t kArguments = 8;
	static constexpr size_t kLogParts = 8;
	static constexpr size_t kNodes = 4096;
	static constexpr size_t kDerivedFirings = 32;
	static constexpr size_t kQueue = 128;
	static constexpr int64_t kTimerMinMs = 100;
	static constexpr int64_t kTimerMaxMs = 30LL * 86400000LL;
	static constexpr size_t kMessage = 200; ///< Also the longest text part of a log message.
	static constexpr size_t kChoices = 16;  ///< Choices per parameter.
	static constexpr size_t kLabel = 63;    ///< Bytes of a label text.
	static constexpr size_t kLabelLanguages = 16; ///< Entries of a label's language map.
};

/// "No index" in the compiled structures.
constexpr uint16_t kNone = 0xFFFF;
constexpr uint8_t kNoState = 0xFF;

enum class NodeKind : uint8_t { literal, datapoint, parameter, timer, unary, binary, call };

/// Operators of unary and binary nodes.
enum class Op : uint8_t { neg, not_, or_, and_, eq, ne, lt, le, gt, ge, add, sub, mul, div, mod };

/// The functions of spec/rules.md 4.7, in the order of kFunctionNames.
enum class Fn : uint8_t {
	if_, known, coalesce, between, hysteresis,
	abs, min, max, clamp, round, floor, ceil, trunc, round_to, to_int,
	now, today, time, weekday, hour, minute, second, year, month, day, date_of, time_of, in_window,
	sunrise, sunset, dawn, dusk, sun_azimuth, sun_elevation,
	since_update, since_change, changed_within, last_known, quality,
	running, remaining, state,
	length, contains, starts_with, ends_with, to_string,
	rgb, rgbw, red, green, blue, white, controlled, control_bit, control_value,
	count
};

/// The spec's name of a function ("roundTo", "sinceChange").
const char* function_name(Fn fn);
/// The text of an operator ("and", "<=", "-").
const char* operator_text(Op op);

/// One node of an expression: 16 bytes. The nodes of an expression are contiguous, in pre-order
/// with the children of a node next to each other (children at a .. a + argc - 1), so an
/// expression is the node range [root, root + size) and no node points to the heap.
struct Node {
	NodeKind kind;
	uint8_t op;   ///< Op for unary/binary, Fn for call.
	Type type;    ///< The static result type.
	uint8_t argc; ///< Children (unary 1, binary 2, call its arguments).
	/// First child; datapoint id; parameter index in its rule; timer index in its rule; rule index
	/// of state("id"); constant index of a long literal (string, raw, dateTime).
	uint16_t a;
	uint16_t size; ///< Nodes of the subtree rooted here (including itself).
	union Literal {
		Literal() : i(0) {}
		int64_t i; ///< integer, duration
		double r;
		uint32_t e;
		bool b;
		TimeOfDay tod;
		Date date;
		Controlled ctl;
		Color color;
	} lit;
};

enum class TriggerKind : uint8_t { datapoint, match, schedule, timer, start, parameter, silence };

/// Trigger flags.
struct TriggerFlags {
	static constexpr uint8_t kOnUpdate = 0x01;     ///< datapoint: every update, not only changes.
	static constexpr uint8_t kOriginReport = 0x02; ///< datapoint: reports (and stale) only.
	static constexpr uint8_t kOriginCommand = 0x04;///< datapoint: commands only.
	static constexpr uint8_t kBoot = 0x01;         ///< start reasons
	static constexpr uint8_t kImage = 0x02;
	static constexpr uint8_t kEnable = 0x04;
};

// The records below are the binary image's (spec/runtime-image.md 9.13, 9.14), read in place from
// the slot: exactly that layout, no implicit padding.

/// `rules.triggers` (8 bytes).
struct Trigger {
	TriggerKind kind;
	uint8_t flags;
	/// Datapoint index (datapoint, silence), the match condition's root, the `schedules` index of the
	/// image, the local timer or parameter index.
	uint16_t ref;
	uint16_t ref2; ///< silence: the root of `after`.
	uint16_t reserved;
};
static_assert(sizeof(Trigger) == 8, "trigger");

/// A silence trigger (spec/rules.md 2.3, 7.11), derived at load: its datapoint and the root of its
/// `after` duration.
struct SilenceRef {
	uint16_t trigger;   ///< Index in the trigger table.
	uint16_t datapoint; ///< Datapoint id.
	uint16_t after;     ///< Root node of the duration (constant within the rule: literals, parameters).
	uint8_t rule;
};

enum class ActionKind : uint8_t { write, start_timer, restart_timer, cancel_timer, set_parameter, log, read };

/// Log levels of a log action.
enum class LogActionLevel : uint8_t { info, warning, error };

/// `rules.actions` (8 bytes).
struct Action {
	ActionKind kind;
	uint8_t flags;   ///< write: 1 = onlyOnChange; log: LogActionLevel.
	uint16_t target; ///< Datapoint id (write, read), timer index, parameter index, first log part.
	uint16_t expr;   ///< Value or duration root (kNone: none).
	uint16_t count;  ///< log: the parts.
};
static_assert(sizeof(Action) == 8, "action");

/// `rules.logParts` (8 bytes): verbatim text (expr kNone) or an expression.
struct LogPart {
	uint32_t text; ///< String offset (0 for an expression part).
	uint16_t expr;
	uint16_t reserved;
};
static_assert(sizeof(LogPart) == 8, "log part");

/// A run of actions.
struct ActionList {
	uint16_t first;
	uint8_t count;
	uint8_t reserved;
};

/// `rules.reactions` (16 bytes).
struct Reaction {
	uint16_t first_trigger;
	uint8_t trigger_count;
	uint8_t to;          ///< Target state index, kNoState for none.
	uint16_t condition;  ///< Root node, kNone: always satisfied.
	uint16_t from_mask;  ///< Bit per state index; 0xFFFF without `from`.
	ActionList actions;
	ActionList else_actions;
};
static_assert(sizeof(Reaction) == 16, "reaction");

/// Labels (spec/rules.md 2.2): `label` and `name` members are label references into the image's
/// `labels` (spec/runtime-image.md 9.5): RuleSet::label() resolves one in the device's language,
/// image::write_label() writes it as the image gave it.

/// `rules.choices` (8 bytes).
struct Choice {
	uint16_t value; ///< Constant index.
	uint16_t reserved;
	uint32_t label; ///< Label reference.
};
static_assert(sizeof(Choice) == 8, "choice");

/// `rules.parameters` (28 bytes).
struct Parameter {
	uint32_t id;
	uint32_t label;
	uint32_t unit; ///< String offset (0: without unit).
	Type type;
	bool adjustable;
	uint8_t choice_count;
	uint8_t reserved;
	uint16_t first_choice;
	uint16_t default_value; ///< Constant index.
	uint16_t min;           ///< Constant index or kNone.
	uint16_t max;
	uint16_t step;          ///< Constant index or kNone.
	uint16_t datapoint;     ///< Datapoint index of `rule/<ruleId>/<id>`.
};
static_assert(sizeof(Parameter) == 28, "parameter");

/// `rules.timers` (8 bytes).
struct Timer {
	uint32_t id;
	uint16_t duration; ///< Root node.
	uint16_t reserved;
};
static_assert(sizeof(Timer) == 8, "timer");

/// `rules.states` (16 bytes).
struct State {
	uint32_t id;
	uint32_t label; ///< Label reference (0: none).
	ActionList entry;
	ActionList exit;
};
static_assert(sizeof(State) == 16, "state");

/// `rules` (28 bytes).
struct Rule {
	uint32_t id;
	uint32_t name; ///< Label reference.
	uint16_t first_parameter; ///< The rule's parameter 0 is `enabled`.
	uint16_t first_timer;
	uint16_t first_state;
	uint16_t first_reaction;
	uint8_t parameter_count;
	uint8_t timer_count;
	uint8_t state_count;
	uint8_t reaction_count;
	uint8_t initial_state; ///< kNoState without states.
	uint8_t reserved;
	uint16_t timeline;        ///< Index in `rules.timelines`, kNone without.
	uint16_t state_datapoint; ///< Datapoint index of `rule/<id>/state`, kNone without.
	uint16_t reserved2;
};
static_assert(sizeof(Rule) == 28, "rule");
static_assert(sizeof(Node) == 16, "node");
static_assert(sizeof(Val) == 40, "constant");

/// A match trigger, for the match-memory pass of spec 7.2 step 1.
struct MatchRef {
	uint16_t trigger;
	uint8_t rule;
};

/// Load refusal reasons of spec/rules.md 10.1.
enum class LoadError : uint8_t {
	ok,
	malformed,
	duplicate_id,
	unknown_reference,
	type_error,
	invalid_literal,
	invalid_parameter,
	not_writable,
	not_readable,
	forbidden,
	limit_exceeded,
	out_of_memory, ///< Not a spec reason: the block could not be allocated.
};

/// The spec's name of a reason ("malformed", "typeError", ...).
const char* load_error_name(LoadError error);

/// The outcome of a load: the reason, the rule id, the JSON path and a text.
struct LoadResult {
	LoadError error = LoadError::ok;
	char rule[33] = {};
	char path[112] = {};
	char text[96] = {};

	bool ok() const { return error == LoadError::ok; }
};

/// What a loaded rule set costs: in the slot (the image's records, read in place) and in RAM (the
/// derived tables and the timelines).
struct RuleSetMemory {
	size_t block;     ///< The RAM: the derived tables and the timelines' blocks.
	size_t rules, reactions, triggers, actions, log_parts, parameters, choices, timers, states, nodes, constants,
		tracked, matches, silences, timelines;
};

/// The prefix of the datapoints that mirror rule parameters (spec/rules.md 8.6):
/// `rule/<ruleId>/<parameterId>`. A declared datapoint of the image may not start with it.
constexpr const char* kParameterDatapointPrefix = "rule/";

/// Writes `rule/<rule>/<parameter>` into `out`; false if it does not fit (`size` at least
/// Datapoint::kMaxName + 1 always fits valid ids: 5 + 32 + 1 + 24 bytes).
bool parameter_datapoint_name(const char* rule, const char* parameter, char* out, size_t size);

/// The datapoint value type that mirrors a parameter of `type` (spec 8.6): boolean, signed (integer),
/// real, string, unsigned (duration in ms), time of day, enumeration, color; none for another type.
ValueType parameter_datapoint_type(Type type);

/// The rules of the loaded image (spec/runtime-image.md 9.13, 9.14), read in place from the slot:
/// the records, the expression nodes and the constants are the image's; RAM holds only what the
/// image does not carry and the engine needs (the tracked datapoints of sinceChange/changedWithin,
/// the match and silence triggers in trigger order, the timelines as Timeline objects). Built by
/// bind(); read-only afterwards. Movable, not copyable.
class RuleSet {
public:
	RuleSet() = default;
	~RuleSet();
	RuleSet(RuleSet&& other) noexcept;
	RuleSet& operator=(RuleSet&& other) noexcept;
	RuleSet(const RuleSet&) = delete;
	RuleSet& operator=(const RuleSet&) = delete;

	/// Binds the set to the rules of the validated image `view` (which must outlive the set) and
	/// derives its tables; labels resolve in `language` (a language tag, the device's `ui.lang`; a
	/// copy is kept). Two parameters (or rule states, spec 8.5) whose storage keys (ParameterStore)
	/// collide refuse the set (limitExceeded, spec/runtime-image.md 10.8). A refusal leaves this set
	/// unchanged (out_of_memory when a table could not be allocated).
	LoadResult bind(const image::bin::View& view, const char* language = "en");

	void clear();
	bool empty() const { return rule_count_ == 0; }

	size_t rule_count() const { return rule_count_; }
	const Rule& rule(size_t i) const { return rec<Rule>(rules_, i); }
	const Reaction& reaction(size_t i) const { return rec<Reaction>(reactions_, i); }
	const Trigger& trigger(size_t i) const { return rec<Trigger>(triggers_, i); }
	const Action& action(size_t i) const { return rec<Action>(actions_, i); }
	const LogPart& log_part(size_t i) const { return rec<LogPart>(log_parts_, i); }
	const Parameter& parameter(size_t i) const { return rec<Parameter>(parameters_, i); }
	const Choice& choice(size_t i) const { return rec<Choice>(choices_, i); }
	const Timer& timer(size_t i) const { return rec<Timer>(timers_, i); }
	const State& state(size_t i) const { return rec<State>(states_, i); }
	/// The image's timeline of `rule` (spec 2.8); nullptr without one.
	const Timeline* timeline(size_t rule) const;
	const Node& node(size_t i) const { return rec<Node>(nodes_, i); }
	const Val& constant(size_t i) const { return rec<Val>(constants_, i); }
	const char* str(uint32_t offset) const { return view_.str(offset); }
	/// A label reference resolved in the device's language ("" for none).
	const char* label(uint32_t ref) const { return view_.label_text(ref, language_); }
	/// The image the set reads.
	const image::bin::View& view() const { return view_; }

	size_t reaction_count() const { return reactions_.count; }
	size_t trigger_count() const { return triggers_.count; }
	size_t action_count() const { return actions_.count; }
	size_t parameter_count() const { return parameters_.count; }
	size_t timer_count() const { return timers_.count; }
	size_t state_count() const { return states_.count; }
	size_t node_count() const { return nodes_.count; }

	/// The image's schedules (the `schedule` triggers name them by index).
	size_t schedule_count() const;
	const char* schedule(size_t i) const;
	/// Index of schedule `id` in the image; kNone if there is none.
	uint16_t find_schedule(const char* id) const;

	/// The datapoints referenced by sinceChange/changedWithin (change tracking, spec 7.7).
	size_t tracked_count() const { return tracked_count_; }
	uint16_t tracked(size_t i) const { return tracked_[i]; }
	/// Index of `datapoint` in the tracked list; kNone if not tracked.
	uint16_t find_tracked(uint16_t datapoint) const;

	size_t match_count() const { return match_count_; }
	const MatchRef& match(size_t i) const { return matches_[i]; }

	/// The silence triggers (spec 2.3), in trigger order.
	size_t silence_count() const { return silence_count_; }
	const SilenceRef& silence(size_t i) const { return silences_[i]; }
	/// The silence index of trigger `trigger` (a silence trigger); kNone if it is none.
	uint16_t silence_of(size_t trigger) const;

	/// Index of the rule with `id`; -1 if there is none.
	int find_rule(const char* id) const;
	/// Index (within the rule) of its parameter/timer/state `id`; -1 if none.
	int find_parameter(size_t rule, const char* id) const;
	int find_timer(size_t rule, const char* id) const;
	int find_state(size_t rule, const char* id) const;

	RuleSetMemory memory() const;

private:
	template <class T>
	static const T& rec(const image::bin::Table& table, size_t i) {
		return *reinterpret_cast<const T*>(table.base + static_cast<size_t>(table.stride) * i);
	}

	image::bin::View view_;
	char language_[16] = {};
	image::bin::Table rules_, reactions_, triggers_, actions_, log_parts_, parameters_, choices_, timers_, states_,
		nodes_, constants_;
	size_t rule_count_ = 0;
	/// The derived tables, in one allocation.
	void* block_ = nullptr;
	size_t block_size_ = 0;
	uint16_t* tracked_ = nullptr;
	MatchRef* matches_ = nullptr;
	SilenceRef* silences_ = nullptr;
	Timeline* timelines_ = nullptr; ///< One per rule (empty without a timeline), or none at all.
	size_t tracked_count_ = 0, match_count_ = 0, silence_count_ = 0;
};

} // namespace hc::rules

#endif // HC_RULES_RULE_SET_H
