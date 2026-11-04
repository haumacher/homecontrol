/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_RULES_RULE_SET_H
#define HC_RULES_RULE_SET_H

#include <cstddef>
#include <cstdint>

#include "hc/datapoint.h"
#include "hc/json_value.h"
#include "hc/rules/value.h"

namespace hc::rules {

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
	sunrise, sunset, dawn, dusk,
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

struct Trigger {
	TriggerKind kind;
	uint8_t flags;
	/// Datapoint id, schedule index, timer index, parameter index, the match condition root, or the
	/// index of a silence trigger in RuleSet::silence().
	uint16_t ref;
};

/// A silence trigger (spec/rules.md 2.3, 7.11): its datapoint and the root of its `after` duration.
struct SilenceRef {
	uint16_t trigger;   ///< Index in the trigger array.
	uint16_t datapoint; ///< Datapoint id.
	uint16_t after;     ///< Root node of the duration (constant within the rule: literals, parameters).
	uint8_t rule;
};

enum class ActionKind : uint8_t { write, start_timer, restart_timer, cancel_timer, set_parameter, log, read };

/// Log levels of a log action.
enum class LogActionLevel : uint8_t { info, warning, error };

struct Action {
	ActionKind kind;
	uint8_t flags;   ///< write: 1 = onlyOnChange; log: LogActionLevel.
	uint16_t target; ///< Datapoint id (write, read), timer index, parameter index, first log part.
	uint16_t expr;   ///< Value or duration root (kNone: none); log: the part count.
};

/// A part of a log message: verbatim text (expr kNone) or an expression.
struct LogPart {
	uint32_t text; ///< String offset.
	uint16_t length;
	uint16_t expr;
};

struct ActionList {
	uint16_t first = 0;
	uint8_t count = 0;
};

struct Reaction {
	uint16_t first_trigger;
	uint8_t trigger_count;
	uint8_t to;          ///< Target state index, kNoState for none.
	uint16_t condition;  ///< Root node, kNone: always satisfied.
	uint16_t from_mask;  ///< Bit per state index; 0xFFFF without `from`.
	ActionList actions;
	ActionList else_actions;
};

/// A label (spec/rules.md 2.2, spec/runtime-image.md): `label` and `name` members are string
/// offsets of the text resolved in the device's language at load; the `..._json` members are the
/// string offset of the label's language map as compact JSON (`{"de":"...","en":"..."}`), 0 when
/// the image gave a plain string.
struct Choice {
	uint16_t value; ///< Constant index.
	uint32_t label; ///< String offset.
	uint32_t label_json;
};

struct Parameter {
	uint32_t id;
	uint32_t label;
	uint32_t label_json;
	uint32_t unit; ///< String offset ("" without unit).
	Type type;
	bool adjustable;
	uint8_t choice_count;
	uint16_t first_choice;
	uint16_t default_value; ///< Constant index.
	uint16_t min;           ///< Constant index or kNone.
	uint16_t max;
};

struct Timer {
	uint32_t id;
	uint16_t duration; ///< Root node.
};

struct State {
	uint32_t id;
	ActionList entry;
	ActionList exit;
};

struct Rule {
	uint32_t id;
	uint32_t name;
	uint32_t name_json; ///< See Choice: 0 for a plain string.
	uint16_t first_parameter; ///< The rule's parameter 0 is `enabled`.
	uint16_t first_timer;
	uint16_t first_state;
	uint16_t first_reaction;
	uint8_t parameter_count;
	uint8_t timer_count;
	uint8_t state_count;
	uint8_t reaction_count;
	uint8_t initial_state; ///< kNoState without states.
};

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

/// Where the schedules of the image are (issue #10): the loader resolves `schedule` triggers
/// against it.
class ScheduleDirectory {
public:
	virtual bool has_schedule(const char* id) const = 0;
	/// Whether `datapoint` is the `output` of a schedule entry (spec/schedules.md): the scheduler
	/// owns it, a rule's `write` to it is refused at load (notWritable, spec/rules.md 10.1).
	virtual bool owns_output(const char* datapoint) const {
		(void)datapoint;
		return false;
	}
	/// Whether a technology owns `datapoint` that can be asked for its value (a KNX group object, a
	/// Zigbee attribute, an external source): a rule's `read` of a datapoint the image binds to no
	/// such technology is refused at load (notReadable, spec/rules.md 10.1). True when the directory
	/// cannot tell; the technology's refusal at run time (readRefused) speaks then.
	virtual bool technology_reads(const char* datapoint) const {
		(void)datapoint;
		return true;
	}

protected:
	~ScheduleDirectory() = default;
};

/// What a loaded rule set costs.
struct RuleSetMemory {
	size_t block;     ///< The one allocation, all of the below.
	size_t rules, reactions, triggers, actions, log_parts, parameters, choices, timers, states, nodes, constants,
		schedules, tracked, matches, silences, strings;
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

/// A datapoint mirroring a rule parameter, as the image loader and the tests derive it from the
/// `rules` member: the name, the value type, and the flags kWritable | kTransmit when the parameter
/// is adjustable (a command is a setParameter from the API), none otherwise. Never kPersist: the
/// parameter store keeps the overrides (spec 8.4).
struct ParameterDatapoint {
	char name[Datapoint::kMaxName + 1];
	ValueType type;
	uint8_t flags;
};

/// The parameter datapoints of `rules` (spec 8.6), in rule order, `enabled` first in each rule;
/// writes up to `capacity` of them from the `first` on to `out` (which may be nullptr; a caller fills
/// its tables in batches) and returns how many there are. Lenient:
/// a rule or parameter that is not well-formed (an invalid or repeated id, an unknown type) is
/// skipped, since RuleSet::load() refuses it with the precise reason.
size_t parameter_datapoints(const JsonValue& rules, ParameterDatapoint* out, size_t capacity, size_t first = 0);

/// The `rules` member of an image, compiled: flat arrays in one allocation, indices instead of
/// pointers. Built by load(); read-only afterwards. Movable, not copyable.
class RuleSet {
public:
	RuleSet() = default;
	~RuleSet();
	RuleSet(RuleSet&& other) noexcept;
	RuleSet& operator=(RuleSet&& other) noexcept;
	RuleSet(const RuleSet&) = delete;
	RuleSet& operator=(const RuleSet&) = delete;

