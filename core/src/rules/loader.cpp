/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The rule loader: validates the `rules` member of an image (spec/rules.md section 10) and compiles
// it into a RuleSet. Two passes over the JSON run the same code: the first validates and counts,
// the second writes into the one block allocated in between (doc/rules.md, "Memory").
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

#include "hc/log.h"
#include "hc/rules/parameter_store.h"
#include "hc/rules/rule_set.h"

namespace hc::rules {

namespace {

constexpr const char* kTag = "hc.rule";

constexpr const char* kFunctionNames[] = {
	"if", "known", "coalesce", "between", "hysteresis",
	"abs", "min", "max", "clamp", "round", "floor", "ceil", "trunc", "roundTo", "toInt",
	"now", "today", "time", "weekday", "hour", "minute", "second", "year", "month", "day", "dateOf", "timeOf",
	"inWindow", "sunrise", "sunset", "dawn", "dusk",
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
	"invalidLiteral", "invalidParameter", "notWritable", "forbidden", "limitExceeded", "outOfMemory"};

bool is_time_function(Fn fn) { return fn >= Fn::now && fn <= Fn::dusk; }
bool is_history_function(Fn fn) { return fn >= Fn::since_update && fn <= Fn::quality; }

bool numeric(Type t) { return t == Type::integer || t == Type::real; }

bool ordered(Type t) {
	return numeric(t) || t == Type::duration || t == Type::time_of_day || t == Type::date || t == Type::date_time ||
		t == Type::string;
}

/// The common type of two ordered operands (numeric mix promotes to real); invalid if none.
Type common_ordered(Type a, Type b) {
	if (numeric(a) && numeric(b)) {
		return a == Type::integer && b == Type::integer ? Type::integer : Type::real;
	}
	return a == b && ordered(a) ? a : Type::invalid;
}

/// The common type of two operands of any type (if/coalesce: same type, numeric mix to real).
Type common_any(Type a, Type b) {
	if (numeric(a) && numeric(b)) {
		return a == Type::integer && b == Type::integer ? Type::integer : Type::real;
	}
	return a == b ? a : Type::invalid;
}

bool equality_comparable(Type a, Type b) {
	if (numeric(a) && numeric(b)) {
		return true;
	}
	if ((a == Type::enumeration && b == Type::integer) || (a == Type::integer && b == Type::enumeration)) {
		return true;
	}
	return a == b && a != Type::invalid;
}

bool order_comparable(Type a, Type b) { return common_ordered(a, b) != Type::invalid; }

bool is_parameter_type(Type t) {
	return t == Type::boolean || t == Type::integer || t == Type::real || t == Type::string || t == Type::duration ||
		t == Type::time_of_day || t == Type::enumeration || t == Type::color;
}

bool is_keyword(const char* text) {
	return std::strcmp(text, "and") == 0 || std::strcmp(text, "or") == 0 || std::strcmp(text, "not") == 0 ||
		std::strcmp(text, "true") == 0 || std::strcmp(text, "false") == 0;
}

bool valid_identifier(const char* text) {
	size_t n = std::strlen(text);
	if (n < 1 || n > 24) {
		return false;
	}
	for (size_t i = 0; i < n; ++i) {
		char c = text[i];
		bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
		if (!(letter || (i > 0 && c >= '0' && c <= '9'))) {
			return false;
		}
	}
	return !is_keyword(text);
}

bool valid_rule_id(const char* text) {
	size_t n = std::strlen(text);
	if (n < 1 || n > 32) {
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

/// Ordering of two constant values of an ordered type (for min/max checks of parameters).
int compare_ordered(const Val& a, const Val& b) {
	switch (a.type) {
		case Type::integer:
		case Type::duration: return a.u.i < b.u.i ? -1 : (a.u.i > b.u.i ? 1 : 0);
		case Type::real: return a.u.r < b.u.r ? -1 : (a.u.r > b.u.r ? 1 : 0);
		case Type::time_of_day: {
			int x = a.u.tod.hour * 3600 + a.u.tod.minute * 60 + a.u.tod.second;
			int y = b.u.tod.hour * 3600 + b.u.tod.minute * 60 + b.u.tod.second;
			return x < y ? -1 : (x > y ? 1 : 0);
		}
		default: return 0;
	}
}

size_t align8(size_t n) { return (n + 7) & ~static_cast<size_t>(7); }

char ascii_lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

/// Case-insensitive comparison of the first `n` bytes (language tags are ASCII).
bool same_tag(const char* a, const char* b, size_t n) {
	for (size_t i = 0; i < n; ++i) {
		if (ascii_lower(a[i]) != ascii_lower(b[i])) {
			return false;
		}
		if (a[i] == '\0') {
			return true;
		}
	}
	return true;
}

/// A language tag of a label: 2..8 characters of A-Z a-z 0-9 - (spec/runtime-image.md).
bool valid_language_tag(const char* tag, size_t length) {
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

/// Checks the storage keys of every parameter and rule state (ParameterStore, spec 8.4, 8.5): two
/// that hash alike refuse the set.
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
			std::snprintf(result.text, sizeof(result.text), "the storage keys of %s/%s and %s/%s collide; rename one",
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

// ------------------------------------------------------------------------------------------
// Builder

/// Validates and compiles. In the counting pass (`set_` == nullptr) it only counts what it would
/// allocate; in the building pass it writes into the arrays of `set_`, which were sized by the
/// counting pass. Both passes allocate in the same order.
class Builder {
public:
	Builder(const DatapointModel& model, const ScheduleDirectory* schedules, const char* language)
		: model_(model), schedules_(schedules), language_(language != nullptr && language[0] ? language : "en") {}

	/// Runs one pass. `set` nullptr: the counting pass.
	bool run(const JsonValue& rules, RuleSet* set);

	LoadResult result;

	size_t n_rules = 0, n_reactions = 0, n_triggers = 0, n_actions = 0, n_parts = 0, n_parameters = 0,
		   n_choices = 0, n_timers = 0, n_states = 0, n_nodes = 0, n_constants = 0, n_schedules = 0, n_tracked = 0,
		   n_matches = 0, n_strings = 0;

private:
	struct PathScope {
		PathScope(Builder& b, const char* fmt, ...) HC_PRINTF_FORMAT(3, 4) : builder(b), saved(std::strlen(b.path_)) {
			va_list args;
			va_start(args, fmt);
			size_t room = sizeof(b.path_) - saved;
			std::vsnprintf(b.path_ + saved, room, fmt, args);
			va_end(args);
		}
		~PathScope() { builder.path_[saved] = '\0'; }
		Builder& builder;
		size_t saved;
	};

	/// Per-rule declarations, filled before the rule's expressions are compiled.
	struct RuleScope {
		size_t index = 0;
		size_t parameter_count = 0; ///< With `enabled` at 0.
		char parameters[Limits::kParameters][25] = {};
		Type parameter_types[Limits::kParameters] = {};
		size_t timer_count = 0;
		char timers[Limits::kTimers][25] = {};
		size_t state_count = 0;
		char states[Limits::kStates][25] = {};
	};

	/// Context of one expression.
	struct ExprScope {
		bool match = false;     ///< A match condition: pure functions only.
		size_t nodes = 0;       ///< Nodes of this expression so far.
	};

	bool fail(LoadError error, const char* fmt, ...) HC_PRINTF_FORMAT(3, 4);

	bool only(const JsonValue& object, const char* const* allowed);
	bool string_member(const JsonValue& object, const char* name, bool required, char* out, size_t size,
		size_t min_length, size_t max_length, bool& present);
	bool bool_member(const JsonValue& object, const char* name, bool fallback, bool& out);
	bool array_member(const JsonValue& object, const char* name, bool required, JsonValue& out, bool& present);

	uint32_t add_string(const char* text);
	void put_byte(char c);
	void put_json_string(const char* text);
	uint16_t add_constant(const Val& value);
	bool label_member(const JsonValue& object, const char* name, uint32_t& text, uint32_t& json);
	bool retype_enumeration(Node& node, uint16_t slot);
	bool alloc_nodes(ExprScope& scope, size_t count, uint16_t& first);

	bool prescan(const JsonValue& rules);
	bool rule(const JsonValue& json);
	bool parameter(const JsonValue& json, RuleScope& rs, Parameter* out);
	bool parameter_value(const JsonValue& json, Type type, Val& out, LoadError invalid);
	bool timer_duration(const JsonValue& json);
	bool action_list(const JsonValue& array, ActionList& out);
	bool action(const JsonValue& json, Action* out);
	bool trigger(const JsonValue& json, Trigger* out);
	bool reaction(const JsonValue& json, Reaction* out);

	/// Compiles the expression `json` into a new root; `type` receives its static type, `out` (if
	/// given) the root node.
	bool root(const JsonValue& json, bool match, uint16_t& index, Type& type, Node* out = nullptr);
	bool expr(const JsonValue& json, ExprScope& scope, size_t depth, uint16_t slot, bool timer_allowed, Type& type,
		uint16_t& size, Node* out = nullptr);
	bool folded_negation(const JsonValue& operand, ExprScope& scope, size_t depth, uint16_t slot, Type& type,
		uint16_t& size, Node* out, bool& folded);
	bool call(const JsonValue& json, ExprScope& scope, size_t depth, uint16_t slot, Node& node, Type& type,
		uint16_t& size);

	int find_parameter(const char* id) const;
	int find_timer(const char* id) const;
	int find_state(const char* id) const;
	uint16_t schedule_index(const char* id);
	void track(uint16_t datapoint);

	Node* node_at(uint16_t index) { return set_ ? &set_->nodes_[index] : nullptr; }

	const DatapointModel& model_;
	const ScheduleDirectory* schedules_;
	const char* language_;
	RuleSet* set_ = nullptr;
	RuleScope rs_;
	char path_[sizeof(LoadResult::path)] = {};
	size_t rule_ids_count_ = 0;
	char rule_ids_[Limits::kRules][33] = {};
	bool rule_has_states_[Limits::kRules] = {};
};

bool Builder::fail(LoadError error, const char* fmt, ...) {
	if (result.error == LoadError::ok) {
		result.error = error;
		std::snprintf(result.path, sizeof(result.path), "%s", path_);
		if (rs_.index < rule_ids_count_) {
			std::snprintf(result.rule, sizeof(result.rule), "%s", rule_ids_[rs_.index]);
		}
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(result.text, sizeof(result.text), fmt, args);
		va_end(args);
	}
	return false;
}

bool Builder::only(const JsonValue& object, const char* const* allowed) {
	if (!object.is(JsonKind::object)) {
		return fail(LoadError::malformed, "an object expected");
	}
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

bool Builder::string_member(const JsonValue& object, const char* name, bool required, char* out, size_t size,
	size_t min_length, size_t max_length, bool& present) {
	JsonValue member;
	out[0] = '\0';
	present = object.member(name, member);
	if (!present) {
		return required ? fail(LoadError::malformed, "member '%s' missing", name) : true;
	}
	PathScope scope(*this, ".%s", name);
	size_t length = 0;
	if (!member.is(JsonKind::string)) {
		return fail(LoadError::malformed, "'%s' must be a string", name);
	}
	if (!member.get_string(out, size, &length) || length < min_length || length > max_length) {
		out[0] = '\0';
		return fail(LoadError::malformed, "'%s' must have %u..%u bytes", name, static_cast<unsigned>(min_length),
			static_cast<unsigned>(max_length));
	}
	return true;
}

bool Builder::bool_member(const JsonValue& object, const char* name, bool fallback, bool& out) {
	JsonValue member;
	out = fallback;
	if (!object.member(name, member)) {
		return true;
	}
	PathScope scope(*this, ".%s", name);
	if (!member.get_bool(out)) {
		return fail(LoadError::malformed, "'%s' must be a boolean", name);
	}
	return true;
}

bool Builder::array_member(const JsonValue& object, const char* name, bool required, JsonValue& out, bool& present) {
	present = object.member(name, out);
	if (!present) {
		return required ? fail(LoadError::malformed, "member '%s' missing", name) : true;
	}
	if (!out.is(JsonKind::array)) {
		PathScope scope(*this, ".%s", name);
		return fail(LoadError::malformed, "'%s' must be an array", name);
	}
	return true;
}

uint32_t Builder::add_string(const char* text) {
	size_t length = std::strlen(text);
	uint32_t offset = static_cast<uint32_t>(n_strings);
	if (set_) {
		std::memcpy(set_->strings_ + offset, text, length + 1);
	}
	n_strings += length + 1;
	return offset;
}

void Builder::put_byte(char c) {
	if (set_) {
		set_->strings_[n_strings] = c;
	}
	++n_strings;
}

void Builder::put_json_string(const char* text) {
	static const char kHex[] = "0123456789abcdef";
	put_byte('"');
	for (; *text; ++text) {
		unsigned char c = static_cast<unsigned char>(*text);
		if (c == '"' || c == '\\') {
			put_byte('\\');
			put_byte(static_cast<char>(c));
		} else if (c < 0x20) {
			put_byte('\\');
			put_byte('u');
			put_byte('0');
			put_byte('0');
			put_byte(kHex[c >> 4]);
			put_byte(kHex[c & 15]);
		} else {
			put_byte(static_cast<char>(c));
		}
	}
	put_byte('"');
}

bool Builder::label_member(const JsonValue& object, const char* name, uint32_t& text, uint32_t& json) {
	JsonValue member;
	if (!object.member(name, member)) {
		return fail(LoadError::malformed, "member '%s' missing", name);
	}
	PathScope scope(*this, ".%s", name);
	char buffer[Limits::kLabel + 1];
	size_t length = 0;
	if (member.is(JsonKind::string)) {
		if (!member.get_string(buffer, sizeof(buffer), &length) || length < 1 || length > Limits::kLabel) {
			return fail(LoadError::malformed, "'%s' must have 1..%u bytes", name, static_cast<unsigned>(Limits::kLabel));
		}
		text = add_string(buffer);
		json = 0;
		return true;
	}
	if (!member.is(JsonKind::object)) {
		return fail(LoadError::malformed, "'%s' must be a string or an object of language tags to strings", name);
	}
	// The text in the device's language: the exact tag, its base language, English, the first entry.
	size_t base_length = 0;
	while (language_[base_length] != '\0' && language_[base_length] != '-') {
		++base_length;
	}
	size_t position = 0;
	char tag[16];
	size_t tag_length = 0;
	JsonValue value;
	size_t entries = 0;
	int exact = -1, base = -1, english = -1;
	while (member.next_member(position, tag, sizeof(tag), tag_length, value)) {
		if (tag_length >= sizeof(tag) || !valid_language_tag(tag, tag_length)) {
			return fail(LoadError::malformed, "a language tag of '%s' must have 2..8 characters of A-Z a-z 0-9 -", name);
		}
		PathScope tag_scope(*this, ".%s", tag);
		if (entries >= Limits::kLabelLanguages) {
			return fail(LoadError::limit_exceeded, "more than %u languages in a label",
				static_cast<unsigned>(Limits::kLabelLanguages));
		}
		if (!value.is(JsonKind::string) || !value.get_string(buffer, sizeof(buffer), &length) || length < 1 ||
			length > Limits::kLabel) {
			return fail(LoadError::malformed, "a label text must be a string of 1..%u bytes",
				static_cast<unsigned>(Limits::kLabel));
		}
		if (exact < 0 && same_tag(tag, language_, sizeof(tag)) && std::strlen(language_) == tag_length) {
			exact = static_cast<int>(entries);
		}
		if (base < 0 && tag_length == base_length && same_tag(tag, language_, base_length)) {
			base = static_cast<int>(entries);
		}
		if (english < 0 && tag_length == 2 && same_tag(tag, "en", 2)) {
			english = static_cast<int>(entries);
		}
		++entries;
	}
	if (entries == 0) {
		return fail(LoadError::malformed, "'%s' names no language", name);
	}
	int chosen = exact >= 0 ? exact : (base >= 0 ? base : (english >= 0 ? english : 0));
	// Second walk: the chosen text, then the map as compact JSON.
	position = 0;
	for (int i = 0; member.next_member(position, tag, sizeof(tag), tag_length, value); ++i) {
		if (i == chosen) {
			value.get_string(buffer, sizeof(buffer), &length);
			text = add_string(buffer);
		}
	}
	json = static_cast<uint32_t>(n_strings);
	put_byte('{');
	position = 0;
	for (int i = 0; member.next_member(position, tag, sizeof(tag), tag_length, value); ++i) {
		if (i > 0) {
			put_byte(',');
		}
		put_json_string(tag);
		put_byte(':');
		value.get_string(buffer, sizeof(buffer), &length);
		put_json_string(buffer);
	}
	put_byte('}');
	put_byte('\0');
	return true;
}

bool Builder::retype_enumeration(Node& node, uint16_t slot) {
	if (node.kind != NodeKind::literal || node.type != Type::integer || node.lit.i < 0 || node.lit.i > 4294967295LL) {
		return false;
	}
	uint32_t element = static_cast<uint32_t>(node.lit.i);
	node.lit = Node::Literal();
	node.lit.e = element;
	node.type = Type::enumeration;
	if (Node* target = node_at(slot)) {
		*target = node;
	}
	return true;
}

uint16_t Builder::add_constant(const Val& value) {
	uint16_t index = static_cast<uint16_t>(n_constants);
	if (set_) {
		set_->constants_[index] = value;
	}
	++n_constants;
	return index;
}

bool Builder::alloc_nodes(ExprScope& scope, size_t count, uint16_t& first) {
	if (scope.nodes + count > Limits::kNodesPerExpression) {
		return fail(LoadError::limit_exceeded, "more than %u nodes in an expression",
			static_cast<unsigned>(Limits::kNodesPerExpression));
	}
	if (n_nodes + count > Limits::kNodes) {
		return fail(LoadError::limit_exceeded, "more than %u expression nodes in the image",
			static_cast<unsigned>(Limits::kNodes));
	}
	first = static_cast<uint16_t>(n_nodes);
	n_nodes += count;
	scope.nodes += count;
	if (set_) {
		for (size_t i = 0; i < count; ++i) {
			set_->nodes_[first + i] = Node{};
		}
	}
	return true;
}

int Builder::find_parameter(const char* id) const {
	for (size_t i = 0; i < rs_.parameter_count; ++i) {
		if (std::strcmp(rs_.parameters[i], id) == 0) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

int Builder::find_timer(const char* id) const {
	for (size_t i = 0; i < rs_.timer_count; ++i) {
		if (std::strcmp(rs_.timers[i], id) == 0) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

int Builder::find_state(const char* id) const {
	for (size_t i = 0; i < rs_.state_count; ++i) {
		if (std::strcmp(rs_.states[i], id) == 0) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

uint16_t Builder::schedule_index(const char* id) {
	if (set_) {
		for (size_t i = 0; i < n_schedules; ++i) {
			if (std::strcmp(set_->strings_ + set_->schedules_[i], id) == 0) {
				return static_cast<uint16_t>(i);
			}
		}
		uint16_t index = static_cast<uint16_t>(n_schedules++);
		set_->schedules_[index] = add_string(id);
		return index;
	}
	// Counting: an upper bound (every reference may be a new schedule).
	add_string(id);
	return static_cast<uint16_t>(n_schedules++);
}

void Builder::track(uint16_t datapoint) {
	if (set_) {
		for (size_t i = 0; i < n_tracked; ++i) {
			if (set_->tracked_[i] == datapoint) {
				return;
			}
		}
		set_->tracked_[n_tracked] = datapoint;
	}
	++n_tracked; // counting: an upper bound
}

bool Builder::prescan(const JsonValue& rules) {
	size_t position = 0;
	JsonValue item;
	size_t i = 0;
	while (rules.next_item(position, item)) {
		PathScope scope(*this, "rules[%u]", static_cast<unsigned>(i));
		if (i >= Limits::kRules) {
			return fail(LoadError::limit_exceeded, "more than %u rules", static_cast<unsigned>(Limits::kRules));
		}
		if (!item.is(JsonKind::object)) {
			return fail(LoadError::malformed, "a rule must be an object");
		}
		bool present = false;
		char id[33];
		if (!string_member(item, "id", true, id, sizeof(id), 1, 32, present)) {
			return false;
		}
		if (!valid_rule_id(id)) {
			PathScope id_scope(*this, ".id");
			return fail(LoadError::malformed, "rule id '%s' is not 1..32 characters of A-Z a-z 0-9 _ -", id);
		}
		for (size_t j = 0; j < i; ++j) {
			if (std::strcmp(rule_ids_[j], id) == 0) {
				PathScope id_scope(*this, ".id");
				return fail(LoadError::duplicate_id, "rule id '%s' is used twice", id);
			}
		}
		std::memcpy(rule_ids_[i], id, sizeof(id));
		JsonValue states;
		rule_has_states_[i] = item.member("states", states) && states.is(JsonKind::array) && states.size() > 0;
		++i;
	}
	rule_ids_count_ = i;
	return true;
}

bool Builder::run(const JsonValue& rules, RuleSet* set) {
	set_ = set;
	n_rules = n_reactions = n_triggers = n_actions = n_parts = n_parameters = n_choices = n_timers = n_states = 0;
	n_nodes = n_constants = n_schedules = n_tracked = n_matches = n_strings = 0;
	path_[0] = '\0';
	rs_ = RuleScope{};
	rs_.index = Limits::kRules; // no rule yet
	if (!rules.is(JsonKind::array)) {
		std::snprintf(path_, sizeof(path_), "rules");
		return fail(LoadError::malformed, "'rules' must be an array");
	}
	if (!prescan(rules)) {
		return false;
	}
	add_string(""); // offset 0: the empty string
	size_t position = 0;
	JsonValue item;
	while (rules.next_item(position, item)) {
		rs_ = RuleScope{};
		rs_.index = n_rules;
		PathScope scope(*this, "rules[%u]", static_cast<unsigned>(n_rules));
		if (!rule(item)) {
			return false;
		}
	}
	return true;
}

bool Builder::rule(const JsonValue& json) {
	static const char* const kMembers[] = {"id", "name", "enabled", "enabledAdjustable", "parameters", "timers",
		"states", "initialState", "reactions", nullptr};
	if (!only(json, kMembers)) {
		return false;
	}
	size_t index = n_rules++;
	Rule* r = set_ ? &set_->rules_[index] : nullptr;
	bool present = false;
	Rule rule{};
	rule.id = add_string(rule_ids_[index]);
	if (!label_member(json, "name", rule.name, rule.name_json)) {
		return false;
	}
	bool enabled = true;
	bool enabled_adjustable = true;
	if (!bool_member(json, "enabled", true, enabled) || !bool_member(json, "enabledAdjustable", true, enabled_adjustable)) {
		return false;
	}
	rule.initial_state = kNoState;

	// Parameters: `enabled` first, then the declared ones.
	JsonValue parameters;
	bool has_parameters = false;
	if (!array_member(json, "parameters", false, parameters, has_parameters)) {
		return false;
	}
	rule.first_parameter = static_cast<uint16_t>(n_parameters);
	{
		Parameter p{};
		p.id = add_string("enabled");
		p.label = add_string("Enabled");
		p.label_json = 0;
		p.unit = 0;
		p.type = Type::boolean;
		p.adjustable = enabled_adjustable;
		p.default_value = add_constant(Val::of_bool(enabled));
		p.min = p.max = kNone;
		p.first_choice = static_cast<uint16_t>(n_choices);
		if (set_) {
			set_->parameters_[n_parameters] = p;
		}
		++n_parameters;
		std::snprintf(rs_.parameters[0], sizeof(rs_.parameters[0]), "enabled");
		rs_.parameter_types[0] = Type::boolean;
		rs_.parameter_count = 1;
	}
	if (has_parameters) {
		PathScope scope(*this, ".parameters");
		size_t position = 0;
		JsonValue item;
		size_t i = 0;
		while (parameters.next_item(position, item)) {
			PathScope item_scope(*this, "[%u]", static_cast<unsigned>(i++));
			if (rs_.parameter_count >= Limits::kParameters) {
				return fail(LoadError::limit_exceeded, "more than %u parameters (with 'enabled')",
					static_cast<unsigned>(Limits::kParameters));
			}
			Parameter p{};
			if (!parameter(item, rs_, &p)) {
				return false;
			}
			if (set_) {
				set_->parameters_[n_parameters] = p;
			}
			++n_parameters;
		}
	}
	rule.parameter_count = static_cast<uint8_t>(rs_.parameter_count);

	// Timer and state ids first: expressions and actions may reference any of them.
	JsonValue timers;
	bool has_timers = false;
	if (!array_member(json, "timers", false, timers, has_timers)) {
		return false;
	}
	JsonValue states;
	bool has_states_member = false;
	if (!array_member(json, "states", false, states, has_states_member)) {
		return false;
	}
	if (has_timers) {
		PathScope scope(*this, ".timers");
		size_t position = 0;
		JsonValue item;
		size_t i = 0;
		while (timers.next_item(position, item)) {
			PathScope item_scope(*this, "[%u]", static_cast<unsigned>(i++));
			static const char* const kTimerMembers[] = {"id", "duration", nullptr};
			if (!only(item, kTimerMembers)) {
				return false;
			}
			if (rs_.timer_count >= Limits::kTimers) {
				return fail(LoadError::limit_exceeded, "more than %u timers", static_cast<unsigned>(Limits::kTimers));
			}
			char id[25];
			if (!string_member(item, "id", true, id, sizeof(id), 1, 24, present)) {
				return false;
			}
			if (!valid_identifier(id)) {
				PathScope id_scope(*this, ".id");
				return fail(LoadError::malformed, "timer id '%s' is not an identifier", id);
			}
			if (find_timer(id) >= 0) {
				PathScope id_scope(*this, ".id");
				return fail(LoadError::duplicate_id, "timer id '%s' is used twice", id);
			}
			std::memcpy(rs_.timers[rs_.timer_count++], id, sizeof(id));
		}
	}
	if (has_states_member) {
		PathScope scope(*this, ".states");
		size_t position = 0;
		JsonValue item;
		size_t i = 0;
		while (states.next_item(position, item)) {
			PathScope item_scope(*this, "[%u]", static_cast<unsigned>(i++));
			static const char* const kStateMembers[] = {"id", "entry", "exit", nullptr};
			if (!only(item, kStateMembers)) {
				return false;
			}
			if (rs_.state_count >= Limits::kStates) {
				return fail(LoadError::limit_exceeded, "more than %u states", static_cast<unsigned>(Limits::kStates));
			}
			char id[25];
			if (!string_member(item, "id", true, id, sizeof(id), 1, 24, present)) {
				return false;
			}
			if (!valid_identifier(id)) {
				PathScope id_scope(*this, ".id");
				return fail(LoadError::malformed, "state id '%s' is not an identifier", id);
			}
			if (find_state(id) >= 0) {
				PathScope id_scope(*this, ".id");
				return fail(LoadError::duplicate_id, "state id '%s' is used twice", id);
			}
			std::memcpy(rs_.states[rs_.state_count++], id, sizeof(id));
		}
	}
	char initial[25];
	bool has_initial = false;
	if (!string_member(json, "initialState", false, initial, sizeof(initial), 1, 24, has_initial)) {
		return false;
	}
	if (rs_.state_count > 0) {
		if (!has_initial) {
			return fail(LoadError::malformed, "member 'initialState' missing (the rule has states)");
		}
		int s = find_state(initial);
		if (s < 0) {
			PathScope scope(*this, ".initialState");
			return fail(LoadError::unknown_reference, "initial state '%s' is not a state of the rule", initial);
		}
		rule.initial_state = static_cast<uint8_t>(s);
	} else if (has_initial) {
		PathScope scope(*this, ".initialState");
		return fail(LoadError::forbidden, "'initialState' in a rule without states");
	}

	// Timer durations.
	rule.first_timer = static_cast<uint16_t>(n_timers);
	rule.timer_count = static_cast<uint8_t>(rs_.timer_count);
	if (has_timers) {
		PathScope scope(*this, ".timers");
		size_t position = 0;
		JsonValue item;
		size_t i = 0;
		while (timers.next_item(position, item)) {
			PathScope item_scope(*this, "[%u]", static_cast<unsigned>(i));
			Timer t{};
			t.id = add_string(rs_.timers[i]);
			JsonValue duration;
			if (!item.member("duration", duration)) {
				return fail(LoadError::malformed, "member 'duration' missing");
			}
			PathScope duration_scope(*this, ".duration");
			Type type = Type::invalid;
			if (!root(duration, false, t.duration, type)) {
				return false;
			}
			if (type != Type::duration) {
				return fail(LoadError::type_error, "a timer duration must be a duration, not %s", type_name(type));
			}
			if (set_) {
				set_->timers_[n_timers] = t;
			}
			++n_timers;
			++i;
		}
	}

	// States with their entry and exit actions.
	rule.first_state = static_cast<uint16_t>(n_states);
	rule.state_count = static_cast<uint8_t>(rs_.state_count);
	size_t state_base = n_states;
	n_states += rs_.state_count;
	if (has_states_member) {
		PathScope scope(*this, ".states");
		size_t position = 0;
		JsonValue item;
		size_t i = 0;
		while (states.next_item(position, item)) {
			PathScope item_scope(*this, "[%u]", static_cast<unsigned>(i));
			State s{};
			s.id = add_string(rs_.states[i]);
			JsonValue list;
			bool has_list = false;
			if (!array_member(item, "entry", false, list, has_list)) {
				return false;
			}
			if (has_list) {
				PathScope list_scope(*this, ".entry");
				if (!action_list(list, s.entry)) {
					return false;
				}
			}
			if (!array_member(item, "exit", false, list, has_list)) {
				return false;
			}
			if (has_list) {
				PathScope list_scope(*this, ".exit");
				if (!action_list(list, s.exit)) {
					return false;
				}
			}
			if (set_) {
				set_->states_[state_base + i] = s;
			}
			++i;
		}
	}

	// Reactions.
	JsonValue reactions;
	if (!array_member(json, "reactions", true, reactions, present)) {
		return false;
	}
	{
		PathScope scope(*this, ".reactions");
		size_t count = reactions.size();
		if (count < 1) {
			return fail(LoadError::malformed, "a rule needs at least one reaction");
		}
		if (count > Limits::kReactions) {
			return fail(LoadError::limit_exceeded, "more than %u reactions", static_cast<unsigned>(Limits::kReactions));
		}
		rule.first_reaction = static_cast<uint16_t>(n_reactions);
		rule.reaction_count = static_cast<uint8_t>(count);
		size_t base = n_reactions;
		n_reactions += count;
		size_t position = 0;
		JsonValue item;
		size_t i = 0;
		while (reactions.next_item(position, item)) {
			PathScope item_scope(*this, "[%u]", static_cast<unsigned>(i));
			Reaction reaction_value{};
			if (!reaction(item, &reaction_value)) {
				return false;
			}
			if (set_) {
				set_->reactions_[base + i] = reaction_value;
			}
			++i;
		}
	}
	if (r) {
		*r = rule;
	}
	return true;
}

bool Builder::parameter_value(const JsonValue& json, Type type, Val& out, LoadError invalid) {
	const char* reason = nullptr;
	LiteralStatus status = val_from_json(type, json, out, &reason);
	if (status == LiteralStatus::malformed) {
		return fail(LoadError::malformed, "%s", reason ? reason : "not a value of the type");
	}
	if (status == LiteralStatus::invalid) {
		return fail(invalid, "%s", reason ? reason : "value out of range");
	}
	return true;
}

bool Builder::parameter(const JsonValue& json, RuleScope& rs, Parameter* out) {
	static const char* const kMembers[] = {"id", "label", "type", "default", "min", "max", "step", "unit", "choices",
		"adjustable", nullptr};
	if (!only(json, kMembers)) {
		return false;
	}
	bool present = false;
	char id[25];
	char type_text[24];
	char unit[16];
	if (!string_member(json, "id", true, id, sizeof(id), 1, 24, present)) {
		return false;
	}
	if (!valid_identifier(id)) {
		PathScope scope(*this, ".id");
		return fail(LoadError::malformed, "parameter id '%s' is not an identifier", id);
	}
	if (std::strcmp(id, "enabled") == 0) {
		PathScope scope(*this, ".id");
		return fail(LoadError::duplicate_id, "the parameter id 'enabled' is reserved for the enable switch");
	}
	if (find_parameter(id) >= 0) {
		PathScope scope(*this, ".id");
		return fail(LoadError::duplicate_id, "parameter id '%s' is used twice", id);
	}
	Parameter p{};
	p.id = add_string(id);
	if (!label_member(json, "label", p.label, p.label_json) ||
		!string_member(json, "type", true, type_text, sizeof(type_text), 1, 23, present)) {
		return false;
	}
	Type type = type_from_name(type_text);
	if (type == Type::invalid) {
		PathScope scope(*this, ".type");
		return fail(LoadError::malformed, "unknown type '%s'", type_text);
	}
	if (!is_parameter_type(type)) {
		PathScope scope(*this, ".type");
		return fail(LoadError::invalid_parameter, "'%s' is not a parameter type", type_text);
	}
	JsonValue unit_json;
	unit[0] = '\0';
	if (json.member("unit", unit_json)) {
		PathScope scope(*this, ".unit");
		size_t length = 0;
		if (!unit_json.is(JsonKind::string)) {
			return fail(LoadError::malformed, "'unit' must be a string");
		}
		if (!unit_json.get_string(unit, sizeof(unit), &length)) {
			return fail(LoadError::invalid_parameter, "a unit has at most 15 bytes");
		}
	}
	JsonValue step;
	if (json.member("step", step)) {
		double number = 0;
		if (!step.get_number(number)) {
			PathScope scope(*this, ".step");
			return fail(LoadError::malformed, "'step' must be a number");
		}
	}
	bool adjustable = true;
	if (!bool_member(json, "adjustable", true, adjustable)) {
		return false;
	}
	p.unit = unit[0] ? add_string(unit) : 0;
	p.type = type;
	p.adjustable = adjustable;
	p.min = p.max = kNone;
	bool ranged = type == Type::integer || type == Type::real || type == Type::duration || type == Type::time_of_day;
	Val min_value, max_value;
	JsonValue bound;
	for (int which = 0; which < 2; ++which) {
		const char* name = which == 0 ? "min" : "max";
		if (json.member(name, bound)) {
			PathScope scope(*this, ".%s", name);
			if (!ranged) {
				return fail(LoadError::invalid_parameter, "'%s' on a %s parameter", name, type_text);
			}
			Val& value = which == 0 ? min_value : max_value;
			if (!parameter_value(bound, type, value, LoadError::invalid_literal)) {
				return false;
			}
			(which == 0 ? p.min : p.max) = add_constant(value);
		}
	}
	if (p.min != kNone && p.max != kNone && compare_ordered(min_value, max_value) > 0) {
		return fail(LoadError::invalid_parameter, "min > max");
	}
	JsonValue choices;
	bool has_choices = false;
	if (!array_member(json, "choices", false, choices, has_choices)) {
		return false;
	}
	p.first_choice = static_cast<uint16_t>(n_choices);
	Val choice_values[Limits::kChoices];
	size_t choice_count = 0;
	if (has_choices) {
		PathScope scope(*this, ".choices");
		if (type != Type::integer && type != Type::string && type != Type::enumeration) {
			return fail(LoadError::invalid_parameter, "'choices' on a %s parameter", type_text);
		}
		size_t position = 0;
		JsonValue item;
		while (choices.next_item(position, item)) {
			PathScope item_scope(*this, "[%u]", static_cast<unsigned>(choice_count));
			static const char* const kChoiceMembers[] = {"value", "label", nullptr};
			if (!only(item, kChoiceMembers)) {
				return false;
			}
			if (choice_count >= Limits::kChoices) {
				return fail(LoadError::limit_exceeded, "more than %u choices", static_cast<unsigned>(Limits::kChoices));
			}
			JsonValue value_json;
			if (!item.member("value", value_json)) {
				return fail(LoadError::malformed, "member 'value' missing");
			}
			Val value;
			{
				PathScope value_scope(*this, ".value");
				if (!parameter_value(value_json, type, value, LoadError::invalid_literal)) {
					return false;
				}
			}
			Choice c{};
			c.value = add_constant(value);
			if (!label_member(item, "label", c.label, c.label_json)) {
				return false;
			}
			if (set_) {
				set_->choices_[n_choices] = c;
			}
			++n_choices;
			choice_values[choice_count++] = value;
		}
		if (choice_count == 0) {
			return fail(LoadError::invalid_parameter, "'choices' is empty");
		}
	}
	p.choice_count = static_cast<uint8_t>(choice_count);
	JsonValue default_json;
	if (!json.member("default", default_json)) {
		return fail(LoadError::malformed, "member 'default' missing");
	}
	Val value;
	{
		PathScope scope(*this, ".default");
		if (!parameter_value(default_json, type, value, LoadError::invalid_literal)) {
			return false;
		}
		if ((p.min != kNone && compare_ordered(value, min_value) < 0) ||
			(p.max != kNone && compare_ordered(value, max_value) > 0)) {
			return fail(LoadError::invalid_parameter, "the default is outside min..max");
		}
		if (choice_count > 0) {
			bool found = false;
			for (size_t i = 0; i < choice_count && !found; ++i) {
				found = val_equal(value, choice_values[i]);
			}
			if (!found) {
				return fail(LoadError::invalid_parameter, "the default is not one of the choices");
			}
		}
	}
	p.default_value = add_constant(value);
	std::memcpy(rs.parameters[rs.parameter_count], id, sizeof(id));
	rs.parameter_types[rs.parameter_count] = type;
	++rs.parameter_count;
	*out = p;
	return true;
}

bool Builder::action_list(const JsonValue& array, ActionList& out) {
	size_t count = array.size();
	if (count > Limits::kActions) {
		return fail(LoadError::limit_exceeded, "more than %u actions in a list", static_cast<unsigned>(Limits::kActions));
	}
	out.first = static_cast<uint16_t>(n_actions);
	out.count = static_cast<uint8_t>(count);
	size_t base = n_actions;
	n_actions += count;
	size_t position = 0;
	JsonValue item;
	size_t i = 0;
	while (array.next_item(position, item)) {
		PathScope scope(*this, "[%u]", static_cast<unsigned>(i));
		Action a{};
		if (!action(item, &a)) {
			return false;
		}
		if (set_) {
			set_->actions_[base + i] = a;
		}
		++i;
	}
	return true;
}

bool Builder::action(const JsonValue& json, Action* out) {
	if (!json.is(JsonKind::object)) {
		return fail(LoadError::malformed, "an action must be an object");
	}
	bool present = false;
	char kind[24];
	if (!string_member(json, "kind", true, kind, sizeof(kind), 1, 23, present)) {
		return false;
	}
	Action a{};
	a.expr = kNone;
	if (std::strcmp(kind, "write") == 0) {
		static const char* const kMembers[] = {"kind", "datapoint", "value", "onlyOnChange", nullptr};
		if (!only(json, kMembers)) {
			return false;
		}
		char name[Datapoint::kMaxName + 1];
		if (!string_member(json, "datapoint", true, name, sizeof(name), 1, Datapoint::kMaxName, present)) {
			return false;
		}
		const Datapoint* dp = model_.find(name);
		if (dp == nullptr) {
			PathScope scope(*this, ".datapoint");
			return fail(LoadError::unknown_reference, "no datapoint '%s'", name);
		}
		if (!dp->writable()) {
			PathScope scope(*this, ".datapoint");
			return fail(LoadError::not_writable, "datapoint '%s' is not writable", name);
		}
		if (schedules_ != nullptr && schedules_->owns_output(name)) {
			PathScope scope(*this, ".datapoint");
			return fail(LoadError::not_writable, "datapoint '%s' is the output of a schedule entry; the scheduler owns it",
				name);
		}
		bool only_on_change = false;
		if (!bool_member(json, "onlyOnChange", false, only_on_change)) {
			return false;
		}
		JsonValue value;
		if (!json.member("value", value)) {
			return fail(LoadError::malformed, "member 'value' missing");
		}
		PathScope scope(*this, ".value");
		Type type = Type::invalid;
		if (!root(value, false, a.expr, type)) {
			return false;
		}
		if (!write_allowed(type, dp->type())) {
			return fail(LoadError::type_error, "a %s value cannot be written to the %s datapoint '%s'", type_name(type),
				datapoint_type_name(dp->type()), name);
		}
		a.kind = ActionKind::write;
		a.flags = only_on_change ? 1 : 0;
		a.target = dp->id();
	} else if (std::strcmp(kind, "startTimer") == 0 || std::strcmp(kind, "restartTimer") == 0 ||
		std::strcmp(kind, "cancelTimer") == 0) {
		bool cancel = kind[0] == 'c';
		static const char* const kStartMembers[] = {"kind", "timer", "duration", nullptr};
		static const char* const kCancelMembers[] = {"kind", "timer", nullptr};
		if (!only(json, cancel ? kCancelMembers : kStartMembers)) {
			return false;
		}
		char id[25];
		if (!string_member(json, "timer", true, id, sizeof(id), 1, 24, present)) {
			return false;
		}
		int t = find_timer(id);
		if (t < 0) {
			PathScope scope(*this, ".timer");
			return fail(LoadError::unknown_reference, "no timer '%s' in the rule", id);
		}
		a.kind = cancel ? ActionKind::cancel_timer
						: (kind[0] == 's' ? ActionKind::start_timer : ActionKind::restart_timer);
		a.target = static_cast<uint16_t>(t);
		JsonValue duration;
		if (!cancel && json.member("duration", duration)) {
			PathScope scope(*this, ".duration");
			Type type = Type::invalid;
			if (!root(duration, false, a.expr, type)) {
				return false;
			}
			if (type != Type::duration) {
				return fail(LoadError::type_error, "a timer duration must be a duration, not %s", type_name(type));
			}
		}
	} else if (std::strcmp(kind, "setParameter") == 0) {
		static const char* const kMembers[] = {"kind", "parameter", "value", nullptr};
		if (!only(json, kMembers)) {
			return false;
		}
		char id[25];
		if (!string_member(json, "parameter", true, id, sizeof(id), 1, 24, present)) {
			return false;
		}
		int p = find_parameter(id);
		if (p == 0) {
			PathScope scope(*this, ".parameter");
			return fail(LoadError::forbidden, "the enable switch cannot be set by a rule");
		}
		if (p < 0) {
			PathScope scope(*this, ".parameter");
			return fail(LoadError::unknown_reference, "no parameter '%s' in the rule", id);
		}
		JsonValue value;
		if (!json.member("value", value)) {
			return fail(LoadError::malformed, "member 'value' missing");
		}
		PathScope scope(*this, ".value");
		Type type = Type::invalid;
		Node value_node{};
		if (!root(value, false, a.expr, type, &value_node)) {
			return false;
		}
		if (rs_.parameter_types[p] == Type::enumeration && retype_enumeration(value_node, a.expr)) {
			type = Type::enumeration; // the value takes the type of the parameter it is assigned to (4.8)
		}
		if (!assign_allowed(type, rs_.parameter_types[p])) {
			return fail(LoadError::type_error, "a %s value cannot be assigned to the %s parameter '%s'", type_name(type),
				type_name(rs_.parameter_types[p]), id);
		}
		a.kind = ActionKind::set_parameter;
		a.target = static_cast<uint16_t>(p);
	} else if (std::strcmp(kind, "log") == 0) {
		static const char* const kMembers[] = {"kind", "level", "message", nullptr};
		if (!only(json, kMembers)) {
			return false;
		}
		char level[16];
		if (!string_member(json, "level", true, level, sizeof(level), 1, 15, present)) {
			return false;
		}
		if (std::strcmp(level, "info") == 0) {
			a.flags = static_cast<uint8_t>(LogActionLevel::info);
		} else if (std::strcmp(level, "warning") == 0) {
			a.flags = static_cast<uint8_t>(LogActionLevel::warning);
		} else if (std::strcmp(level, "error") == 0) {
			a.flags = static_cast<uint8_t>(LogActionLevel::error);
		} else {
			PathScope scope(*this, ".level");
			return fail(LoadError::malformed, "unknown level '%s'", level);
		}
		JsonValue message;
		if (!array_member(json, "message", true, message, present)) {
			return false;
		}
		PathScope scope(*this, ".message");
		size_t count = message.size();
		if (count > Limits::kLogParts) {
			return fail(LoadError::limit_exceeded, "more than %u parts in a log message",
				static_cast<unsigned>(Limits::kLogParts));
		}
		a.kind = ActionKind::log;
		a.target = static_cast<uint16_t>(n_parts);
		a.expr = static_cast<uint16_t>(count);
		size_t base = n_parts;
		n_parts += count;
		size_t position = 0;
		JsonValue item;
		size_t i = 0;
		while (message.next_item(position, item)) {
			PathScope item_scope(*this, "[%u]", static_cast<unsigned>(i));
			LogPart part{};
			part.expr = kNone;
			if (item.is(JsonKind::string)) {
				char text[Limits::kMessage + 1];
				size_t length = 0;
				if (!item.get_string(text, sizeof(text), &length)) {
					// Longer than a message can be: keep what a message can show.
					return fail(LoadError::limit_exceeded, "a text part longer than %u bytes",
						static_cast<unsigned>(Limits::kMessage));
				}
				part.text = add_string(text);
				part.length = static_cast<uint16_t>(length);
			} else {
				Type type = Type::invalid;
				if (!root(item, false, part.expr, type)) {
					return false;
				}
			}
			if (set_) {
				set_->log_parts_[base + i] = part;
			}
			++i;
		}
	} else {
		PathScope scope(*this, ".kind");
		return fail(LoadError::malformed, "unknown action kind '%s'", kind);
	}
	*out = a;
	return true;
}

bool Builder::trigger(const JsonValue& json, Trigger* out) {
	if (!json.is(JsonKind::object)) {
		return fail(LoadError::malformed, "a trigger must be an object");
	}
	bool present = false;
	char kind[24];
	if (!string_member(json, "kind", true, kind, sizeof(kind), 1, 23, present)) {
		return false;
	}
	Trigger t{};
	if (std::strcmp(kind, "datapoint") == 0) {
		static const char* const kMembers[] = {"kind", "datapoint", "on", "origin", nullptr};
		if (!only(json, kMembers)) {
			return false;
		}
		char name[Datapoint::kMaxName + 1];
		if (!string_member(json, "datapoint", true, name, sizeof(name), 1, Datapoint::kMaxName, present)) {
			return false;
		}
		const Datapoint* dp = model_.find(name);
		if (dp == nullptr) {
			PathScope scope(*this, ".datapoint");
			return fail(LoadError::unknown_reference, "no datapoint '%s'", name);
		}
		char on[16];
		char origin[16];
		if (!string_member(json, "on", false, on, sizeof(on), 1, 15, present)) {
			return false;
		}
		if (present && std::strcmp(on, "update") == 0) {
			t.flags |= TriggerFlags::kOnUpdate;
		} else if (present && std::strcmp(on, "change") != 0) {
			PathScope scope(*this, ".on");
			return fail(LoadError::malformed, "'on' must be change or update");
		}
		if (!string_member(json, "origin", false, origin, sizeof(origin), 1, 15, present)) {
			return false;
		}
		if (present && std::strcmp(origin, "report") == 0) {
			t.flags |= TriggerFlags::kOriginReport;
		} else if (present && std::strcmp(origin, "command") == 0) {
			t.flags |= TriggerFlags::kOriginCommand;
		} else if (present && std::strcmp(origin, "any") != 0) {
			PathScope scope(*this, ".origin");
			return fail(LoadError::malformed, "'origin' must be any, report or command");
		}
		t.kind = TriggerKind::datapoint;
		t.ref = dp->id();
	} else if (std::strcmp(kind, "match") == 0) {
		static const char* const kMembers[] = {"kind", "condition", nullptr};
		if (!only(json, kMembers)) {
			return false;
		}
		JsonValue condition;
		if (!json.member("condition", condition)) {
			return fail(LoadError::malformed, "member 'condition' missing");
		}
		PathScope scope(*this, ".condition");
		Type type = Type::invalid;
		if (!root(condition, true, t.ref, type)) {
			return false;
		}
		if (type != Type::boolean) {
			return fail(LoadError::type_error, "a condition must be boolean, not %s", type_name(type));
		}
		t.kind = TriggerKind::match;
		if (set_) {
			set_->matches_[n_matches] = MatchRef{static_cast<uint16_t>(n_triggers), static_cast<uint8_t>(rs_.index)};
		}
		++n_matches;
	} else if (std::strcmp(kind, "schedule") == 0) {
		static const char* const kMembers[] = {"kind", "schedule", nullptr};
		if (!only(json, kMembers)) {
			return false;
		}
		char id[33];
		if (!string_member(json, "schedule", true, id, sizeof(id), 1, 32, present)) {
			return false;
		}
		if (schedules_ == nullptr || !schedules_->has_schedule(id)) {
			PathScope scope(*this, ".schedule");
			return fail(LoadError::unknown_reference, "no schedule '%s'", id);
		}
		t.kind = TriggerKind::schedule;
		t.ref = schedule_index(id);
	} else if (std::strcmp(kind, "timer") == 0) {
		static const char* const kMembers[] = {"kind", "timer", nullptr};
		if (!only(json, kMembers)) {
			return false;
		}
		char id[25];
		if (!string_member(json, "timer", true, id, sizeof(id), 1, 24, present)) {
			return false;
		}
		int timer = find_timer(id);
		if (timer < 0) {
			PathScope scope(*this, ".timer");
			return fail(LoadError::unknown_reference, "no timer '%s' in the rule", id);
		}
		t.kind = TriggerKind::timer;
		t.ref = static_cast<uint16_t>(timer);
	} else if (std::strcmp(kind, "start") == 0) {
		static const char* const kMembers[] = {"kind", "reasons", nullptr};
		if (!only(json, kMembers)) {
			return false;
		}
		JsonValue reasons;
		bool has_reasons = false;
		if (!array_member(json, "reasons", false, reasons, has_reasons)) {
			return false;
		}
		t.kind = TriggerKind::start;
		t.flags = TriggerFlags::kBoot | TriggerFlags::kImage | TriggerFlags::kEnable;
		if (has_reasons) {
			t.flags = 0;
			PathScope scope(*this, ".reasons");
			size_t position = 0;
			JsonValue item;
			size_t i = 0;
			while (reasons.next_item(position, item)) {
				PathScope item_scope(*this, "[%u]", static_cast<unsigned>(i++));
				char reason[16];
				if (!item.get_string(reason, sizeof(reason))) {
					return fail(LoadError::malformed, "a reason must be boot, image or enable");
				}
				if (std::strcmp(reason, "boot") == 0) {
					t.flags |= TriggerFlags::kBoot;
				} else if (std::strcmp(reason, "image") == 0) {
					t.flags |= TriggerFlags::kImage;
				} else if (std::strcmp(reason, "enable") == 0) {
					t.flags |= TriggerFlags::kEnable;
				} else {
					return fail(LoadError::malformed, "unknown start reason '%s'", reason);
				}
			}
		}
	} else if (std::strcmp(kind, "parameter") == 0) {
		static const char* const kMembers[] = {"kind", "parameter", nullptr};
		if (!only(json, kMembers)) {
			return false;
		}
		char id[25];
		if (!string_member(json, "parameter", true, id, sizeof(id), 1, 24, present)) {
			return false;
		}
		int p = find_parameter(id);
		if (p == 0) {
			PathScope scope(*this, ".parameter");
			return fail(LoadError::forbidden, "the enable switch cannot trigger a reaction");
		}
		if (p < 0) {
			PathScope scope(*this, ".parameter");
			return fail(LoadError::unknown_reference, "no parameter '%s' in the rule", id);
		}
		t.kind = TriggerKind::parameter;
		t.ref = static_cast<uint16_t>(p);
	} else {
		PathScope scope(*this, ".kind");
		return fail(LoadError::malformed, "unknown trigger kind '%s'", kind);
	}
	*out = t;
	return true;
}

bool Builder::reaction(const JsonValue& json, Reaction* out) {
	static const char* const kMembers[] = {"triggers", "condition", "actions", "elseActions", "from", "to", nullptr};
	if (!only(json, kMembers)) {
		return false;
	}
	Reaction r{};
	r.condition = kNone;
	r.to = kNoState;
	r.from_mask = 0xFFFF;
	bool present = false;
	JsonValue triggers;
	if (!array_member(json, "triggers", true, triggers, present)) {
		return false;
	}
	{
		PathScope scope(*this, ".triggers");
		size_t count = triggers.size();
		if (count < 1) {
			return fail(LoadError::malformed, "a reaction needs at least one trigger");
		}
		if (count > Limits::kTriggers) {
			return fail(LoadError::limit_exceeded, "more than %u triggers", static_cast<unsigned>(Limits::kTriggers));
		}
		r.first_trigger = static_cast<uint16_t>(n_triggers);
		r.trigger_count = static_cast<uint8_t>(count);
		size_t position = 0;
		JsonValue item;
		size_t i = 0;
		while (triggers.next_item(position, item)) {
			PathScope item_scope(*this, "[%u]", static_cast<unsigned>(i++));
			Trigger t{};
			if (!trigger(item, &t)) {
				return false;
			}
			if (set_) {
				set_->triggers_[n_triggers] = t;
			}
			++n_triggers;
		}
	}
	JsonValue condition;
	if (json.member("condition", condition)) {
		PathScope scope(*this, ".condition");
		Type type = Type::invalid;
		if (!root(condition, false, r.condition, type)) {
			return false;
		}
		if (type != Type::boolean) {
			return fail(LoadError::type_error, "a condition must be boolean, not %s", type_name(type));
		}
	}
	JsonValue list;
	if (!array_member(json, "actions", true, list, present)) {
		return false;
	}
	{
		PathScope scope(*this, ".actions");
		if (!action_list(list, r.actions)) {
			return false;
		}
	}
	bool has_else = false;
	if (!array_member(json, "elseActions", false, list, has_else)) {
		return false;
	}
	if (has_else) {
		PathScope scope(*this, ".elseActions");
		if (!action_list(list, r.else_actions)) {
			return false;
		}
	}
	JsonValue from;
	bool has_from = false;
	if (!array_member(json, "from", false, from, has_from)) {
		return false;
	}
	if (has_from) {
		PathScope scope(*this, ".from");
		if (rs_.state_count == 0) {
			return fail(LoadError::forbidden, "'from' in a rule without states");
		}
		r.from_mask = 0;
		size_t position = 0;
		JsonValue item;
		size_t i = 0;
		while (from.next_item(position, item)) {
			PathScope item_scope(*this, "[%u]", static_cast<unsigned>(i++));
			char id[25];
			if (!item.get_string(id, sizeof(id))) {
				return fail(LoadError::malformed, "a state id expected");
			}
			int s = find_state(id);
			if (s < 0) {
				return fail(LoadError::unknown_reference, "no state '%s' in the rule", id);
			}
			r.from_mask = static_cast<uint16_t>(r.from_mask | (1u << s));
		}
	}
	char to[25];
	bool has_to = false;
	if (!string_member(json, "to", false, to, sizeof(to), 1, 24, has_to)) {
		return false;
	}
	if (has_to) {
		PathScope scope(*this, ".to");
		if (rs_.state_count == 0) {
			return fail(LoadError::forbidden, "'to' in a rule without states");
		}
		if (has_else) {
			return fail(LoadError::forbidden, "'elseActions' together with 'to'");
		}
		int s = find_state(to);
		if (s < 0) {
			return fail(LoadError::unknown_reference, "no state '%s' in the rule", to);
		}
		r.to = static_cast<uint8_t>(s);
	}
	*out = r;
	return true;
}

bool Builder::root(const JsonValue& json, bool match, uint16_t& index, Type& type, Node* out) {
	ExprScope scope;
	scope.match = match;
	uint16_t slot = 0;
	if (!alloc_nodes(scope, 1, slot)) {
		return false;
	}
	uint16_t size = 0;
	if (!expr(json, scope, 1, slot, false, type, size, out)) {
		return false;
	}
	index = slot;
	return true;
}

bool Builder::folded_negation(const JsonValue& operand, ExprScope& scope, size_t depth, uint16_t slot, Type& type,
	uint16_t& size, Node* out, bool& folded) {
	// A minus applied directly to an integer, real or duration literal is part of the literal
	// (spec 4.8): a chain of unary minus nodes ending in such a literal compiles to one literal.
	folded = false;
	JsonValue inner = operand;
	size_t minus = 1;
	char text[16];
	JsonValue member;
	for (;;) {
		if (!inner.is(JsonKind::object) || !inner.member("kind", member) || !member.get_string(text, sizeof(text))) {
			return true;
		}
		if (std::strcmp(text, "unary") != 0) {
			break;
		}
		static const char* const kMembers[] = {"kind", "op", "operand", nullptr};
		if (!inner.member("op", member) || !member.get_string(text, sizeof(text)) || std::strcmp(text, "-") != 0 ||
			!only(inner, kMembers) || !inner.member("operand", member)) {
			return result.ok(); // not a chain of minus signs: compiled as usual (only() may have refused)
		}
		++minus;
		inner = member;
	}
	if (std::strcmp(text, "literal") != 0 || !inner.member("type", member) || !member.get_string(text, sizeof(text)) ||
		(std::strcmp(text, "integer") != 0 && std::strcmp(text, "real") != 0 && std::strcmp(text, "duration") != 0)) {
		return true;
	}
	Node node{};
	{
		PathScope p(*this, ".operand");
		if (!expr(inner, scope, depth, slot, false, type, size, &node)) {
			return false;
		}
	}
	if (minus % 2 != 0) {
		if (node.type == Type::real) {
			node.lit.r = -node.lit.r;
		} else {
			node.lit.i = -node.lit.i; // a literal's magnitude is at most 2^53 - 1
		}
	}
	if (Node* target = node_at(slot)) {
		*target = node;
	}
	if (out) {
		*out = node;
	}
	folded = true;
	return true;
}

bool Builder::expr(const JsonValue& json, ExprScope& scope, size_t depth, uint16_t slot, bool timer_allowed,
	Type& type, uint16_t& size, Node* out) {
	if (depth > Limits::kDepth) {
		return fail(LoadError::limit_exceeded, "an expression deeper than %u", static_cast<unsigned>(Limits::kDepth));
	}
	if (!json.is(JsonKind::object)) {
		return fail(LoadError::malformed, "an expression node must be an object");
	}
	bool present = false;
	char kind[16];
	if (!string_member(json, "kind", true, kind, sizeof(kind), 1, 15, present)) {
		return false;
	}
	Node node{};
	node.a = kNone;
	size = 1;
	if (std::strcmp(kind, "literal") == 0) {
		static const char* const kMembers[] = {"kind", "type", "value", nullptr};
		if (!only(json, kMembers)) {
			return false;
		}
		char type_text[24];
		if (!string_member(json, "type", true, type_text, sizeof(type_text), 1, 23, present)) {
			return false;
		}
		Type t = type_from_name(type_text);
		if (t == Type::invalid) {
			PathScope p(*this, ".type");
			return fail(LoadError::malformed, "unknown type '%s'", type_text);
		}
		JsonValue value_json;
		if (!json.member("value", value_json)) {
			return fail(LoadError::malformed, "member 'value' missing");
		}
		Val value;
		{
			PathScope p(*this, ".value");
			if (!parameter_value(value_json, t, value, LoadError::invalid_literal)) {
				return false;
			}
		}
		// Normalization (spec 4.8): an enumeration literal is the integer of its element number (its
		// partner gives it back the type, retype_enumeration()); a controlled literal is the call
		// controlled(bit, n).
		if (t == Type::enumeration) {
			value = Val::of_int(value.u.e);
			t = Type::integer;
		}
		node.kind = NodeKind::literal;
		node.type = t;
		if (t == Type::controlled) {
			if (depth + 1 > Limits::kDepth) {
				return fail(LoadError::limit_exceeded, "an expression deeper than %u (a controlled literal is a call)",
					static_cast<unsigned>(Limits::kDepth));
			}
			uint16_t children = 0;
			if (!alloc_nodes(scope, 2, children)) {
				return false;
			}
			Node bit{};
			bit.kind = NodeKind::literal;
			bit.type = Type::boolean;
			bit.a = kNone;
			bit.size = 1;
			bit.lit.b = value.u.ctl.control;
			Node number{};
			number.kind = NodeKind::literal;
			number.type = Type::integer;
			number.a = kNone;
			number.size = 1;
			number.lit.i = value.u.ctl.value;
			if (Node* target = node_at(children)) {
				target[0] = bit;
				target[1] = number;
			}
			node.kind = NodeKind::call;
			node.op = static_cast<uint8_t>(Fn::controlled);
			node.type = Type::controlled;
			node.argc = 2;
			node.a = children;
			size = 3;
			type = Type::controlled;
		} else switch (t) {
			case Type::boolean: node.lit.b = value.u.b; break;
			case Type::integer:
			case Type::duration: node.lit.i = value.u.i; break;
			case Type::real: node.lit.r = value.u.r; break;
			case Type::enumeration: node.lit.e = value.u.e; break;
			case Type::time_of_day: node.lit.tod = value.u.tod; break;
			case Type::date: node.lit.date = value.u.date; break;
			case Type::controlled: node.lit.ctl = value.u.ctl; break;
			case Type::color: node.lit.color = value.u.color; break;
			default: node.a = add_constant(value); break;
		}
	} else if (std::strcmp(kind, "datapoint") == 0) {
		static const char* const kMembers[] = {"kind", "name", nullptr};
		if (!only(json, kMembers)) {
			return false;
		}
		char name[Datapoint::kMaxName + 1];
		if (!string_member(json, "name", true, name, sizeof(name), 1, Datapoint::kMaxName, present)) {
			return false;
		}
		const Datapoint* dp = model_.find(name);
		if (dp == nullptr) {
			PathScope p(*this, ".name");
			return fail(LoadError::unknown_reference, "no datapoint '%s'", name);
		}
		node.kind = NodeKind::datapoint;
		node.type = type_of_datapoint(dp->type());
		node.a = dp->id();
	} else if (std::strcmp(kind, "parameter") == 0) {
		static const char* const kMembers[] = {"kind", "id", nullptr};
		if (!only(json, kMembers)) {
			return false;
		}
		char id[25];
		if (!string_member(json, "id", true, id, sizeof(id), 1, 24, present)) {
			return false;
		}
		int p = find_parameter(id);
		if (p == 0) {
			PathScope ps(*this, ".id");
			return fail(LoadError::forbidden, "the enable switch cannot be referenced");
		}
		if (p < 0) {
			PathScope ps(*this, ".id");
			return fail(LoadError::unknown_reference, "no parameter '%s' in the rule", id);
		}
		node.kind = NodeKind::parameter;
		node.type = rs_.parameter_types[p];
		node.a = static_cast<uint16_t>(p);
	} else if (std::strcmp(kind, "timer") == 0) {
		static const char* const kMembers[] = {"kind", "id", nullptr};
		if (!only(json, kMembers)) {
			return false;
		}
		if (!timer_allowed) {
			return fail(LoadError::forbidden, "a timer node outside running() or remaining()");
		}
		char id[25];
		if (!string_member(json, "id", true, id, sizeof(id), 1, 24, present)) {
			return false;
		}
		int t = find_timer(id);
		if (t < 0) {
			PathScope ps(*this, ".id");
			return fail(LoadError::unknown_reference, "no timer '%s' in the rule", id);
		}
		node.kind = NodeKind::timer;
		node.type = Type::invalid;
		node.a = static_cast<uint16_t>(t);
	} else if (std::strcmp(kind, "unary") == 0) {
		static const char* const kMembers[] = {"kind", "op", "operand", nullptr};
		if (!only(json, kMembers)) {
			return false;
		}
		char op[8];
		if (!string_member(json, "op", true, op, sizeof(op), 1, 7, present)) {
			return false;
		}
		Op o;
		if (std::strcmp(op, "-") == 0) {
			o = Op::neg;
		} else if (std::strcmp(op, "not") == 0) {
			o = Op::not_;
		} else {
			PathScope p(*this, ".op");
			return fail(LoadError::malformed, "unknown unary operator '%s'", op);
		}
		JsonValue operand;
		if (!json.member("operand", operand)) {
			return fail(LoadError::malformed, "member 'operand' missing");
		}
		if (o == Op::neg) {
			bool folded = false;
			if (!folded_negation(operand, scope, depth, slot, type, size, out, folded)) {
				return false;
			}
			if (folded) {
				return true;
			}
		}
		uint16_t child = 0;
		if (!alloc_nodes(scope, 1, child)) {
			return false;
		}
		Type t = Type::invalid;
		uint16_t child_size = 0;
		{
			PathScope p(*this, ".operand");
			if (!expr(operand, scope, depth + 1, child, false, t, child_size)) {
				return false;
			}
		}
		if (o == Op::not_ ? t != Type::boolean : !(numeric(t) || t == Type::duration)) {
			return fail(LoadError::type_error, "operator %s on %s", op, type_name(t));
		}
		node.kind = NodeKind::unary;
		node.op = static_cast<uint8_t>(o);
		node.type = t;
		node.argc = 1;
		node.a = child;
		size = static_cast<uint16_t>(1 + child_size);
	} else if (std::strcmp(kind, "binary") == 0) {
		static const char* const kMembers[] = {"kind", "op", "left", "right", nullptr};
		if (!only(json, kMembers)) {
			return false;
		}
		char op[8];
		if (!string_member(json, "op", true, op, sizeof(op), 1, 7, present)) {
			return false;
		}
		Op o = Op::neg;
		bool found = false;
		for (uint8_t i = static_cast<uint8_t>(Op::or_); i <= static_cast<uint8_t>(Op::mod); ++i) {
			if (std::strcmp(op, kOperatorTexts[i]) == 0) {
				o = static_cast<Op>(i);
				found = true;
				break;
			}
		}
		if (!found) {
			PathScope p(*this, ".op");
			return fail(LoadError::malformed, "unknown binary operator '%s'", op);
		}
		JsonValue left, right;
		if (!json.member("left", left)) {
			return fail(LoadError::malformed, "member 'left' missing");
		}
		if (!json.member("right", right)) {
			return fail(LoadError::malformed, "member 'right' missing");
		}
		uint16_t children = 0;
		if (!alloc_nodes(scope, 2, children)) {
			return false;
		}
		Type a = Type::invalid, b = Type::invalid;
		uint16_t size_a = 0, size_b = 0;
		Node left_node{}, right_node{};
		{
			PathScope p(*this, ".left");
			if (!expr(left, scope, depth + 1, children, false, a, size_a, &left_node)) {
				return false;
			}
		}
		{
			PathScope p(*this, ".right");
			if (!expr(right, scope, depth + 1, static_cast<uint16_t>(children + 1), false, b, size_b, &right_node)) {
				return false;
			}
		}
		if (o == Op::eq || o == Op::ne) {
			// An integer literal compared with an enumeration is an enumeration literal (4.8).
			if (a == Type::enumeration && retype_enumeration(right_node, static_cast<uint16_t>(children + 1))) {
				b = Type::enumeration;
			} else if (b == Type::enumeration && retype_enumeration(left_node, children)) {
				a = Type::enumeration;
			}
		}
		Type t = Type::invalid;
		switch (o) {
			case Op::or_:
			case Op::and_:
				if (a == Type::boolean && b == Type::boolean) {
					t = Type::boolean;
				}
				break;
			case Op::eq:
			case Op::ne:
				if (equality_comparable(a, b)) {
					t = Type::boolean;
				}
				break;
			case Op::lt:
			case Op::le:
			case Op::gt:
			case Op::ge:
				if (order_comparable(a, b)) {
					t = Type::boolean;
				}
				break;
			case Op::add:
			case Op::sub:
				if (numeric(a) && numeric(b)) {
					t = a == Type::integer && b == Type::integer ? Type::integer : Type::real;
				} else if (a == Type::duration && b == Type::duration) {
					t = Type::duration;
				} else if ((a == Type::time_of_day || a == Type::date_time) && b == Type::duration) {
					t = a;
				} else if (o == Op::sub &&
					((a == Type::time_of_day && b == Type::time_of_day) || (a == Type::date_time && b == Type::date_time) ||
						(a == Type::date && b == Type::date))) {
					t = Type::duration;
				} else if (o == Op::add && a == Type::string && b == Type::string) {
					t = Type::string;
				}
				break;
			case Op::mul:
				if (numeric(a) && numeric(b)) {
					t = a == Type::integer && b == Type::integer ? Type::integer : Type::real;
				} else if ((a == Type::duration && numeric(b)) || (numeric(a) && b == Type::duration)) {
					t = Type::duration;
				}
				break;
			case Op::div:
				if (numeric(a) && numeric(b)) {
					t = Type::real;
				} else if (a == Type::duration && numeric(b)) {
					t = Type::duration;
				} else if (a == Type::duration && b == Type::duration) {
					t = Type::real;
				}
				break;
			case Op::mod:
				if (a == Type::integer && b == Type::integer) {
					t = Type::integer;
				} else if (a == Type::duration && b == Type::duration) {
					t = Type::duration;
				}
				break;
			default: break;
		}
		if (t == Type::invalid) {
			return fail(LoadError::type_error, "operator %s on %s and %s", op, type_name(a), type_name(b));
		}
		node.kind = NodeKind::binary;
		node.op = static_cast<uint8_t>(o);
		node.type = t;
		node.argc = 2;
		node.a = children;
		size = static_cast<uint16_t>(1 + size_a + size_b);
	} else if (std::strcmp(kind, "call") == 0) {
		static const char* const kMembers[] = {"kind", "function", "args", nullptr};
		if (!only(json, kMembers)) {
			return false;
		}
		if (!call(json, scope, depth, slot, node, type, size)) {
			return false;
		}
	} else {
		PathScope p(*this, ".kind");
		return fail(LoadError::malformed, "unknown expression kind '%s'", kind);
	}
	if (node.kind != NodeKind::call) {
		type = node.type;
	}
	node.size = size;
	if (Node* target = node_at(slot)) {
		*target = node;
	}
	if (out) {
		*out = node;
	}
	return true;
}

bool Builder::call(const JsonValue& json, ExprScope& scope, size_t depth, uint16_t slot, Node& node, Type& type,
	uint16_t& size) {
	(void)slot;
	bool present = false;
	char name[24];
	if (!string_member(json, "function", true, name, sizeof(name), 1, 23, present)) {
		return false;
	}
	int fn_index = -1;
	for (size_t i = 0; i < static_cast<size_t>(Fn::count); ++i) {
		if (std::strcmp(name, kFunctionNames[i]) == 0) {
			fn_index = static_cast<int>(i);
			break;
		}
	}
	if (fn_index < 0) {
		PathScope p(*this, ".function");
		return fail(LoadError::malformed, "unknown function '%s'", name);
	}
	Fn fn = static_cast<Fn>(fn_index);
	if (scope.match && (is_time_function(fn) || is_history_function(fn) || fn == Fn::running || fn == Fn::remaining ||
		fn == Fn::state)) {
		return fail(LoadError::forbidden, "%s() in a match condition", name);
	}
	JsonValue args;
	if (!array_member(json, "args", true, args, present)) {
		return false;
	}
	PathScope args_scope(*this, ".args");
	size_t argc = args.size();
	if (argc > Limits::kArguments) {
		return fail(LoadError::limit_exceeded, "more than %u arguments", static_cast<unsigned>(Limits::kArguments));
	}

	// Arity.
	size_t min_args = 0, max_args = 0;
	switch (fn) {
		case Fn::now: case Fn::today: case Fn::time: case Fn::sunrise: case Fn::sunset: case Fn::dawn: case Fn::dusk:
			min_args = max_args = 0;
			break;
		case Fn::weekday: case Fn::state: min_args = 0; max_args = 1; break;
		case Fn::known: case Fn::abs: case Fn::round: case Fn::floor: case Fn::ceil: case Fn::trunc: case Fn::to_int:
		case Fn::hour: case Fn::minute: case Fn::second: case Fn::year: case Fn::month: case Fn::day: case Fn::date_of:
		case Fn::time_of: case Fn::since_update: case Fn::since_change: case Fn::last_known: case Fn::quality:
		case Fn::running: case Fn::remaining: case Fn::length: case Fn::to_string: case Fn::red: case Fn::green:
		case Fn::blue: case Fn::white: case Fn::control_bit: case Fn::control_value:
			min_args = max_args = 1;
			break;
		case Fn::coalesce: case Fn::round_to: case Fn::changed_within: case Fn::contains: case Fn::starts_with:
		case Fn::ends_with: case Fn::controlled:
			min_args = max_args = 2;
			break;
		case Fn::if_: case Fn::between: case Fn::clamp: case Fn::in_window: case Fn::rgb: min_args = max_args = 3; break;
		case Fn::hysteresis: case Fn::rgbw: min_args = max_args = 4; break;
		case Fn::min: case Fn::max: min_args = 2; max_args = 8; break;
		case Fn::count: break;
	}
	if (argc < min_args || argc > max_args) {
		return fail(LoadError::type_error, "%s() takes %u..%u arguments, not %u", name, static_cast<unsigned>(min_args),
			static_cast<unsigned>(max_args), static_cast<unsigned>(argc));
	}

	// Structural constraints on arguments.
	JsonValue first;
	size_t first_position = 0;
	bool has_first = args.next_item(first_position, first);
	char first_kind[16] = {};
	if (has_first) {
		JsonValue kind_json;
		if (first.member("kind", kind_json)) {
			kind_json.get_string(first_kind, sizeof(first_kind));
		}
	}
	if (is_history_function(fn) && std::strcmp(first_kind, "datapoint") != 0) {
		return fail(LoadError::type_error, "the first argument of %s() must be a datapoint reference", name);
	}
	if ((fn == Fn::running || fn == Fn::remaining) && std::strcmp(first_kind, "timer") != 0) {
		return fail(LoadError::type_error, "the argument of %s() must be a timer", name);
	}
	uint16_t state_rule = static_cast<uint16_t>(rs_.index);
	if (fn == Fn::state) {
		if (argc == 0) {
			if (rs_.state_count == 0) {
				return fail(LoadError::forbidden, "state() in a rule without states");
			}
		} else {
			char literal_type[16] = {};
			JsonValue type_json, value_json;
			char id[33];
			if (std::strcmp(first_kind, "literal") != 0 || !first.member("type", type_json) ||
				!type_json.get_string(literal_type, sizeof(literal_type)) || std::strcmp(literal_type, "string") != 0 ||
				!first.member("value", value_json)) {
				return fail(LoadError::forbidden, "the argument of state() must be one string literal");
			}
			if (!value_json.get_string(id, sizeof(id))) {
				return fail(LoadError::unknown_reference, "no rule with that id");
			}
			int target = -1;
			for (size_t i = 0; i < rule_ids_count_; ++i) {
				if (std::strcmp(rule_ids_[i], id) == 0) {
					target = static_cast<int>(i);
				}
			}
			if (target < 0) {
				return fail(LoadError::unknown_reference, "no rule '%s'", id);
			}
			if (!rule_has_states_[target]) {
				return fail(LoadError::unknown_reference, "rule '%s' has no states", id);
			}
			state_rule = static_cast<uint16_t>(target);
		}
	}

	// The arguments.
	uint16_t children = 0;
	if (argc > 0 && !alloc_nodes(scope, argc, children)) {
		return false;
	}
	Type t[Limits::kArguments] = {};
	Node nodes[Limits::kArguments] = {};
	uint16_t total = 1;
	size_t position = 0;
	JsonValue item;
	size_t i = 0;
	while (args.next_item(position, item)) {
		PathScope p(*this, "[%u]", static_cast<unsigned>(i));
		uint16_t child_size = 0;
		bool timer_allowed = (fn == Fn::running || fn == Fn::remaining) && i == 0;
		if (!expr(item, scope, depth + 1, static_cast<uint16_t>(children + i), timer_allowed, t[i], child_size,
				&nodes[i])) {
			return false;
		}
		total = static_cast<uint16_t>(total + child_size);
		++i;
	}
	// An integer literal next to an enumeration in if() and coalesce(), and the argument of toInt(),
	// is an enumeration literal (4.8).
	auto partners = [&](size_t x, size_t y) {
		if (t[x] == Type::enumeration && retype_enumeration(nodes[y], static_cast<uint16_t>(children + y))) {
			t[y] = Type::enumeration;
		} else if (t[y] == Type::enumeration && retype_enumeration(nodes[x], static_cast<uint16_t>(children + x))) {
			t[x] = Type::enumeration;
		}
	};
	if (fn == Fn::if_) {
		partners(1, 2);
	} else if (fn == Fn::coalesce) {
		partners(0, 1);
	} else if (fn == Fn::to_int && retype_enumeration(nodes[0], children)) {
		t[0] = Type::enumeration;
	}
	if ((fn == Fn::since_change || fn == Fn::changed_within) && set_) {
		track(set_->nodes_[children].a);
	} else if (fn == Fn::since_change || fn == Fn::changed_within) {
		track(0);
	}

	// Types.
	Type result = Type::invalid;
	auto bad = [&]() {
		char types[96] = {};
		size_t used = 0;
		for (size_t k = 0; k < argc && used < sizeof(types); ++k) {
			used += static_cast<size_t>(std::snprintf(types + used, sizeof(types) - used, k ? ", %s" : "%s",
				type_name(t[k])));
		}
		return fail(LoadError::type_error, "%s(%s)", name, types);
	};
	switch (fn) {
		case Fn::if_:
			if (t[0] != Type::boolean || (result = common_any(t[1], t[2])) == Type::invalid) {
				return bad();
			}
			break;
		case Fn::known: result = Type::boolean; break;
		case Fn::coalesce:
			if ((result = common_any(t[0], t[1])) == Type::invalid) {
				return bad();
			}
			break;
		case Fn::between:
			if (common_ordered(common_ordered(t[0], t[1]), t[2]) == Type::invalid ||
				common_ordered(t[0], t[1]) == Type::invalid) {
				return bad();
			}
			result = Type::boolean;
			break;
		case Fn::hysteresis: {
			bool numbers = numeric(t[0]) && numeric(t[1]) && numeric(t[2]);
			bool durations = t[0] == Type::duration && t[1] == Type::duration && t[2] == Type::duration;
			if (!(numbers || durations) || t[3] != Type::boolean) {
				return bad();
			}
			result = Type::boolean;
			break;
		}
		case Fn::abs:
			if (!(numeric(t[0]) || t[0] == Type::duration)) {
				return bad();
			}
			result = t[0];
			break;
		case Fn::min:
		case Fn::max:
		case Fn::clamp: {
			Type c = t[0];
			for (size_t k = 1; k < argc && c != Type::invalid; ++k) {
				c = common_ordered(c, t[k]);
			}
			if (c == Type::invalid || !ordered(c)) {
				return bad();
			}
			result = c;
			break;
		}
		case Fn::round:
		case Fn::floor:
		case Fn::ceil:
		case Fn::trunc:
			if (!numeric(t[0])) {
				return bad();
			}
			result = Type::integer;
			break;
		case Fn::round_to:
			if (!numeric(t[0]) || !numeric(t[1])) {
				return bad();
			}
			result = Type::real;
			break;
		case Fn::to_int:
			if (t[0] != Type::enumeration && t[0] != Type::boolean) {
				return bad();
			}
			result = Type::integer;
			break;
		case Fn::now: result = Type::date_time; break;
		case Fn::today: result = Type::date; break;
		case Fn::time: result = Type::time_of_day; break;
		case Fn::weekday:
			if (argc == 1 && t[0] != Type::date && t[0] != Type::date_time && t[0] != Type::time_of_day) {
				return bad();
			}
			result = Type::integer;
			break;
		case Fn::hour:
		case Fn::minute:
		case Fn::second:
			if (t[0] != Type::time_of_day && t[0] != Type::date_time) {
				return bad();
			}
			result = Type::integer;
			break;
		case Fn::year:
		case Fn::month:
		case Fn::day:
			if (t[0] != Type::date && t[0] != Type::date_time) {
				return bad();
			}
			result = Type::integer;
			break;
		case Fn::date_of:
		case Fn::time_of:
			if (t[0] != Type::date_time) {
				return bad();
			}
			result = fn == Fn::date_of ? Type::date : Type::time_of_day;
			break;
		case Fn::in_window:
			if ((t[0] != Type::time_of_day && t[0] != Type::date_time) || t[1] != Type::time_of_day ||
				t[2] != Type::time_of_day) {
				return bad();
			}
			result = Type::boolean;
			break;
		case Fn::sunrise:
		case Fn::sunset:
		case Fn::dawn:
		case Fn::dusk: result = Type::time_of_day; break;
		case Fn::since_update:
		case Fn::since_change: result = Type::duration; break;
		case Fn::changed_within:
			if (t[1] != Type::duration) {
				return bad();
			}
			result = Type::boolean;
			break;
		case Fn::last_known: result = t[0]; break;
		case Fn::quality: result = Type::string; break;
		case Fn::running: result = Type::boolean; break;
		case Fn::remaining: result = Type::duration; break;
		case Fn::state:
			if (argc == 1 && t[0] != Type::string) {
				return bad();
			}
			result = Type::string;
			break;
		case Fn::length:
			if (t[0] != Type::string) {
				return bad();
			}
			result = Type::integer;
			break;
		case Fn::contains:
		case Fn::starts_with:
		case Fn::ends_with:
			if (t[0] != Type::string || t[1] != Type::string) {
				return bad();
			}
			result = Type::boolean;
			break;
		case Fn::to_string: result = Type::string; break;
		case Fn::rgb:
		case Fn::rgbw:
			for (size_t k = 0; k < argc; ++k) {
				if (t[k] != Type::integer) {
					return bad();
				}
			}
			result = Type::color;
			break;
		case Fn::red:
		case Fn::green:
		case Fn::blue:
		case Fn::white:
			if (t[0] != Type::color) {
				return bad();
			}
			result = Type::integer;
			break;
		case Fn::controlled:
			if (t[0] != Type::boolean || t[1] != Type::integer) {
				return bad();
			}
			result = Type::controlled;
			break;
		case Fn::control_bit:
		case Fn::control_value:
			if (t[0] != Type::controlled) {
				return bad();
			}
			result = fn == Fn::control_bit ? Type::boolean : Type::integer;
			break;
		case Fn::count: break;
	}
	node.kind = NodeKind::call;
	node.op = static_cast<uint8_t>(fn);
	node.type = result;
	node.argc = static_cast<uint8_t>(argc);
	node.a = argc > 0 ? children : kNone;
	if (fn == Fn::state) {
		node.lit.e = state_rule;
	}
	type = result;
	size = total;
	return true;
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
	if (block_) {
		for (size_t i = 0; i < constant_count_; ++i) {
			constants_[i].~Val();
		}
		::operator delete(block_, std::nothrow);
	}
	new (this) RuleSet();
}

LoadResult RuleSet::load(const JsonValue& rules, const DatapointModel& model, const ScheduleDirectory* schedules,
	const char* language) {
	Builder* builder = new (std::nothrow) Builder(model, schedules, language);
	LoadResult result;
	if (builder == nullptr) {
		result.error = LoadError::out_of_memory;
		std::snprintf(result.text, sizeof(result.text), "no memory for the loader (%u bytes)",
			static_cast<unsigned>(sizeof(Builder)));
		Log::error(kTag, "rules refused: %s", result.text);
		return result;
	}
	if (!builder->run(rules, nullptr)) {
		result = builder->result;
		delete builder;
		Log::error(kTag, "rules refused: rule '%s' at %s: %s: %s", result.rule, result.path,
			load_error_name(result.error), result.text);
		return result;
	}

	// Lay out the block from the counts of the first pass.
	RuleSet next;
	struct Section {
		size_t offset;
		size_t bytes;
	};
	size_t offset = 0;
	auto section = [&offset](size_t bytes) {
		Section s{offset, bytes};
		offset = align8(offset + bytes);
		return s;
	};
	Section s_rules = section(builder->n_rules * sizeof(Rule));
	Section s_reactions = section(builder->n_reactions * sizeof(Reaction));
	Section s_triggers = section(builder->n_triggers * sizeof(Trigger));
	Section s_actions = section(builder->n_actions * sizeof(Action));
	Section s_parts = section(builder->n_parts * sizeof(LogPart));
	Section s_parameters = section(builder->n_parameters * sizeof(Parameter));
	Section s_choices = section(builder->n_choices * sizeof(Choice));
	Section s_timers = section(builder->n_timers * sizeof(Timer));
	Section s_states = section(builder->n_states * sizeof(State));
	Section s_nodes = section(builder->n_nodes * sizeof(Node));
	Section s_constants = section(builder->n_constants * sizeof(Val));
	Section s_schedules = section(builder->n_schedules * sizeof(uint32_t));
	Section s_tracked = section(builder->n_tracked * sizeof(uint16_t));
	Section s_matches = section(builder->n_matches * sizeof(MatchRef));
	Section s_strings = section(builder->n_strings);
	size_t total = offset;
	void* block = total > 0 ? ::operator new(total, std::nothrow) : nullptr;
	if (total > 0 && block == nullptr) {
		delete builder;
		result.error = LoadError::out_of_memory;
		std::snprintf(result.text, sizeof(result.text), "no memory for the rules (%u bytes)", static_cast<unsigned>(total));
		Log::error(kTag, "rules refused: %s", result.text);
		return result;
	}
	char* base = static_cast<char*>(block);
	next.block_ = block;
	next.block_size_ = total;
	next.rules_ = reinterpret_cast<Rule*>(base + s_rules.offset);
	next.reactions_ = reinterpret_cast<Reaction*>(base + s_reactions.offset);
	next.triggers_ = reinterpret_cast<Trigger*>(base + s_triggers.offset);
	next.actions_ = reinterpret_cast<Action*>(base + s_actions.offset);
	next.log_parts_ = reinterpret_cast<LogPart*>(base + s_parts.offset);
	next.parameters_ = reinterpret_cast<Parameter*>(base + s_parameters.offset);
	next.choices_ = reinterpret_cast<Choice*>(base + s_choices.offset);
	next.timers_ = reinterpret_cast<Timer*>(base + s_timers.offset);
	next.states_ = reinterpret_cast<State*>(base + s_states.offset);
	next.nodes_ = reinterpret_cast<Node*>(base + s_nodes.offset);
	next.constants_ = reinterpret_cast<Val*>(base + s_constants.offset);
	next.schedules_ = reinterpret_cast<uint32_t*>(base + s_schedules.offset);
	next.tracked_ = reinterpret_cast<uint16_t*>(base + s_tracked.offset);
	next.matches_ = reinterpret_cast<MatchRef*>(base + s_matches.offset);
	next.strings_ = base + s_strings.offset;
	for (size_t i = 0; i < builder->n_constants; ++i) {
		new (&next.constants_[i]) Val();
	}
	next.constant_count_ = builder->n_constants;
	(void)s_rules.bytes;

	if (!builder->run(rules, &next)) {
		// The counting pass accepted what the building pass refused: a loader bug, never silent.
		result = builder->result;
		delete builder;
		Log::error(kTag, "rules refused in the second pass (loader inconsistency): %s: %s", load_error_name(result.error),
			result.text);
		return result;
	}
	next.rule_count_ = builder->n_rules;
	next.reaction_count_ = builder->n_reactions;
	next.trigger_count_ = builder->n_triggers;
	next.action_count_ = builder->n_actions;
	next.log_part_count_ = builder->n_parts;
	next.parameter_count_ = builder->n_parameters;
	next.choice_count_ = builder->n_choices;
	next.timer_count_ = builder->n_timers;
	next.state_count_ = builder->n_states;
	next.node_count_ = builder->n_nodes;
	next.schedule_count_ = builder->n_schedules;
	next.tracked_count_ = builder->n_tracked;
	next.match_count_ = builder->n_matches;
	next.string_bytes_ = builder->n_strings;
	delete builder;
	if (!check_storage_keys(next, result)) {
		return result;
	}
	*this = std::move(next);
	Log::info(kTag, "rules loaded: %u rules, %u nodes, %u bytes", static_cast<unsigned>(rule_count_),
		static_cast<unsigned>(node_count_), static_cast<unsigned>(block_size_));
	return result;
}

uint16_t RuleSet::find_schedule(const char* id) const {
	for (size_t i = 0; i < schedule_count_; ++i) {
		if (std::strcmp(strings_ + schedules_[i], id) == 0) {
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

int RuleSet::find_rule(const char* id) const {
	for (size_t i = 0; i < rule_count_; ++i) {
		if (std::strcmp(str(rules_[i].id), id) == 0) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

int RuleSet::find_parameter(size_t rule, const char* id) const {
	const Rule& r = rules_[rule];
	for (size_t i = 0; i < r.parameter_count; ++i) {
		if (std::strcmp(str(parameters_[r.first_parameter + i].id), id) == 0) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

int RuleSet::find_timer(size_t rule, const char* id) const {
	const Rule& r = rules_[rule];
	for (size_t i = 0; i < r.timer_count; ++i) {
		if (std::strcmp(str(timers_[r.first_timer + i].id), id) == 0) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

int RuleSet::find_state(size_t rule, const char* id) const {
	const Rule& r = rules_[rule];
	for (size_t i = 0; i < r.state_count; ++i) {
		if (std::strcmp(str(states_[r.first_state + i].id), id) == 0) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

RuleSetMemory RuleSet::memory() const {
	RuleSetMemory m{};
	m.block = block_size_;
	m.rules = rule_count_ * sizeof(Rule);
	m.reactions = reaction_count_ * sizeof(Reaction);
	m.triggers = trigger_count_ * sizeof(Trigger);
	m.actions = action_count_ * sizeof(Action);
	m.log_parts = log_part_count_ * sizeof(LogPart);
	m.parameters = parameter_count_ * sizeof(Parameter);
	m.choices = choice_count_ * sizeof(Choice);
	m.timers = timer_count_ * sizeof(Timer);
	m.states = state_count_ * sizeof(State);
	m.nodes = node_count_ * sizeof(Node);
	m.constants = constant_count_ * sizeof(Val);
	m.schedules = schedule_count_ * sizeof(uint32_t);
	m.tracked = tracked_count_ * sizeof(uint16_t);
	m.matches = match_count_ * sizeof(MatchRef);
	m.strings = string_bytes_;
	return m;
}

} // namespace hc::rules