	/// Validates `rules` (spec 10) against the datapoints of `model` and the schedules of
	/// `schedules` (nullptr: no schedules) and replaces this set on success. A refusal leaves this
	/// set unchanged and is logged (tag hc.rule) with the rule, the path and the reason. The trees
	/// are normalized (spec 4.8); labels are resolved in `language` (a language tag, the device's
	/// `ui.lang`; spec 2.2). Two parameters (or rule states, spec 8.5) whose storage keys
	/// (ParameterStore) collide refuse the set (limitExceeded).
	LoadResult load(const JsonValue& rules, const DatapointModel& model, const ScheduleDirectory* schedules,
		const char* language = "en");
	/// The same against a directory of the datapoints (the image loader's tables: no scratch model).
	LoadResult load(const JsonValue& rules, const DatapointDirectory& datapoints, const ScheduleDirectory* schedules,
		const char* language = "en");

	void clear();
	bool empty() const { return rule_count_ == 0; }

	size_t rule_count() const { return rule_count_; }
	const Rule& rule(size_t i) const { return rules_[i]; }
	const Reaction& reaction(size_t i) const { return reactions_[i]; }
	const Trigger& trigger(size_t i) const { return triggers_[i]; }
	const Action& action(size_t i) const { return actions_[i]; }
	const LogPart& log_part(size_t i) const { return log_parts_[i]; }
	const Parameter& parameter(size_t i) const { return parameters_[i]; }
	const Choice& choice(size_t i) const { return choices_[i]; }
	const Timer& timer(size_t i) const { return timers_[i]; }
	const State& state(size_t i) const { return states_[i]; }
	const Node& node(size_t i) const { return nodes_[i]; }
	const Val& constant(size_t i) const { return constants_[i]; }
	const char* str(uint32_t offset) const { return strings_ + offset; }
	/// The language map of a label as JSON (`..._json` member), nullptr for a plain string.
	const char* label_json(uint32_t offset) const { return offset != 0 ? strings_ + offset : nullptr; }

	size_t reaction_count() const { return reaction_count_; }
	size_t trigger_count() const { return trigger_count_; }
	size_t action_count() const { return action_count_; }
	size_t parameter_count() const { return parameter_count_; }
	size_t timer_count() const { return timer_count_; }
	size_t state_count() const { return state_count_; }
	size_t node_count() const { return node_count_; }

	size_t schedule_count() const { return schedule_count_; }
	const char* schedule(size_t i) const { return strings_ + schedules_[i]; }
	/// Index of schedule `id` in this set; kNone if no trigger names it.
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

	/// Index of the rule with `id`; -1 if there is none.
	int find_rule(const char* id) const;
	/// Index (within the rule) of its parameter/timer/state `id`; -1 if none.
	int find_parameter(size_t rule, const char* id) const;
	int find_timer(size_t rule, const char* id) const;
	int find_state(size_t rule, const char* id) const;

	RuleSetMemory memory() const;

private:
	friend class Builder;

	/// One allocation per table (homecontrol #70: not one block of every table, which a fragmented heap
	/// refused with enough free bytes); block_size_ is their sum.
	static constexpr size_t kParts = 16;
	void* parts_[kParts] = {};
	size_t block_size_ = 0;
	Rule* rules_ = nullptr;
	Reaction* reactions_ = nullptr;
	Trigger* triggers_ = nullptr;
	Action* actions_ = nullptr;
	LogPart* log_parts_ = nullptr;
	Parameter* parameters_ = nullptr;
	Choice* choices_ = nullptr;
	Timer* timers_ = nullptr;
	State* states_ = nullptr;
	Node* nodes_ = nullptr;
	Val* constants_ = nullptr;
	uint32_t* schedules_ = nullptr;
	uint16_t* tracked_ = nullptr;
	MatchRef* matches_ = nullptr;
	SilenceRef* silences_ = nullptr;
	char* strings_ = nullptr;
	size_t rule_count_ = 0, reaction_count_ = 0, trigger_count_ = 0, action_count_ = 0, log_part_count_ = 0,
		   parameter_count_ = 0, choice_count_ = 0, timer_count_ = 0, state_count_ = 0, node_count_ = 0,
		   constant_count_ = 0, schedule_count_ = 0, tracked_count_ = 0, match_count_ = 0, silence_count_ = 0,
		   string_bytes_ = 0;
};

} // namespace hc::rules

#endif // HC_RULES_RULE_SET_H
