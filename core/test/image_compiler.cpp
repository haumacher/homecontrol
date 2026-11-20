/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The test-only compiler of runtime images (issue #87): spec/runtime-image.md section 9 in the
// canonical layout of 9.18. The semantic details the specification leaves to the loaders (which
// implied datapoints exist and in which order, the static types of expression nodes, the
// normalization of 4.8, the resolved defaults of the control pages) are taken from the device's JSON
// loaders of firmware 1.2 (core/src/image/loader.cpp, core/src/rules/loader.cpp, timeline.cpp,
// core/src/blocks/block_set.cpp, model_view.cpp, web_api.cpp), copied here because the device no
// longer has them.
#include "image_compiler.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <utility>

#include "hc/blocks/block_type.h"
#include "hc/dpt.h"
#include "hc/json.h"
#include "hc/json_document.h"
#include "hc/rules/value.h"
#include "hc/sha.h"
#include "hc/signature.h"
#include "hc/value_store.h"

namespace hc::test {

namespace {

using hc::rules::Type;
using hc::rules::Val;

constexpr uint16_t kNone = 0xFFFF;

// ---- Sections (spec/runtime-image.md 9.4) --------------------------------------------------------

enum Section : uint16_t {
	kImage = 0x01,
	kStrings = 0x02,
	kLabels = 0x03,
	kSettings = 0x04,
	kDatapoints = 0x10,
	kLocations = 0x11,
	kFunctions = 0x12,
	kRoles = 0x13,
	kHistory = 0x14,
	kArchives = 0x15,
	kKnxAddresses = 0x20,
	kKnxBindings = 0x21,
	kKnxListen = 0x22,
	kKnxListeners = 0x23,
	kKnxTimeMaster = 0x24,
	kZigbee = 0x28,
	kExternalSources = 0x2C,
	kExternalBindings = 0x2D,
	kMqtt = 0x2E,
	kSchedules = 0x30,
	kScheduleDates = 0x31,
	kScheduleBounds = 0x32,
	kRules = 0x40,
	kParameters = 0x41,
	kChoices = 0x42,
	kTimers = 0x43,
	kStates = 0x44,
	kReactions = 0x45,
	kTriggers = 0x46,
	kActions = 0x47,
	kLogParts = 0x48,
	kTimelines = 0x49,
	kPlans = 0x4A,
	kPoints = 0x4B,
	kAssignments = 0x4C,
	kNodes = 0x4D,
	kConstants = 0x4E,
	kBlocks = 0x50,
	kPorts = 0x51,
	kBound = 0x52,
	kBlockParameters = 0x53,
	kUi = 0x60,
	kUiAnonymous = 0x62,
	kUiAnonymousDatapoints = 0x63,
	kUiAnonymousFunctions = 0x64,
};

uint16_t entry_size(uint16_t id) {
	switch (id) {
		case kImage: return 32;
		case kLabels: return 8;
		case kSettings: return 32;
		case kDatapoints: return 24;
		case kLocations: return 12;
		case kFunctions: return 28;
		case kRoles: return 8;
		case kHistory: return 16;
		case kArchives: return 8;
		case kKnxAddresses: return 8;
		case kKnxBindings: return 12;
		case kKnxListen: return 2;
		case kKnxListeners: return 2;
		case kKnxTimeMaster: return 12;
		case kZigbee: return 32;
		case kExternalSources: return 20;
		case kExternalBindings: return 32;
		case kMqtt: return 20;
		case kSchedules: return 64;
		case kScheduleDates: return 4;
		case kScheduleBounds: return 32;
		case kRules: return 28;
		case kParameters: return 28;
		case kChoices: return 8;
		case kTimers: return 8;
		case kStates: return 16;
		case kReactions: return 16;
		case kTriggers: return 8;
		case kActions: return 8;
		case kLogParts: return 8;
		case kTimelines: return 8;
		case kPlans: return 12;
		case kPoints: return 16;
		case kAssignments: return 2;
		case kNodes: return 16;
		case kConstants: return 40;
		case kBlocks: return 28;
		case kPorts: return 12;
		case kBound: return 2;
		case kBlockParameters: return 16;
		case kUiAnonymousDatapoints: return 4;
		case kUiAnonymousFunctions: return 2;
		default: return 0; // strings, ui, ui.anonymous: byte sections
	}
}

/// A table section under construction: fixed-size records, little-endian fields.
struct Table {
	uint16_t id = 0;
	uint16_t entry = 0;
	std::vector<uint8_t> bytes;

	size_t count() const { return entry == 0 ? 0 : bytes.size() / entry; }
	size_t add() {
		size_t i = count();
		bytes.resize(bytes.size() + entry, 0);
		return i;
	}
	uint8_t* at(size_t record, size_t offset) { return bytes.data() + record * entry + offset; }
	void u8(size_t r, size_t o, uint64_t v) { *at(r, o) = static_cast<uint8_t>(v); }
	void u16(size_t r, size_t o, uint64_t v) {
		for (size_t i = 0; i < 2; ++i) {
			at(r, o)[i] = static_cast<uint8_t>(v >> (8 * i));
		}
	}
	void u32(size_t r, size_t o, uint64_t v) {
		for (size_t i = 0; i < 4; ++i) {
			at(r, o)[i] = static_cast<uint8_t>(v >> (8 * i));
		}
	}
	void u64(size_t r, size_t o, uint64_t v) {
		for (size_t i = 0; i < 8; ++i) {
			at(r, o)[i] = static_cast<uint8_t>(v >> (8 * i));
		}
	}
	void i32(size_t r, size_t o, int64_t v) { u32(r, o, static_cast<uint32_t>(static_cast<int32_t>(v))); }
	void i64(size_t r, size_t o, int64_t v) { u64(r, o, static_cast<uint64_t>(v)); }
	void f64(size_t r, size_t o, double v) {
		uint64_t bits = 0;
		std::memcpy(&bits, &v, sizeof(bits));
		u64(r, o, bits);
	}
	uint8_t get8(size_t r, size_t o) { return *at(r, o); }
	uint16_t get16(size_t r, size_t o) { return static_cast<uint16_t>(at(r, o)[0] | (at(r, o)[1] << 8)); }
	int64_t get64(size_t r, size_t o) {
		uint64_t v = 0;
		for (size_t i = 0; i < 8; ++i) {
			v |= static_cast<uint64_t>(at(r, o)[i]) << (8 * i);
		}
		return static_cast<int64_t>(v);
	}
};

// ---- JSON helpers over spans of the validated text -----------------------------------------------

using Kind = JsonSpan::Kind;

bool member(const JsonSpan& object, const char* name, JsonSpan& out) {
	return object.kind == Kind::object && json_member(object, name, out);
}

std::string text_of(const JsonSpan& span) {
	static char buffer[4096];
	size_t length = 0;
	if (span.kind != Kind::string || !json_string(span, buffer, sizeof(buffer), &length)) {
		return std::string();
	}
	return std::string(buffer, length);
}

std::string member_text(const JsonSpan& object, const char* name) {
	JsonSpan value;
	return member(object, name, value) ? text_of(value) : std::string();
}

bool has_member(const JsonSpan& object, const char* name) {
	JsonSpan value;
	return member(object, name, value);
}

double number_of(const JsonSpan& span, double fallback = 0) {
	double value = 0;
	return span.kind == Kind::number && json_number(span, value) ? value : fallback;
}

double member_number(const JsonSpan& object, const char* name, double fallback = 0) {
	JsonSpan value;
	return member(object, name, value) ? number_of(value, fallback) : fallback;
}

bool member_flag(const JsonSpan& object, const char* name, bool fallback) {
	JsonSpan value;
	bool b = fallback;
	if (member(object, name, value) && value.kind == Kind::boolean) {
		json_bool(value, b);
	}
	return b;
}

std::vector<JsonSpan> items(const JsonSpan& array) {
	std::vector<JsonSpan> out;
	if (array.kind != Kind::array) {
		return out;
	}
	size_t position = 0;
	JsonSpan item;
	while (json_next_item(array, position, item)) {
		out.push_back(item);
	}
	return out;
}

std::vector<JsonSpan> member_items(const JsonSpan& object, const char* name) {
	JsonSpan value;
	return member(object, name, value) ? items(value) : std::vector<JsonSpan>();
}

std::vector<std::pair<std::string, JsonSpan>> members_of(const JsonSpan& object) {
	std::vector<std::pair<std::string, JsonSpan>> out;
	if (object.kind != Kind::object) {
		return out;
	}
	size_t position = 0;
	JsonSpan name;
	JsonSpan value;
	while (json_next_member(object, position, name, value)) {
		out.emplace_back(text_of(name), value);
	}
	return out;
}

/// "<major>.<minor>".
bool parse_version(const std::string& text, uint32_t& major, uint32_t& minor) {
	unsigned a = 0, b = 0;
	char tail = 0;
	if (std::sscanf(text.c_str(), "%u.%u%c", &a, &b, &tail) != 2) {
		return false;
	}
	major = a;
	minor = b;
	return true;
}

/// A group address "main/middle/sub" or "main/sub" as its 16-bit value.
bool group_address(const std::string& text, uint16_t& out) {
	uint32_t parts[3] = {};
	size_t count = 0;
	const char* p = text.c_str();
	while (count < 3) {
		if (*p < '0' || *p > '9') {
			return false;
		}
		uint32_t value = 0;
		while (*p >= '0' && *p <= '9') {
			value = value * 10 + static_cast<uint32_t>(*p - '0');
			if (value > 65535) {
				return false;
			}
			++p;
		}
		parts[count++] = value;
		if (*p == '\0') {
			break;
		}
		if (*p != '/') {
			return false;
		}
		++p;
	}
	if (*p != '\0' || count < 2) {
		return false;
	}
	if (count == 2) {
		out = static_cast<uint16_t>((parts[0] << 11) | parts[1]);
	} else {
		out = static_cast<uint16_t>((parts[0] << 11) | (parts[1] << 8) | parts[2]);
	}
	return true;
}

/// An IEEE address: 16 hex digits, colons allowed between the octets.
bool parse_ieee(const std::string& text, uint64_t& out) {
	uint64_t value = 0;
	int digits = 0;
	for (char c : text) {
		if (c == ':') {
			continue;
		}
		int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
		if (d < 0) {
			return false;
		}
		value = (value << 4) | static_cast<uint64_t>(d);
		++digits;
	}
	out = value;
	return digits == 16;
}

int index_of(const char* const* names, size_t count, const std::string& name) {
	for (size_t i = 0; i < count; ++i) {
		if (name == names[i]) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

uint32_t seconds_of_clock(const JsonSpan& span) {
	return static_cast<uint32_t>(member_number(span, "hour") * 3600 + member_number(span, "minute") * 60 +
		member_number(span, "second"));
}

uint32_t ceil_div(uint64_t a, uint64_t b) { return b == 0 ? 0 : static_cast<uint32_t>((a + b - 1) / b); }

// ---- Expression types (spec/rules.md 3, 4; the rule loader's inference) ------------------------

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

enum class Op : uint8_t { neg, not_, or_, and_, eq, ne, lt, le, gt, ge, add, sub, mul, div, mod };

constexpr const char* kOperatorTexts[] = {"-", "not", "or", "and", "==", "!=", "<", "<=", ">", ">=", "+", "-", "*",
	"/", "%"};

enum NodeKind : uint8_t { kLiteral, kDatapointNode, kParameterNode, kTimerNode, kUnary, kBinary, kCall };

bool numeric(Type t) { return t == Type::integer || t == Type::real; }

bool ordered(Type t) {
	return numeric(t) || t == Type::duration || t == Type::time_of_day || t == Type::date || t == Type::date_time ||
		t == Type::string;
}

Type common_ordered(Type a, Type b) {
	if (numeric(a) && numeric(b)) {
		return a == Type::integer && b == Type::integer ? Type::integer : Type::real;
	}
	return a == b && ordered(a) ? a : Type::invalid;
}

Type common_any(Type a, Type b) {
	if (numeric(a) && numeric(b)) {
		return a == Type::integer && b == Type::integer ? Type::integer : Type::real;
	}
	return a == b ? a : Type::invalid;
}

/// The node record of the canonical form (9.14) while it is built.
struct Node {
	uint8_t kind = 0;
	uint8_t op = 0;
	uint8_t type = 0;
	uint8_t argc = 0;
	uint16_t a = kNone;
	uint16_t size = 1;
	uint8_t lit[8] = {};
};

// ---- Labels --------------------------------------------------------------------------------------

/// A label of the source: one entry with an empty tag for a plain string, else the language map's
/// entries in the source's order.
using Label = std::vector<std::pair<std::string, std::string>>;

// ---- The compiler --------------------------------------------------------------------------------

struct Datapoint {
	std::string name;
	int label = -1;
	std::string unit;
	uint8_t type = 0;
	uint8_t flags = 0;
	uint8_t owner = 0;
	uint16_t owner_index = kNone;
	uint16_t owner_member = 0;
	uint16_t history = kNone;
};

constexpr uint8_t kReadable = 0x01;
constexpr uint8_t kWritable = 0x02;
constexpr uint8_t kTransmit = 0x04;
constexpr uint8_t kReadOnStart = 0x08;
constexpr uint8_t kPersist = 0x10;
constexpr uint8_t kHistoryFlag = 0x20;
constexpr uint8_t kApplication = 0x40;
constexpr uint8_t kVirtual = 0x80;

enum Owner : uint8_t { kOwnerNone, kOwnerKnx, kOwnerScheduler, kOwnerTimeMaster, kOwnerZigbee, kOwnerRules,
	kOwnerBlocks, kOwnerExternal };

/// Per rule, what its expressions may reference.
struct RuleScope {
	size_t index = 0;
	std::string id;
	std::vector<std::string> parameters; ///< `enabled` at 0.
	std::vector<Type> parameter_types;
	std::vector<std::string> timers;
	std::vector<std::string> states;
};

class Compiler {
public:
	Compiler(const blocks::BlockRegistry& registry, bool harness) : registry_(registry), harness_(harness) {}

	bool compile(const char* json, size_t length, std::vector<uint8_t>& out, const uint8_t* seed);

	std::string error;

private:
	bool fail(const char* format, ...) __attribute__((format(printf, 2, 3)));

	Table& table(uint16_t id) {
		Table& t = tables_[id];
		t.id = id;
		t.entry = entry_size(id);
		return t;
	}
	void put_string(Table& t, size_t record, size_t offset, const std::string& text) {
		if (!text.empty()) {
			string_fixes_.push_back(StringFix{t.id, record * t.entry + offset, text});
		}
	}
	void put_label(Table& t, size_t record, size_t offset, int label) {
		if (label >= 0) {
			label_fixes_.push_back(LabelFix{t.id, record * t.entry + offset, label});
		}
	}
	int label(const JsonSpan& span);
	int datapoint(const std::string& name) const {
		auto it = index_.find(name);
		return it == index_.end() ? -1 : static_cast<int>(it->second);
	}
	uint16_t add_datapoint(const std::string& name, uint8_t type, uint8_t flags, uint8_t owner, uint16_t owner_index,
		uint16_t owner_member);

	bool declared_datapoints(const JsonSpan& root);
	void time_master(const JsonSpan& root);
	void rule_datapoints(const JsonSpan& root);
	void schedule_datapoints(const JsonSpan& root);
	void block_datapoints(const JsonSpan& root);
	bool knx_tables();
	void zigbee_table();
	void history_tables();
	bool locations_and_functions(const JsonSpan& root);
	bool external(const JsonSpan& root);
	void mqtt(const JsonSpan& root);
	bool schedules(const JsonSpan& root);
	bool rules(const JsonSpan& root);
	bool rule(const JsonSpan& json, size_t index);
	bool action_list(const JsonSpan& list, RuleScope& scope, uint16_t& first, uint8_t& count);
	bool trigger(const JsonSpan& json, RuleScope& scope, size_t record);
	bool timeline(const JsonSpan& json, RuleScope& scope, uint16_t& index);
	bool blocks(const JsonSpan& root);
	bool ui(const JsonSpan& root);
	/// The body of GET /api/pages for `pages` into the byte section `section` (`ui`, `ui.anonymous`).
	bool pages_body(const std::vector<JsonSpan>& pages, uint16_t section);
	/// The tables of the anonymous pages (9.16): what `pages` show.
	bool anonymous_tables(const std::vector<JsonSpan>& pages);
	void image_and_settings(const JsonSpan& root);
	void datapoint_table();
	bool assemble(std::vector<uint8_t>& out, const uint8_t* seed);

	// Expressions.
	uint16_t add_constant(const Val& value);
	uint16_t alloc_nodes(size_t count);
	void write_node(uint16_t index, const Node& node);
	Node read_node(uint16_t index);
	bool retype_enumeration(uint16_t index);
	bool root(const JsonSpan& json, RuleScope& scope, uint16_t& index, Type& type);
	bool expr(const JsonSpan& json, RuleScope& scope, uint16_t slot, Type& type, uint16_t& size);
	bool call(const JsonSpan& json, RuleScope& scope, uint16_t slot, Node& node, Type& type, uint16_t& size);

	struct StringFix {
		uint16_t section;
		size_t position;
		std::string text;
	};
	struct LabelFix {
		uint16_t section;
		size_t position;
		int label;
	};
	struct KnxDraft {
		bool present = false;
		uint16_t main = 0;
		uint16_t sub = 0;
		bool has_send = false;
		uint16_t send = 0;
		std::vector<uint16_t> listen;
	};
	struct ZigbeeDraft {
		bool present = false;
		uint64_t ieee = 0;
		uint16_t cluster = 0;
		uint16_t attribute = 0;
		uint16_t command = kNone;
		uint16_t report_min = 0;
		uint16_t report_max = 0;
		double report_change = 0;
		uint8_t endpoint = 0;
		uint8_t flags = 0;
	};
	struct HistoryDraft {
		bool present = false;
		uint32_t retention = 0;
		uint32_t spacing = 0;
		std::vector<std::pair<uint32_t, uint32_t>> archives; ///< resolution, retention
	};

	const blocks::BlockRegistry& registry_;
	bool harness_;
	JsonSpanDocument document_;
	std::map<uint16_t, Table> tables_;
	std::vector<StringFix> string_fixes_;
	std::vector<LabelFix> label_fixes_;
	std::vector<Label> labels_;
	std::vector<Datapoint> datapoints_;
	std::map<std::string, uint16_t> index_;
	size_t declared_ = 0;
	std::vector<KnxDraft> knx_;
	std::vector<ZigbeeDraft> zigbee_;
	std::vector<HistoryDraft> history_;
	uint32_t history_ram_ = 0;
	uint32_t history_flash_ = 0;
	// The time master (3.6): its datapoints by member, kNone without.
	bool has_time_master_ = false;
	uint16_t time_master_dp_[3] = {kNone, kNone, kNone};
	uint16_t time_master_address_[3] = {};
	// The rules: ids, whether they have states, their parameter and state datapoints.
	std::vector<std::string> rule_ids_;
	std::vector<bool> rule_states_;
	std::vector<std::vector<uint16_t>> rule_parameter_dps_;
	std::vector<uint16_t> rule_state_dp_;
	// The schedules: ids and time datapoints.
	std::vector<std::string> schedule_ids_;
	std::vector<uint16_t> schedule_time_dp_;
	// The blocks: their datapoints by member (kind << 8 | index).
	std::vector<std::map<uint16_t, uint16_t>> block_dps_;
	std::vector<std::string> function_ids_;
};

bool Compiler::fail(const char* format, ...) {
	if (error.empty()) {
		char buffer[256];
		va_list args;
		va_start(args, format);
		std::vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);
		error = buffer;
	}
	return false;
}

int Compiler::label(const JsonSpan& span) {
	Label label;
	if (span.kind == Kind::string) {
		label.emplace_back(std::string(), text_of(span));
	} else if (span.kind == Kind::object) {
		for (auto& m : members_of(span)) {
			label.emplace_back(m.first, text_of(m.second));
		}
	} else {
		return -1;
	}
	if (label.empty()) {
		return -1;
	}
	labels_.push_back(std::move(label));
	return static_cast<int>(labels_.size() - 1);
}

uint16_t Compiler::add_datapoint(const std::string& name, uint8_t type, uint8_t flags, uint8_t owner,
	uint16_t owner_index, uint16_t owner_member) {
	Datapoint dp;
	dp.name = name;
	dp.type = type;
	dp.flags = flags;
	dp.owner = owner;
	dp.owner_index = owner_index;
	dp.owner_member = owner_member;
	datapoints_.push_back(dp);
	return static_cast<uint16_t>(datapoints_.size() - 1);
}

// ---- Datapoints (3.3, 3.6, 9.7) ------------------------------------------------------------------

bool Compiler::declared_datapoints(const JsonSpan& root) {
	for (const JsonSpan& item : member_items(root, "datapoints")) {
		Datapoint dp;
		dp.name = member_text(item, "id");
		ValueType type = rules::datapoint_type_from_name(member_text(item, "type").c_str());
		if (dp.name.empty() || type == ValueType::none) {
			return fail("datapoints[%u]: no id or an unknown type", static_cast<unsigned>(datapoints_.size()));
		}
		dp.type = static_cast<uint8_t>(type);
		JsonSpan value;
		if (member(item, "label", value)) {
			dp.label = label(value);
		}
		dp.unit = member_text(item, "unit");
		dp.flags = static_cast<uint8_t>((member_flag(item, "readable", false) ? kReadable : 0) |
			(member_flag(item, "writable", false) ? kWritable : 0) | (member_flag(item, "transmit", false) ? kTransmit : 0) |
			(member_flag(item, "readOnStart", false) ? kReadOnStart : 0) |
			(member_flag(item, "persist", false) ? kPersist : 0));
		bool is_virtual = has_member(item, "virtual");
		bool technology = has_member(item, "knx") || has_member(item, "zigbee");
		if (is_virtual) {
			dp.flags = static_cast<uint8_t>(dp.flags | kApplication | kVirtual);
		} else if (!technology && !harness_) {
			// A value of the application (3.3, 1.6). The harness's test technology (13.1) has no flag.
			dp.flags = static_cast<uint8_t>(dp.flags | kApplication);
		}
		KnxDraft knx;
		JsonSpan knx_member;
		if (member(item, "knx", knx_member)) {
			knx.present = true;
			std::optional<Dpt> dpt = Dpt::parse(member_text(knx_member, "dpt").c_str());
			if (!dpt) {
				return fail("datapoints[%u].knx.dpt: not a DPT", static_cast<unsigned>(datapoints_.size()));
			}
			knx.main = dpt->main;
			knx.sub = dpt->sub;
			if (member(knx_member, "send", value)) {
				if (!group_address(text_of(value), knx.send)) {
					return fail("datapoints[%u].knx.send: not a group address", static_cast<unsigned>(datapoints_.size()));
				}
				knx.has_send = true;
			}
			for (const JsonSpan& address : member_items(knx_member, "listen")) {
				uint16_t a = 0;
				if (!group_address(text_of(address), a)) {
					return fail("datapoints[%u].knx.listen: not a group address", static_cast<unsigned>(datapoints_.size()));
				}
				// The listen addresses in the source's order, without `send` and each once (9.10).
				if (!(knx.has_send && a == knx.send) && std::find(knx.listen.begin(), knx.listen.end(), a) == knx.listen.end()) {
					knx.listen.push_back(a);
				}
			}
		}
		ZigbeeDraft zigbee;
		JsonSpan zigbee_member;
		if (member(item, "zigbee", zigbee_member)) {
			zigbee.present = true;
			if (!parse_ieee(member_text(zigbee_member, "ieee"), zigbee.ieee)) {
				return fail("datapoints[%u].zigbee.ieee: not an IEEE address", static_cast<unsigned>(datapoints_.size()));
			}
			zigbee.endpoint = static_cast<uint8_t>(member_number(zigbee_member, "endpoint"));
			zigbee.cluster = static_cast<uint16_t>(member_number(zigbee_member, "cluster"));
			zigbee.attribute = static_cast<uint16_t>(member_number(zigbee_member, "attribute"));
			if (member_flag(zigbee_member, "client", false)) {
				zigbee.flags |= 0x01;
				zigbee.attribute = 0;
			}
			if (member(zigbee_member, "command", value)) {
				zigbee.command = static_cast<uint16_t>(number_of(value));
			}
			JsonSpan reporting;
			if (member(zigbee_member, "reporting", reporting)) {
				if (member(reporting, "min", value)) {
					zigbee.flags |= 0x02;
					zigbee.report_min = static_cast<uint16_t>(number_of(value));
				}
				if (member(reporting, "max", value)) {
					zigbee.flags |= 0x04;
					zigbee.report_max = static_cast<uint16_t>(number_of(value));
				}
				if (member(reporting, "change", value)) {
					zigbee.flags |= 0x08;
					zigbee.report_change = number_of(value);
				}
			}
		}
		HistoryDraft history;
		JsonSpan history_member;
		if (member(item, "history", history_member)) {
			history.present = true;
			history.retention = static_cast<uint32_t>(member_number(history_member, "retention"));
			history.spacing = static_cast<uint32_t>(member_number(history_member, "spacing"));
			for (const JsonSpan& archive : member_items(history_member, "archives")) {
				history.archives.emplace_back(static_cast<uint32_t>(member_number(archive, "resolution")),
					static_cast<uint32_t>(member_number(archive, "retention")));
			}
		}
		datapoints_.push_back(dp);
		knx_.push_back(knx);
		zigbee_.push_back(zigbee);
		history_.push_back(history);
	}
	declared_ = datapoints_.size();
	return true;
}

void Compiler::time_master(const JsonSpan& root) {
	JsonSpan knx;
	JsonSpan tm;
	if (!member(root, "knx", knx) || !member(knx, "timeMaster", tm)) {
		return;
	}
	static const char* const kMembers[] = {"time", "date", "dateTime"};
	static const char* const kNames[] = {"knx.time", "knx.date", "knx.datetime"};
	static const ValueType kTypes[] = {ValueType::time_of_day, ValueType::date, ValueType::date_time};
	for (uint16_t m = 0; m < 3; ++m) {
		JsonSpan value;
		uint16_t address = 0;
		if (member(tm, kMembers[m], value) && group_address(text_of(value), address)) {
			time_master_address_[m] = address;
			// Readable (answers reads) and transmitting (doc/knx.md "Time master"); the time master
			// owns it through its KNX binding, set in knx_tables().
			time_master_dp_[m] = add_datapoint(kNames[m], static_cast<uint8_t>(kTypes[m]), kReadable | kTransmit,
				kOwnerTimeMaster, kNone, m);
			has_time_master_ = true;
		}
	}
}

void Compiler::rule_datapoints(const JsonSpan& root) {
	std::vector<JsonSpan> list = member_items(root, "rules");
	for (size_t r = 0; r < list.size(); ++r) {
		const JsonSpan& item = list[r];
		std::string id = member_text(item, "id");
		rule_ids_.push_back(id);
		bool has_states = !member_items(item, "states").empty();
		rule_states_.push_back(has_states);
		std::vector<uint16_t> dps;
		bool adjustable = member_flag(item, "enabledAdjustable", true);
		dps.push_back(add_datapoint("rule/" + id + "/enabled", static_cast<uint8_t>(ValueType::boolean),
			adjustable ? (kWritable | kTransmit) : 0, kOwnerRules, static_cast<uint16_t>(r), 0));
		bool state_parameter = false;
		uint16_t local = 1;
		for (const JsonSpan& p : member_items(item, "parameters")) {
			std::string pid = member_text(p, "id");
			state_parameter = state_parameter || pid == "state";
			ValueType type = ValueType::none;
			switch (rules::type_from_name(member_text(p, "type").c_str())) {
				case Type::boolean: type = ValueType::boolean; break;
				case Type::integer: type = ValueType::signed_int; break;
				case Type::real: type = ValueType::real; break;
				case Type::string: type = ValueType::string; break;
				case Type::duration: type = ValueType::unsigned_int; break;
				case Type::time_of_day: type = ValueType::time_of_day; break;
				case Type::enumeration: type = ValueType::enumeration; break;
				case Type::color: type = ValueType::color; break;
				default: break;
			}
			bool p_adjustable = member_flag(p, "adjustable", true);
			dps.push_back(add_datapoint("rule/" + id + "/" + pid, static_cast<uint8_t>(type),
				p_adjustable ? (kWritable | kTransmit) : 0, kOwnerRules, static_cast<uint16_t>(r), local++));
		}
		rule_parameter_dps_.push_back(dps);
		// The state datapoint (spec/rules.md 8.8), behind the parameters; a source before 1.7 that
		// declares a parameter `state` keeps that parameter's datapoint instead.
		uint16_t state = kNone;
		if (has_states && !state_parameter) {
			state = add_datapoint("rule/" + id + "/state", static_cast<uint8_t>(ValueType::enumeration), 0, kOwnerRules,
				static_cast<uint16_t>(r), 0x8000);
		}
		rule_state_dp_.push_back(state);
	}
}

void Compiler::schedule_datapoints(const JsonSpan& root) {
	std::vector<JsonSpan> list = member_items(root, "schedules");
	for (size_t s = 0; s < list.size(); ++s) {
		std::string id = member_text(list[s], "id");
		std::string kind = member_text(list[s], "kind");
		schedule_ids_.push_back(id);
		bool time = false;
		if (kind == "weekly" || kind == "yearly" || kind == "dates") {
			for (const JsonSpan& name : member_items(list[s], "adjustable")) {
				time = time || text_of(name) == "time";
			}
		}
		schedule_time_dp_.push_back(time ? add_datapoint("schedule/" + id + "/time",
			static_cast<uint8_t>(ValueType::time_of_day), kWritable | kTransmit, kOwnerScheduler,
			static_cast<uint16_t>(s), 1) : kNone);
	}
}

ValueType port_datapoint_type(blocks::PortType type) {
	switch (type) {
		case blocks::PortType::boolean: return ValueType::boolean;
		case blocks::PortType::integer: return ValueType::signed_int;
		case blocks::PortType::real: return ValueType::real;
		case blocks::PortType::enumeration: return ValueType::enumeration;
	}
	return ValueType::none;
}

ValueType param_datapoint_type(blocks::ParamType type) {
	switch (type) {
		case blocks::ParamType::boolean: return ValueType::boolean;
		case blocks::ParamType::integer: return ValueType::signed_int;
		case blocks::ParamType::real: return ValueType::real;
		case blocks::ParamType::duration: return ValueType::unsigned_int;
		case blocks::ParamType::time_of_day: return ValueType::time_of_day;
		case blocks::ParamType::enumeration: return ValueType::enumeration;
	}
	return ValueType::none;
}

/// The manifest of a block of the source at its major version; nullptr for a type this registry
/// does not have.
const blocks::Manifest* manifest_of(const blocks::BlockRegistry& registry, const JsonSpan& item) {
	uint32_t major = 0, minor = 0;
	if (!parse_version(member_text(item, "version"), major, minor)) {
		return nullptr;
	}
	const blocks::BlockType* type = registry.find(member_text(item, "type").c_str(), static_cast<uint16_t>(major));
	return type != nullptr ? &type->manifest() : nullptr;
}

void Compiler::block_datapoints(const JsonSpan& root) {
	std::vector<JsonSpan> list = member_items(root, "blocks");
	for (size_t b = 0; b < list.size(); ++b) {
		const JsonSpan& item = list[b];
		std::string id = member_text(item, "id");
		std::map<uint16_t, uint16_t> dps;
		uint8_t commandable = kWritable | kTransmit;
		uint16_t index = static_cast<uint16_t>(b);
		// Every block has its enable switch, also one of a type the registry lacks (the device refuses
		// it by its type, 10.8).
		dps[0] = add_datapoint("block/" + id + "/enabled", static_cast<uint8_t>(ValueType::boolean),
			member_flag(item, "enabledAdjustable", true) ? commandable : 0, kOwnerBlocks, index, 0);
		const blocks::Manifest* m = manifest_of(registry_, item);
		if (m != nullptr) {
			for (size_t p = 0; p < m->param_count; ++p) {
				if (m->params[p].adjustable) {
					uint16_t key = static_cast<uint16_t>(0x100 + p);
					dps[key] = add_datapoint("block/" + id + "/" + m->params[p].id,
						static_cast<uint8_t>(param_datapoint_type(m->params[p].type)), commandable, kOwnerBlocks, index, key);
				}
			}
			for (size_t p = 0; p < m->port_count; ++p) {
				if (m->ports[p].direction == blocks::Direction::out) {
					uint16_t key = static_cast<uint16_t>(0x200 + p);
					dps[key] = add_datapoint("block/" + id + "/" + m->ports[p].id,
						static_cast<uint8_t>(port_datapoint_type(m->ports[p].type)), 0, kOwnerBlocks, index, key);
				}
			}
			JsonSpan inputs;
			if (member(item, "inputs", inputs)) {
				for (size_t p = 0; p < m->port_count; ++p) {
					JsonSpan bound;
					if (m->ports[p].direction != blocks::Direction::in || !member(inputs, m->ports[p].id, bound) ||
						items(bound).size() < 2) {
						continue;
					}
					uint16_t key = static_cast<uint16_t>(0x300 + p);
					dps[key] = add_datapoint("block/" + id + "/" + m->ports[p].id,
						static_cast<uint8_t>(port_datapoint_type(m->ports[p].type)), 0, kOwnerBlocks, index, key);
				}
			}
		}
		block_dps_.push_back(dps);
	}
}

// ---- KNX, Zigbee, history (9.9 to 9.11) ----------------------------------------------------------

bool Compiler::knx_tables() {
	struct Binding {
		uint16_t datapoint;
		uint16_t main;
		uint16_t sub;
		bool has_send;
		uint16_t send;
		std::vector<uint16_t> listen;
		uint8_t owner;
	};
	std::vector<Binding> bindings;
	for (size_t i = 0; i < declared_; ++i) {
		if (knx_[i].present) {
			bindings.push_back(Binding{static_cast<uint16_t>(i), knx_[i].main, knx_[i].sub, knx_[i].has_send, knx_[i].send,
				knx_[i].listen, kOwnerKnx});
		}
	}
	static const uint16_t kDpt[3][2] = {{10, 1}, {11, 1}, {19, 1}};
	for (size_t m = 0; m < 3; ++m) {
		if (time_master_dp_[m] != kNone) {
			bindings.push_back(Binding{time_master_dp_[m], kDpt[m][0], kDpt[m][1], true, time_master_address_[m], {},
				kOwnerTimeMaster});
		}
	}
	std::set<uint16_t> address_set;
	for (const Binding& b : bindings) {
		if (b.has_send) {
			address_set.insert(b.send);
		}
		address_set.insert(b.listen.begin(), b.listen.end());
	}
	std::vector<uint16_t> addresses(address_set.begin(), address_set.end());
	auto address_index = [&addresses](uint16_t a) {
		return static_cast<uint16_t>(std::lower_bound(addresses.begin(), addresses.end(), a) - addresses.begin());
	};
	if (!bindings.empty()) {
		Table& t = table(kKnxBindings);
		Table& listen = table(kKnxListen);
		for (size_t k = 0; k < bindings.size(); ++k) {
			const Binding& b = bindings[k];
			size_t r = t.add();
			t.u16(r, 0, b.datapoint);
			t.u16(r, 2, b.main);
			t.u16(r, 4, b.sub);
			t.u16(r, 6, b.has_send ? address_index(b.send) : kNone);
			t.u16(r, 8, listen.count());
			t.u8(r, 10, b.listen.size());
			for (uint16_t a : b.listen) {
				size_t l = listen.add();
				listen.u16(l, 0, address_index(a));
			}
			Datapoint& dp = datapoints_[b.datapoint];
			dp.owner = b.owner;
			dp.owner_index = static_cast<uint16_t>(k);
		}
		Table& at = table(kKnxAddresses);
		Table& listeners = table(kKnxListeners);
		for (uint16_t a : addresses) {
			size_t r = at.add();
			at.u16(r, 0, a);
			at.u16(r, 2, listeners.count());
			size_t n = 0;
			for (size_t k = 0; k < bindings.size(); ++k) {
				const Binding& b = bindings[k];
				if ((b.has_send && b.send == a) || std::find(b.listen.begin(), b.listen.end(), a) != b.listen.end()) {
					size_t l = listeners.add();
					listeners.u16(l, 0, k);
					++n;
				}
			}
			at.u16(r, 4, n);
		}
	}
	return true;
}

void Compiler::zigbee_table() {
	for (size_t i = 0; i < declared_; ++i) {
		const ZigbeeDraft& z = zigbee_[i];
		if (!z.present) {
			continue;
		}
		Table& t = table(kZigbee);
		size_t r = t.add();
		t.u64(r, 0, z.ieee);
		t.u16(r, 8, i);
		t.u16(r, 10, z.cluster);
		t.u16(r, 12, z.attribute);
		t.u16(r, 14, z.command);
		t.u16(r, 16, z.report_min);
		t.u16(r, 18, z.report_max);
		t.u8(r, 20, z.endpoint);
		t.u8(r, 21, z.flags);
		t.f64(r, 24, z.report_change);
		datapoints_[i].owner = kOwnerZigbee;
		datapoints_[i].owner_index = static_cast<uint16_t>(r);
	}
}

void Compiler::history_tables() {
	for (size_t i = 0; i < declared_; ++i) {
		const HistoryDraft& h = history_[i];
		if (!h.present) {
			continue;
		}
		Table& t = table(kHistory);
		Table& archives = table(kArchives);
		size_t r = t.add();
		t.u16(r, 0, i);
		t.u8(r, 2, h.archives.size());
		t.u32(r, 4, h.retention);
		t.u32(r, 8, h.spacing);
		t.u16(r, 12, archives.count());
		// What they need (3.7): 8 bytes a ring sample; per archive its open slot and an hour of closed
		// ones in RAM (32 bytes each), and its segments of 127 slots in flash (4 KB each).
		history_ram_ += 8 * (ceil_div(h.retention, h.spacing) + 1);
		for (const auto& a : h.archives) {
			size_t k = archives.add();
			archives.u32(k, 0, a.first);
			archives.u32(k, 4, a.second);
			history_ram_ += (ceil_div(3600, a.first) + 2) * 32;
			history_flash_ += ceil_div(ceil_div(a.second, a.first) + 1, 127) * 4096;
		}
		datapoints_[i].history = static_cast<uint16_t>(r);
		datapoints_[i].flags = static_cast<uint8_t>(datapoints_[i].flags | kHistoryFlag);
	}
}

// ---- Locations, functions, roles (9.8) -----------------------------------------------------------

bool Compiler::locations_and_functions(const JsonSpan& root) {
	static const char* const kLocationKinds[] = {"building", "floor", "room", "area", "outside"};
	std::vector<JsonSpan> locations = member_items(root, "locations");
	std::vector<std::string> ids;
	for (const JsonSpan& item : locations) {
		ids.push_back(member_text(item, "id"));
	}
	auto location = [&ids](const std::string& id) {
		auto it = std::find(ids.begin(), ids.end(), id);
		return it == ids.end() ? kNone : static_cast<uint16_t>(it - ids.begin());
	};
	for (const JsonSpan& item : locations) {
		Table& t = table(kLocations);
		size_t r = t.add();
		put_string(t, r, 0, member_text(item, "id"));
		JsonSpan value;
		if (member(item, "label", value)) {
			put_label(t, r, 4, label(value));
		}
		t.u16(r, 8, has_member(item, "parent") ? location(member_text(item, "parent")) : kNone);
		t.u8(r, 10, index_of(kLocationKinds, 5, member_text(item, "kind")) + 1);
	}
	// The roles of each kind (3.5): a role the kind does not have is not compiled.
	static const char* const kKinds[] = {"light", "dimmer", "blind", "switch", "sensor", "climate", "scene", "button"};
	static const std::vector<std::vector<std::string>> kKindRoles = {
		{"command", "state"},
		{"command", "state", "levelCommand", "level", "step"},
		{"move", "stop", "position", "positionCommand", "slat", "slatCommand", "atTop", "atBottom"},
		{"command", "state"},
		{"value"},
		{"temperature", "setpoint", "setpointCommand", "mode", "modeCommand", "heating", "heatingLevel"},
		{"command"},
		{"press", "state"},
	};
	static const char* const kPresses[] = {"press", "on", "off", "up", "down", "release"};
	for (const JsonSpan& item : member_items(root, "functions")) {
		Table& t = table(kFunctions);
		Table& roles = table(kRoles);
		size_t r = t.add();
		std::string id = member_text(item, "id");
		function_ids_.push_back(id);
		put_string(t, r, 0, id);
		JsonSpan value;
		if (member(item, "label", value)) {
			put_label(t, r, 4, label(value));
		}
		put_string(t, r, 8, member_text(item, "unit"));
		t.u16(r, 12, has_member(item, "room") ? location(member_text(item, "room")) : kNone);
		t.u16(r, 14, roles.count());
		int kind = index_of(kKinds, 8, member_text(item, "kind"));
		if (kind < 0) {
			return fail("functions[%u].kind: unknown", static_cast<unsigned>(r));
		}
		t.u8(r, 16, kind);
		t.u8(r, 17, kind == 6 ? static_cast<uint8_t>(member_number(item, "scene")) : 0);
		size_t count = 0;
		JsonSpan datapoints;
		if (member(item, "datapoints", datapoints)) {
			for (auto& m : members_of(datapoints)) {
				const std::vector<std::string>& known = kKindRoles[static_cast<size_t>(kind)];
				if (std::find(known.begin(), known.end(), m.first) == known.end()) {
					continue;
				}
				int dp = datapoint(text_of(m.second));
				if (dp < 0) {
					return fail("functions[%u].datapoints.%s: unknown datapoint", static_cast<unsigned>(r), m.first.c_str());
				}
				size_t k = roles.add();
				put_string(roles, k, 0, m.first);
				roles.u16(k, 4, dp);
				++count;
			}
		}
		t.u8(r, 18, count);
		JsonSpan presses;
		if (member(item, "presses", presses)) {
			for (size_t p = 0; p < 6; ++p) {
				uint8_t bits = 0;
				for (const JsonSpan& e : member_items(presses, kPresses[p])) {
					double element = number_of(e, -1);
					if (element >= 0 && element <= 5) {
						bits = static_cast<uint8_t>(bits | (1u << static_cast<unsigned>(element)));
					}
				}
				t.u8(r, 20 + p, bits);
			}
		}
	}
	return true;
}

// ---- External values, MQTT (9.11) ----------------------------------------------------------------

bool Compiler::external(const JsonSpan& root) {
	JsonSpan member_value;
	if (!member(root, "external", member_value)) {
		return true;
	}
	for (const JsonSpan& source : member_items(member_value, "sources")) {
		Table& t = table(kExternalSources);
		Table& bindings = table(kExternalBindings);
		size_t r = t.add();
		put_string(t, r, 0, member_text(source, "id"));
		put_string(t, r, 4, member_text(source, "url"));
		put_string(t, r, 8, member_text(source, "preset"));
		t.u32(r, 12, static_cast<uint32_t>(member_number(source, "interval", 900)));
		t.u16(r, 16, bindings.count());
		size_t n = 0;
		for (const JsonSpan& binding : member_items(source, "bindings")) {
			int dp = datapoint(member_text(binding, "datapoint"));
			if (dp < 0) {
				return fail("external.sources[%u].bindings[%u].datapoint: unknown datapoint", static_cast<unsigned>(r),
					static_cast<unsigned>(n));
			}
			size_t k = bindings.add();
			bindings.u16(k, 0, dp);
			put_string(bindings, k, 4, member_text(binding, "path"));
			put_string(bindings, k, 8, member_text(binding, "variable"));
			put_string(bindings, k, 12, member_text(binding, "unit"));
			bindings.f64(k, 16, member_number(binding, "scale", 1));
			bindings.f64(k, 24, member_number(binding, "offset", 0));
			datapoints_[static_cast<size_t>(dp)].owner = kOwnerExternal;
			datapoints_[static_cast<size_t>(dp)].owner_index = static_cast<uint16_t>(k);
			datapoints_[static_cast<size_t>(dp)].owner_member = 0;
			++n;
		}
		t.u8(r, 18, n);
	}
	return true;
}

void Compiler::mqtt(const JsonSpan& root) {
	JsonSpan m;
	if (!member(root, "mqtt", m)) {
		return;
	}
	Table& t = table(kMqtt);
	size_t r = t.add();
	put_string(t, r, 0, member_text(m, "broker"));
	put_string(t, r, 4, member_text(m, "username"));
	put_string(t, r, 8, member_text(m, "baseTopic"));
	JsonSpan discovery;
	JsonSpan history;
	bool has_discovery = member(m, "discovery", discovery);
	bool has_history = member(m, "history", history);
	put_string(t, r, 12, has_discovery ? member_text(discovery, "prefix") : std::string());
	t.u16(r, 16, static_cast<uint16_t>(has_history ? member_number(history, "interval", 15) : 15));
	uint8_t flags = 0;
	flags |= member_flag(m, "enabled", true) ? 0x01 : 0;
	flags |= member_flag(m, "retain", true) ? 0x02 : 0;
	flags |= !has_discovery || member_flag(discovery, "enabled", true) ? 0x04 : 0;
	flags |= !has_history || member_flag(history, "enabled", true) ? 0x08 : 0;
	t.u8(r, 18, flags);
}

// ---- Schedules (9.12) ----------------------------------------------------------------------------

bool Compiler::schedules(const JsonSpan& root) {
	static const char* const kKinds[] = {"weekly", "yearly", "dates", "astro", "interval", "once"};
	static const char* const kSuns[] = {"dawn", "sunrise", "sunset", "dusk"};
	static const char* const kMembers[] = {"time", "days", "offset", "notBefore", "notAfter", "every", "from", "until",
		"at"};
	std::vector<JsonSpan> list = member_items(root, "schedules");
	for (size_t s = 0; s < list.size(); ++s) {
		const JsonSpan& item = list[s];
		Table& t = table(kSchedules);
		Table& dates = table(kScheduleDates);
		Table& bounds = table(kScheduleBounds);
		size_t r = t.add();
		put_string(t, r, 0, member_text(item, "id"));
		put_string(t, r, 4, member_text(item, "label"));
		int kind = index_of(kKinds, 6, member_text(item, "kind"));
		if (kind < 0) {
			return fail("schedules[%u].kind: unknown", static_cast<unsigned>(s));
		}
		t.u8(r, 8, kind);
		JsonSpan value;
		uint8_t flags = 0;
		flags |= member_flag(item, "enabled", true) ? 0x01 : 0;
		flags |= member_flag(item, "enabledAdjustable", true) ? 0x02 : 0;
		flags |= member_text(item, "missed") == "catchUp" ? 0x04 : 0;
		bool uses_days = kind == 0 || kind == 3 || kind == 4;
		if (uses_days) {
			uint8_t days = 0x7F;
			if (member(item, "days", value)) {
				days = 0;
				for (const JsonSpan& d : items(value)) {
					days = static_cast<uint8_t>(days | (1u << (static_cast<unsigned>(number_of(d, 1)) - 1)));
				}
			}
			t.u8(r, 10, days);
		}
		if (kind == 3) {
			t.u8(r, 11, std::max(0, index_of(kSuns, 4, member_text(item, "sun"))));
		}
		uint16_t output = kNone;
		if (member(item, "output", value)) {
			int dp = datapoint(text_of(value));
			if (dp < 0) {
				return fail("schedules[%u].output: unknown datapoint", static_cast<unsigned>(s));
			}
			output = static_cast<uint16_t>(dp);
			datapoints_[output].owner = kOwnerScheduler;
			datapoints_[output].owner_index = static_cast<uint16_t>(s);
			datapoints_[output].owner_member = 0;
		}
		t.u16(r, 12, output);
		t.u16(r, 14, schedule_time_dp_[s]);
		uint16_t adjustable = 0;
		for (const JsonSpan& name : member_items(item, "adjustable")) {
			int m = index_of(kMembers, 9, text_of(name));
			if (m >= 0) {
				adjustable = static_cast<uint16_t>(adjustable | (1u << m));
			}
		}
		t.u16(r, 16, adjustable);
		if (kind == 4) {
			t.u16(r, 18, static_cast<uint16_t>(member_number(item, "every") / 60000));
			t.u32(r, 48, member(item, "from", value) ? seconds_of_clock(value) : 0);
			t.u32(r, 52, member(item, "until", value) ? seconds_of_clock(value) : 0);
		}
		t.u16(r, 20, dates.count());
		size_t date_count = 0;
		if (kind == 2) {
			for (const JsonSpan& d : member_items(item, "dates")) {
				size_t k = dates.add();
				dates.u16(k, 0, static_cast<uint16_t>(member_number(d, "year")));
				dates.u8(k, 2, static_cast<uint8_t>(member_number(d, "month")));
				dates.u8(k, 3, static_cast<uint8_t>(member_number(d, "day")));
				++date_count;
			}
		}
		t.u16(r, 22, date_count);
		t.u16(r, 24, bounds.count());
		size_t bound_count = 0;
		JsonSpan bound_member;
		if (member(item, "bounds", bound_member)) {
			for (auto& b : members_of(bound_member)) {
				int m = index_of(kMembers, 9, b.first);
				if (m < 0) {
					continue;
				}
				uint16_t bit = static_cast<uint16_t>(1u << m);
				bool time = (bit & (0x001 | 0x008 | 0x010 | 0x040 | 0x080)) != 0;
				// The member's scalar (scheduler.h ScheduleBound): seconds of the day for a time, seconds
				// for `offset` and `every`, local seconds since 1970 for `at`; absent bounds its range.
				int64_t min = time ? 0 : bit == 0x004 ? -43200 : bit == 0x020 ? 60 : 0;
				int64_t max = time ? 86399 : bit == 0x004 ? 43200 : bit == 0x020 ? 43200
					: rules::days_of(2155, 12, 31) * 86400 + 86399;
				auto scalar = [&](const JsonSpan& v) -> int64_t {
					if (time) {
						return seconds_of_clock(v);
					}
					if (bit == 0x100) {
						return rules::days_of(static_cast<int32_t>(member_number(v, "year")),
							static_cast<unsigned>(member_number(v, "month")), static_cast<unsigned>(member_number(v, "day"))) *
							86400 + seconds_of_clock(v);
					}
					return static_cast<int64_t>(number_of(v)) / 1000;
				};
				int64_t step = 0;
				if (member(b.second, "min", value)) {
					min = scalar(value);
				}
				if (member(b.second, "max", value)) {
					max = scalar(value);
				}
				if (member(b.second, "step", value)) {
					step = time ? static_cast<int64_t>(number_of(value)) : static_cast<int64_t>(number_of(value)) / 1000;
				}
				size_t k = bounds.add();
				bounds.u16(k, 0, bit);
				bounds.i64(k, 8, min);
				bounds.i64(k, 16, max);
				bounds.i64(k, 24, step);
				++bound_count;
			}
		}
		t.u8(r, 26, bound_count);
		if (kind == 1) {
			t.u8(r, 27, static_cast<uint8_t>(member_number(item, "month")));
			t.u8(r, 28, static_cast<uint8_t>(member_number(item, "day")));
		}
		if (kind == 3) {
			t.i32(r, 32, static_cast<int64_t>(member_number(item, "offset")) / 1000);
			if (member(item, "notBefore", value)) {
				flags |= 0x08;
				t.u32(r, 40, seconds_of_clock(value));
			}
			if (member(item, "notAfter", value)) {
				flags |= 0x10;
				t.u32(r, 44, seconds_of_clock(value));
			}
		}
		if (kind <= 2 && member(item, "time", value)) {
			t.u32(r, 36, seconds_of_clock(value));
		}
		if (kind == 5 && member(item, "at", value)) {
			t.u16(r, 56, static_cast<uint16_t>(member_number(value, "year")));
			t.u8(r, 58, static_cast<uint8_t>(member_number(value, "month")));
			t.u8(r, 59, static_cast<uint8_t>(member_number(value, "day")));
			t.u32(r, 60, seconds_of_clock(value));
		}
		t.u8(r, 9, flags);
	}
	return true;
}

// ---- Expressions (9.14) --------------------------------------------------------------------------

uint16_t Compiler::add_constant(const Val& v) {
	Table& t = table(kConstants);
	size_t r = t.add();
	t.u8(r, 0, static_cast<uint8_t>(v.type));
	t.u8(r, 1, 1);
	switch (v.type) {
		case Type::boolean: t.u8(r, 8, v.u.b ? 1 : 0); break;
		case Type::integer:
		case Type::duration: t.i64(r, 8, v.u.i); break;
		case Type::real: t.f64(r, 8, v.u.r); break;
		case Type::enumeration: t.u32(r, 8, v.u.e); break;
		case Type::time_of_day:
			t.u8(r, 8, v.u.tod.day_of_week);
			t.u8(r, 9, v.u.tod.hour);
			t.u8(r, 10, v.u.tod.minute);
			t.u8(r, 11, v.u.tod.second);
			break;
		case Type::date:
			t.u16(r, 8, v.u.date.year);
			t.u8(r, 10, v.u.date.month);
			t.u8(r, 11, v.u.date.day);
			break;
		case Type::date_time:
			t.u16(r, 8, v.u.dt.year);
			t.u8(r, 10, v.u.dt.month);
			t.u8(r, 11, v.u.dt.day);
			t.u8(r, 12, v.u.dt.day_of_week);
			t.u8(r, 13, v.u.dt.hour);
			t.u8(r, 14, v.u.dt.minute);
			t.u8(r, 15, v.u.dt.second);
			t.u16(r, 16, v.u.dt.flags);
			break;
		case Type::controlled:
			t.u8(r, 8, v.u.ctl.control ? 1 : 0);
			t.u8(r, 9, v.u.ctl.value);
			break;
		case Type::color:
			t.u8(r, 8, v.u.color.red);
			t.u8(r, 9, v.u.color.green);
			t.u8(r, 10, v.u.color.blue);
			t.u8(r, 11, v.u.color.white);
			t.u8(r, 12, v.u.color.valid);
			break;
		case Type::string:
		case Type::raw:
			t.u8(r, 2, v.length);
			std::memcpy(t.at(r, 8), v.u.bytes, v.length);
			break;
		default: break;
	}
	return static_cast<uint16_t>(r);
}

uint16_t Compiler::alloc_nodes(size_t count) {
	Table& t = table(kNodes);
	uint16_t first = static_cast<uint16_t>(t.count());
	for (size_t i = 0; i < count; ++i) {
		t.add();
	}
	return first;
}

void Compiler::write_node(uint16_t index, const Node& node) {
	Table& t = table(kNodes);
	t.u8(index, 0, node.kind);
	t.u8(index, 1, node.op);
	t.u8(index, 2, node.type);
	t.u8(index, 3, node.argc);
	t.u16(index, 4, node.a);
	t.u16(index, 6, node.size);
	std::memcpy(t.at(index, 8), node.lit, 8);
}

Node Compiler::read_node(uint16_t index) {
	Table& t = table(kNodes);
	Node node;
	node.kind = t.get8(index, 0);
	node.op = t.get8(index, 1);
	node.type = t.get8(index, 2);
	node.argc = t.get8(index, 3);
	node.a = t.get16(index, 4);
	node.size = t.get16(index, 6);
	std::memcpy(node.lit, t.at(index, 8), 8);
	return node;
}

/// An integer literal that is an enumeration literal by its partner (spec/rules.md 4.8).
bool Compiler::retype_enumeration(uint16_t index) {
	Node node = read_node(index);
	int64_t value = 0;
	std::memcpy(&value, node.lit, sizeof(value)); // little-endian host, as the image
	if (node.kind != kLiteral || node.type != static_cast<uint8_t>(Type::integer) || value < 0 ||
		value > 4294967295LL) {
		return false;
	}
	uint32_t element = static_cast<uint32_t>(value);
	std::memset(node.lit, 0, sizeof(node.lit));
	for (size_t i = 0; i < 4; ++i) {
		node.lit[i] = static_cast<uint8_t>(element >> (8 * i));
	}
	node.type = static_cast<uint8_t>(Type::enumeration);
	write_node(index, node);
	return true;
}

bool Compiler::root(const JsonSpan& json, RuleScope& scope, uint16_t& index, Type& type) {
	index = alloc_nodes(1);
	uint16_t size = 0;
	return expr(json, scope, index, type, size);
}

void put_lit64(Node& node, uint64_t value) {
	for (size_t i = 0; i < 8; ++i) {
		node.lit[i] = static_cast<uint8_t>(value >> (8 * i));
	}
}

bool Compiler::expr(const JsonSpan& json, RuleScope& scope, uint16_t slot, Type& type, uint16_t& size) {
	std::string kind = member_text(json, "kind");
	Node node;
	size = 1;
	if (kind == "literal") {
		Type t = rules::type_from_name(member_text(json, "type").c_str());
		JsonSpan value_json;
		Val value;
		if (t == Type::invalid || !member(json, "value", value_json) ||
			rules::val_from_json(t, document_.value(value_json), value) != rules::LiteralStatus::ok) {
			return fail("rule '%s': a literal that is no value of its type", scope.id.c_str());
		}
		if (t == Type::enumeration) {
			// An enumeration literal is the integer of its element (4.8); its partner retypes it.
			value = Val::of_int(value.u.e);
			t = Type::integer;
		}
		node.kind = kLiteral;
		node.type = static_cast<uint8_t>(t);
		switch (t) {
			case Type::controlled: {
				// controlled(bit, n) (4.8).
				uint16_t children = alloc_nodes(2);
				Node bit;
				bit.kind = kLiteral;
				bit.type = static_cast<uint8_t>(Type::boolean);
				bit.lit[0] = value.u.ctl.control ? 1 : 0;
				Node number;
				number.kind = kLiteral;
				number.type = static_cast<uint8_t>(Type::integer);
				put_lit64(number, value.u.ctl.value);
				write_node(children, bit);
				write_node(static_cast<uint16_t>(children + 1), number);
				node.kind = kCall;
				node.op = static_cast<uint8_t>(Fn::controlled);
				node.argc = 2;
				node.a = children;
				size = 3;
				break;
			}
			case Type::boolean: node.lit[0] = value.u.b ? 1 : 0; break;
			case Type::integer:
			case Type::duration: put_lit64(node, static_cast<uint64_t>(value.u.i)); break;
			case Type::real: {
				uint64_t bits = 0;
				std::memcpy(&bits, &value.u.r, sizeof(bits));
				put_lit64(node, bits);
				break;
			}
			case Type::time_of_day:
				node.lit[0] = value.u.tod.day_of_week;
				node.lit[1] = value.u.tod.hour;
				node.lit[2] = value.u.tod.minute;
				node.lit[3] = value.u.tod.second;
				break;
			case Type::date:
				node.lit[0] = static_cast<uint8_t>(value.u.date.year);
				node.lit[1] = static_cast<uint8_t>(value.u.date.year >> 8);
				node.lit[2] = value.u.date.month;
				node.lit[3] = value.u.date.day;
				break;
			case Type::color:
				node.lit[0] = value.u.color.red;
				node.lit[1] = value.u.color.green;
				node.lit[2] = value.u.color.blue;
				node.lit[3] = value.u.color.white;
				node.lit[4] = value.u.color.valid;
				break;
			default: // string, raw, dateTime: a constant, written when the node is
				node.a = add_constant(value);
				break;
		}
		type = t;
	} else if (kind == "datapoint") {
		int dp = datapoint(member_text(json, "name"));
		if (dp < 0) {
			return fail("rule '%s': no datapoint '%s'", scope.id.c_str(), member_text(json, "name").c_str());
		}
		node.kind = kDatapointNode;
		type = rules::type_of_datapoint(static_cast<ValueType>(datapoints_[static_cast<size_t>(dp)].type));
		node.type = static_cast<uint8_t>(type);
		node.a = static_cast<uint16_t>(dp);
	} else if (kind == "parameter") {
		std::string id = member_text(json, "id");
		auto it = std::find(scope.parameters.begin(), scope.parameters.end(), id);
		if (it == scope.parameters.end()) {
			return fail("rule '%s': no parameter '%s'", scope.id.c_str(), id.c_str());
		}
		size_t p = static_cast<size_t>(it - scope.parameters.begin());
		node.kind = kParameterNode;
		type = scope.parameter_types[p];
		node.type = static_cast<uint8_t>(type);
		node.a = static_cast<uint16_t>(p);
	} else if (kind == "timer") {
		std::string id = member_text(json, "id");
		auto it = std::find(scope.timers.begin(), scope.timers.end(), id);
		if (it == scope.timers.end()) {
			return fail("rule '%s': no timer '%s'", scope.id.c_str(), id.c_str());
		}
		node.kind = kTimerNode;
		node.type = 0; // a timer has no expression type; 0 written (see the notes of the report)
		node.a = static_cast<uint16_t>(it - scope.timers.begin());
		type = Type::invalid;
	} else if (kind == "unary") {
		std::string op = member_text(json, "op");
		JsonSpan operand;
		if (!member(json, "operand", operand) || (op != "-" && op != "not")) {
			return fail("rule '%s': a malformed unary node", scope.id.c_str());
		}
		if (op == "-") {
			// A chain of minus signs ending in an integer, real or duration literal is that literal
			// negated (4.8).
			JsonSpan inner = operand;
			size_t minus = 1;
			while (member_text(inner, "kind") == "unary" && member_text(inner, "op") == "-") {
				JsonSpan next;
				if (!member(inner, "operand", next)) {
					break;
				}
				inner = next;
				++minus;
			}
			std::string inner_type = member_text(inner, "type");
			if (member_text(inner, "kind") == "literal" &&
				(inner_type == "integer" || inner_type == "real" || inner_type == "duration")) {
				if (!expr(inner, scope, slot, type, size)) {
					return false;
				}
				if (minus % 2 != 0) {
					Node literal = read_node(slot);
					if (type == Type::real) {
						double r = 0;
						std::memcpy(&r, literal.lit, sizeof(r));
						r = -r;
						uint64_t bits = 0;
						std::memcpy(&bits, &r, sizeof(bits));
						put_lit64(literal, bits);
					} else {
						int64_t i = 0;
						std::memcpy(&i, literal.lit, sizeof(i));
						put_lit64(literal, static_cast<uint64_t>(-i));
					}
					write_node(slot, literal);
				}
				return true;
			}
		}
		uint16_t child = alloc_nodes(1);
		Type t = Type::invalid;
		uint16_t child_size = 0;
		if (!expr(operand, scope, child, t, child_size)) {
			return false;
		}
		node.kind = kUnary;
		node.op = static_cast<uint8_t>(op == "-" ? Op::neg : Op::not_);
		node.type = static_cast<uint8_t>(t);
		node.argc = 1;
		node.a = child;
		size = static_cast<uint16_t>(1 + child_size);
		type = t;
	} else if (kind == "binary") {
		std::string op_text = member_text(json, "op");
		int found = -1;
		for (size_t i = static_cast<size_t>(Op::or_); i <= static_cast<size_t>(Op::mod); ++i) {
			if (op_text == kOperatorTexts[i]) {
				found = static_cast<int>(i);
				break;
			}
		}
		JsonSpan left, right;
		if (found < 0 || !member(json, "left", left) || !member(json, "right", right)) {
			return fail("rule '%s': a malformed binary node", scope.id.c_str());
		}
		Op o = static_cast<Op>(found);
		uint16_t children = alloc_nodes(2);
		Type a = Type::invalid, b = Type::invalid;
		uint16_t size_a = 0, size_b = 0;
		if (!expr(left, scope, children, a, size_a) ||
			!expr(right, scope, static_cast<uint16_t>(children + 1), b, size_b)) {
			return false;
		}
		if (o == Op::eq || o == Op::ne) {
			if (a == Type::enumeration && retype_enumeration(static_cast<uint16_t>(children + 1))) {
				b = Type::enumeration;
			} else if (b == Type::enumeration && retype_enumeration(children)) {
				a = Type::enumeration;
			}
		}
		Type t = Type::invalid;
		switch (o) {
			case Op::or_:
			case Op::and_:
			case Op::eq:
			case Op::ne:
			case Op::lt:
			case Op::le:
			case Op::gt:
			case Op::ge: t = Type::boolean; break;
			case Op::add:
			case Op::sub:
				if (numeric(a) && numeric(b)) {
					t = a == Type::integer && b == Type::integer ? Type::integer : Type::real;
				} else if (a == Type::duration && b == Type::duration) {
					t = Type::duration;
				} else if ((a == Type::time_of_day || a == Type::date_time) && b == Type::duration) {
					t = a;
				} else if (o == Op::sub && a == b &&
					(a == Type::time_of_day || a == Type::date_time || a == Type::date)) {
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
			return fail("rule '%s': operator %s on these types", scope.id.c_str(), op_text.c_str());
		}
		node.kind = kBinary;
		node.op = static_cast<uint8_t>(o);
		node.type = static_cast<uint8_t>(t);
		node.argc = 2;
		node.a = children;
		size = static_cast<uint16_t>(1 + size_a + size_b);
		type = t;
	} else if (kind == "call") {
		if (!call(json, scope, slot, node, type, size)) {
			return false;
		}
	} else {
		return fail("rule '%s': unknown expression kind '%s'", scope.id.c_str(), kind.c_str());
	}
	node.size = size;
	write_node(slot, node);
	return true;
}

bool Compiler::call(const JsonSpan& json, RuleScope& scope, uint16_t slot, Node& node, Type& type, uint16_t& size) {
	(void)slot;
	std::string name = member_text(json, "function");
	int fn_index = index_of(kFunctionNames, static_cast<size_t>(Fn::count), name);
	if (fn_index < 0) {
		return fail("rule '%s': unknown function '%s'", scope.id.c_str(), name.c_str());
	}
	Fn fn = static_cast<Fn>(fn_index);
	std::vector<JsonSpan> args = member_items(json, "args");
	size_t argc = args.size();
	if (argc > 8) {
		return fail("rule '%s': more than 8 arguments", scope.id.c_str());
	}
	uint32_t state_rule = static_cast<uint32_t>(scope.index);
	if (fn == Fn::state && argc == 1) {
		std::string id = member_text(args[0], "value");
		auto it = std::find(rule_ids_.begin(), rule_ids_.end(), id);
		if (it == rule_ids_.end()) {
			return fail("rule '%s': state() of no rule '%s'", scope.id.c_str(), id.c_str());
		}
		state_rule = static_cast<uint32_t>(it - rule_ids_.begin());
	}
	uint16_t children = argc > 0 ? alloc_nodes(argc) : kNone;
	Type t[8] = {};
	uint16_t total = 1;
	for (size_t i = 0; i < argc; ++i) {
		uint16_t child_size = 0;
		if (!expr(args[i], scope, static_cast<uint16_t>(children + i), t[i], child_size)) {
			return false;
		}
		total = static_cast<uint16_t>(total + child_size);
	}
	// An integer literal beside an enumeration in if() and coalesce(), and the argument of toInt(),
	// is an enumeration literal (4.8).
	auto partners = [&](size_t x, size_t y) {
		if (t[x] == Type::enumeration && retype_enumeration(static_cast<uint16_t>(children + y))) {
			t[y] = Type::enumeration;
		} else if (t[y] == Type::enumeration && retype_enumeration(static_cast<uint16_t>(children + x))) {
			t[x] = Type::enumeration;
		}
	};
	if (fn == Fn::if_ && argc == 3) {
		partners(1, 2);
	} else if (fn == Fn::coalesce && argc == 2) {
		partners(0, 1);
	} else if (fn == Fn::to_int && argc == 1 && retype_enumeration(children)) {
		t[0] = Type::enumeration;
	}
	Type result = Type::invalid;
	switch (fn) {
		case Fn::if_: result = argc == 3 ? common_any(t[1], t[2]) : Type::invalid; break;
		case Fn::coalesce: result = argc == 2 ? common_any(t[0], t[1]) : Type::invalid; break;
		case Fn::known:
		case Fn::between:
		case Fn::hysteresis:
		case Fn::in_window:
		case Fn::changed_within:
		case Fn::running:
		case Fn::contains:
		case Fn::starts_with:
		case Fn::ends_with:
		case Fn::control_bit: result = Type::boolean; break;
		case Fn::abs:
		case Fn::last_known: result = argc > 0 ? t[0] : Type::invalid; break;
		case Fn::min:
		case Fn::max:
		case Fn::clamp: {
			Type c = argc > 0 ? t[0] : Type::invalid;
			for (size_t k = 1; k < argc && c != Type::invalid; ++k) {
				c = common_ordered(c, t[k]);
			}
			result = c;
			break;
		}
		case Fn::round:
		case Fn::floor:
		case Fn::ceil:
		case Fn::trunc:
		case Fn::to_int:
		case Fn::weekday:
		case Fn::hour:
		case Fn::minute:
		case Fn::second:
		case Fn::year:
		case Fn::month:
		case Fn::day:
		case Fn::length:
		case Fn::red:
		case Fn::green:
		case Fn::blue:
		case Fn::white:
		case Fn::control_value: result = Type::integer; break;
		case Fn::round_to:
		case Fn::sun_azimuth:
		case Fn::sun_elevation: result = Type::real; break;
		case Fn::now: result = Type::date_time; break;
		case Fn::today:
		case Fn::date_of: result = Type::date; break;
		case Fn::time:
		case Fn::time_of:
		case Fn::sunrise:
		case Fn::sunset:
		case Fn::dawn:
		case Fn::dusk: result = Type::time_of_day; break;
		case Fn::since_update:
		case Fn::since_change:
		case Fn::remaining: result = Type::duration; break;
		case Fn::quality:
		case Fn::state:
		case Fn::to_string: result = Type::string; break;
		case Fn::rgb:
		case Fn::rgbw: result = Type::color; break;
		case Fn::controlled: result = Type::controlled; break;
		case Fn::count: break;
	}
	if (result == Type::invalid) {
		return fail("rule '%s': %s() on these types", scope.id.c_str(), name.c_str());
	}
	node.kind = kCall;
	node.op = static_cast<uint8_t>(fn);
	node.type = static_cast<uint8_t>(result);
	node.argc = static_cast<uint8_t>(argc);
	node.a = children;
	if (fn == Fn::state) {
		for (size_t i = 0; i < 4; ++i) {
			node.lit[i] = static_cast<uint8_t>(state_rule >> (8 * i));
		}
	}
	type = result;
	size = total;
	return true;
}

// ---- Rules (9.13) --------------------------------------------------------------------------------

bool Compiler::rules(const JsonSpan& root) {
	std::vector<JsonSpan> list = member_items(root, "rules");
	for (size_t r = 0; r < list.size(); ++r) {
		if (!rule(list[r], r)) {
			return false;
		}
	}
	return true;
}

bool Compiler::rule(const JsonSpan& json, size_t index) {
	Table& rules_table = table(kRules);
	size_t record = rules_table.add();
	RuleScope scope;
	scope.index = index;
	scope.id = rule_ids_[index];
	put_string(rules_table, record, 0, scope.id);
	JsonSpan value;
	if (member(json, "name", value)) {
		put_label(rules_table, record, 4, label(value));
	}

	// Parameters: `enabled` first, then the declared ones, each with its constants (default, min,
	// max, step, the choices' values; 9.18).
	Table& parameters = table(kParameters);
	rules_table.u16(record, 8, parameters.count());
	{
		size_t p = parameters.add();
		put_string(parameters, p, 0, "enabled");
		parameters.u8(p, 12, static_cast<uint8_t>(Type::boolean));
		parameters.u8(p, 13, member_flag(json, "enabledAdjustable", true) ? 1 : 0);
		parameters.u16(p, 16, table(kChoices).count());
		parameters.u16(p, 18, add_constant(Val::of_bool(member_flag(json, "enabled", true))));
		parameters.u16(p, 20, kNone);
		parameters.u16(p, 22, kNone);
		parameters.u16(p, 24, kNone);
		parameters.u16(p, 26, rule_parameter_dps_[index][0]);
		scope.parameters.push_back("enabled");
		scope.parameter_types.push_back(Type::boolean);
	}
	std::vector<JsonSpan> declared = member_items(json, "parameters");
	for (size_t k = 0; k < declared.size(); ++k) {
		const JsonSpan& item = declared[k];
		size_t p = parameters.add();
		std::string id = member_text(item, "id");
		Type type = rules::type_from_name(member_text(item, "type").c_str());
		if (type == Type::invalid) {
			return fail("rule '%s': parameter '%s' of an unknown type", scope.id.c_str(), id.c_str());
		}
		put_string(parameters, p, 0, id);
		if (member(item, "label", value)) {
			put_label(parameters, p, 4, label(value));
		}
		put_string(parameters, p, 8, member_text(item, "unit"));
		parameters.u8(p, 12, static_cast<uint8_t>(type));
		parameters.u8(p, 13, member_flag(item, "adjustable", true) ? 1 : 0);
		auto constant = [&](const char* name, uint16_t& out) {
			JsonSpan v;
			out = kNone;
			if (!member(item, name, v)) {
				return true;
			}
			Val parsed;
			if (rules::val_from_json(type, document_.value(v), parsed) != rules::LiteralStatus::ok) {
				return fail("rule '%s': parameter '%s': %s is no value of its type", scope.id.c_str(), id.c_str(), name);
			}
			out = add_constant(parsed);
			return true;
		};
		uint16_t default_value = kNone, min = kNone, max = kNone, step = kNone;
		if (!constant("default", default_value) || !constant("min", min) || !constant("max", max)) {
			return false;
		}
		if (default_value == kNone) {
			return fail("rule '%s': parameter '%s' has no default", scope.id.c_str(), id.c_str());
		}
		JsonSpan step_json;
		if (member(item, "step", step_json)) {
			// A hint for the UI: a duration of a duration (or time of day) parameter in ms, an integer of
			// an integer parameter where it is whole, else a real.
			double number = number_of(step_json);
			if (type == Type::duration || type == Type::time_of_day) {
				step = add_constant(Val::of_duration(static_cast<int64_t>(number)));
			} else if (type == Type::integer && number == std::floor(number)) {
				step = add_constant(Val::of_int(static_cast<int64_t>(number)));
			} else {
				step = add_constant(Val::of_real(number));
			}
		}
		parameters.u16(p, 18, default_value);
		parameters.u16(p, 20, min);
		parameters.u16(p, 22, max);
		parameters.u16(p, 24, step);
		Table& choices = table(kChoices);
		parameters.u16(p, 16, choices.count());
		size_t choice_count = 0;
		for (const JsonSpan& choice : member_items(item, "choices")) {
			JsonSpan choice_value;
			Val parsed;
			if (!member(choice, "value", choice_value) ||
				rules::val_from_json(type, document_.value(choice_value), parsed) != rules::LiteralStatus::ok) {
				return fail("rule '%s': parameter '%s': a choice is no value of its type", scope.id.c_str(), id.c_str());
			}
			uint16_t constant_index = add_constant(parsed);
			size_t c = choices.add();
			choices.u16(c, 0, constant_index);
			if (member(choice, "label", value)) {
				put_label(choices, c, 4, label(value));
			}
			++choice_count;
		}
		parameters.u8(p, 14, choice_count);
		parameters.u16(p, 26, k + 1 < rule_parameter_dps_[index].size() ? rule_parameter_dps_[index][k + 1] : kNone);
		scope.parameters.push_back(id);
		scope.parameter_types.push_back(type);
	}
	rules_table.u8(record, 16, scope.parameters.size());

	// The ids of the timers and states first: every expression may name any of them.
	std::vector<JsonSpan> timers = member_items(json, "timers");
	std::vector<JsonSpan> states = member_items(json, "states");
	for (const JsonSpan& t : timers) {
		scope.timers.push_back(member_text(t, "id"));
	}
	for (const JsonSpan& s : states) {
		scope.states.push_back(member_text(s, "id"));
	}
	auto state_index = [&scope](const std::string& id) {
		auto it = std::find(scope.states.begin(), scope.states.end(), id);
		return it == scope.states.end() ? -1 : static_cast<int>(it - scope.states.begin());
	};

	// Timers, each followed by its duration's expression.
	Table& timer_table = table(kTimers);
	rules_table.u16(record, 10, timer_table.count());
	rules_table.u8(record, 17, timers.size());
	for (size_t k = 0; k < timers.size(); ++k) {
		size_t t = timer_table.add();
		put_string(timer_table, t, 0, scope.timers[k]);
		JsonSpan duration;
		uint16_t root_index = kNone;
		Type type = Type::invalid;
		if (!member(timers[k], "duration", duration) || !root(duration, scope, root_index, type)) {
			return error.empty() ? fail("rule '%s': a timer without duration", scope.id.c_str()) : false;
		}
		table(kTimers).u16(t, 4, root_index);
	}

	// States, each with its entry actions, then its exit actions.
	Table& state_table = table(kStates);
	rules_table.u16(record, 12, state_table.count());
	rules_table.u8(record, 18, states.size());
	size_t state_base = state_table.count();
	for (size_t k = 0; k < states.size(); ++k) {
		state_table.add();
	}
	for (size_t k = 0; k < states.size(); ++k) {
		size_t s = state_base + k;
		put_string(table(kStates), s, 0, scope.states[k]);
		if (member(states[k], "label", value)) {
			put_label(table(kStates), s, 4, label(value));
		}
		uint16_t first = 0;
		uint8_t count = 0;
		JsonSpan list;
		if (member(states[k], "entry", list)) {
			if (!action_list(list, scope, first, count)) {
				return false;
			}
		} else {
			first = static_cast<uint16_t>(table(kActions).count());
		}
		table(kStates).u16(s, 8, first);
		table(kStates).u8(s, 10, count);
		count = 0;
		if (member(states[k], "exit", list)) {
			if (!action_list(list, scope, first, count)) {
				return false;
			}
		} else {
			first = static_cast<uint16_t>(table(kActions).count());
		}
		table(kStates).u16(s, 12, first);
		table(kStates).u8(s, 14, count);
	}
	int initial = state_index(member_text(json, "initialState"));
	table(kRules).u8(record, 20, states.empty() || initial < 0 ? 0xFF : initial);

	// Reactions, each with its triggers (and their expressions), its condition, its actions and its
	// else actions.
	std::vector<JsonSpan> reactions = member_items(json, "reactions");
	size_t reaction_base = table(kReactions).count();
	table(kRules).u16(record, 14, reaction_base);
	table(kRules).u8(record, 19, reactions.size());
	for (size_t k = 0; k < reactions.size(); ++k) {
		table(kReactions).add();
	}
	for (size_t k = 0; k < reactions.size(); ++k) {
		const JsonSpan& reaction = reactions[k];
		size_t rr = reaction_base + k;
		std::vector<JsonSpan> triggers = member_items(reaction, "triggers");
		size_t trigger_base = table(kTriggers).count();
		table(kReactions).u16(rr, 0, trigger_base);
		table(kReactions).u8(rr, 2, triggers.size());
		for (size_t i = 0; i < triggers.size(); ++i) {
			table(kTriggers).add();
		}
		for (size_t i = 0; i < triggers.size(); ++i) {
			if (!trigger(triggers[i], scope, trigger_base + i)) {
				return false;
			}
		}
		uint8_t to = 0xFF;
		if (member(reaction, "to", value)) {
			int s = state_index(text_of(value));
			to = s < 0 ? 0xFF : static_cast<uint8_t>(s);
		}
		table(kReactions).u8(rr, 3, to);
		uint16_t condition = kNone;
		if (member(reaction, "condition", value)) {
			Type type = Type::invalid;
			if (!root(value, scope, condition, type)) {
				return false;
			}
		}
		table(kReactions).u16(rr, 4, condition);
		uint16_t from = 0xFFFF;
		if (member(reaction, "from", value)) {
			from = 0;
			for (const JsonSpan& s : items(value)) {
				int i = state_index(text_of(s));
				if (i >= 0) {
					from = static_cast<uint16_t>(from | (1u << i));
				}
			}
		}
		table(kReactions).u16(rr, 6, from);
		uint16_t first = static_cast<uint16_t>(table(kActions).count());
		uint8_t count = 0;
		if (member(reaction, "actions", value) && !action_list(value, scope, first, count)) {
			return false;
		}
		table(kReactions).u16(rr, 8, first);
		table(kReactions).u8(rr, 10, count);
		first = static_cast<uint16_t>(table(kActions).count());
		count = 0;
		if (member(reaction, "elseActions", value) && !action_list(value, scope, first, count)) {
			return false;
		}
		table(kReactions).u16(rr, 12, first);
		table(kReactions).u8(rr, 14, count);
	}

	// The timeline: its plans with their points, then its assignments.
	uint16_t timeline_index = kNone;
	if (member(json, "timeline", value) && !timeline(value, scope, timeline_index)) {
		return false;
	}
	table(kRules).u16(record, 22, timeline_index);
	table(kRules).u16(record, 24, rule_state_dp_[index]);
	return true;
}

bool Compiler::action_list(const JsonSpan& list, RuleScope& scope, uint16_t& first, uint8_t& count) {
	std::vector<JsonSpan> actions = items(list);
	size_t base = table(kActions).count();
	first = static_cast<uint16_t>(base);
	count = static_cast<uint8_t>(actions.size());
	// The list's actions together, then the log parts and expressions of each action in order (9.18).
	for (size_t i = 0; i < actions.size(); ++i) {
		table(kActions).add();
	}
	for (size_t i = 0; i < actions.size(); ++i) {
		const JsonSpan& json = actions[i];
		size_t a = base + i;
		std::string kind = member_text(json, "kind");
		JsonSpan value;
		Type type = Type::invalid;
		uint16_t expr_index = kNone;
		if (kind == "write") {
			int dp = datapoint(member_text(json, "datapoint"));
			if (dp < 0) {
				return fail("rule '%s': write to no datapoint '%s'", scope.id.c_str(), member_text(json, "datapoint").c_str());
			}
			if (!member(json, "value", value) || !root(value, scope, expr_index, type)) {
				return error.empty() ? fail("rule '%s': a write without value", scope.id.c_str()) : false;
			}
			table(kActions).u8(a, 0, 0);
			table(kActions).u8(a, 1, member_flag(json, "onlyOnChange", false) ? 1 : 0);
			table(kActions).u16(a, 2, dp);
			table(kActions).u16(a, 4, expr_index);
		} else if (kind == "startTimer" || kind == "restartTimer" || kind == "cancelTimer") {
			std::string id = member_text(json, "timer");
			auto it = std::find(scope.timers.begin(), scope.timers.end(), id);
			if (it == scope.timers.end()) {
				return fail("rule '%s': no timer '%s'", scope.id.c_str(), id.c_str());
			}
			if (kind != "cancelTimer" && member(json, "duration", value) && !root(value, scope, expr_index, type)) {
				return false;
			}
			table(kActions).u8(a, 0, kind == "startTimer" ? 1 : kind == "restartTimer" ? 2 : 3);
			table(kActions).u16(a, 2, it - scope.timers.begin());
			table(kActions).u16(a, 4, expr_index);
		} else if (kind == "setParameter") {
			std::string id = member_text(json, "parameter");
			auto it = std::find(scope.parameters.begin(), scope.parameters.end(), id);
			if (it == scope.parameters.end()) {
				return fail("rule '%s': no parameter '%s'", scope.id.c_str(), id.c_str());
			}
			size_t p = static_cast<size_t>(it - scope.parameters.begin());
			if (!member(json, "value", value) || !root(value, scope, expr_index, type)) {
				return error.empty() ? fail("rule '%s': setParameter without value", scope.id.c_str()) : false;
			}
			// The value takes the type of the enumeration parameter it is assigned to (4.8).
			if (scope.parameter_types[p] == Type::enumeration) {
				retype_enumeration(expr_index);
			}
			table(kActions).u8(a, 0, 4);
			table(kActions).u16(a, 2, p);
			table(kActions).u16(a, 4, expr_index);
		} else if (kind == "log") {
			static const char* const kLevels[] = {"info", "warning", "error"};
			std::vector<JsonSpan> parts = member_items(json, "message");
			Table& log_parts = table(kLogParts);
			size_t part_base = log_parts.count();
			table(kActions).u8(a, 0, 5);
			table(kActions).u8(a, 1, std::max(0, index_of(kLevels, 3, member_text(json, "level"))));
			table(kActions).u16(a, 2, part_base);
			table(kActions).u16(a, 4, kNone);
			table(kActions).u16(a, 6, parts.size());
			for (size_t k = 0; k < parts.size(); ++k) {
				table(kLogParts).add();
			}
			for (size_t k = 0; k < parts.size(); ++k) {
				size_t lp = part_base + k;
				if (parts[k].kind == Kind::string) {
					put_string(table(kLogParts), lp, 0, text_of(parts[k]));
					table(kLogParts).u16(lp, 4, kNone);
				} else {
					uint16_t part_root = kNone;
					if (!root(parts[k], scope, part_root, type)) {
						return false;
					}
					table(kLogParts).u16(lp, 4, part_root);
				}
			}
		} else if (kind == "read") {
			int dp = datapoint(member_text(json, "datapoint"));
			if (dp < 0) {
				return fail("rule '%s': read of no datapoint '%s'", scope.id.c_str(), member_text(json, "datapoint").c_str());
			}
			table(kActions).u8(a, 0, 6);
			table(kActions).u16(a, 2, dp);
			table(kActions).u16(a, 4, kNone);
		} else {
			return fail("rule '%s': unknown action kind '%s'", scope.id.c_str(), kind.c_str());
		}
	}
	return true;
}

bool Compiler::trigger(const JsonSpan& json, RuleScope& scope, size_t record) {
	std::string kind = member_text(json, "kind");
	uint8_t code = 0;
	uint8_t flags = 0;
	uint16_t ref = kNone;
	uint16_t ref2 = kNone;
	JsonSpan value;
	Type type = Type::invalid;
	if (kind == "datapoint" || kind == "silence") {
		int dp = datapoint(member_text(json, "datapoint"));
		if (dp < 0) {
			return fail("rule '%s': trigger on no datapoint '%s'", scope.id.c_str(), member_text(json, "datapoint").c_str());
		}
		ref = static_cast<uint16_t>(dp);
		if (kind == "datapoint") {
			code = 0;
			flags |= member_text(json, "on") == "update" ? 0x01 : 0;
			std::string origin = member_text(json, "origin");
			flags |= origin == "report" ? 0x02 : origin == "command" ? 0x04 : 0;
		} else {
			code = 6;
			if (!member(json, "after", value) || !root(value, scope, ref2, type)) {
				return error.empty() ? fail("rule '%s': silence without after", scope.id.c_str()) : false;
			}
		}
	} else if (kind == "match") {
		code = 1;
		if (!member(json, "condition", value) || !root(value, scope, ref, type)) {
			return error.empty() ? fail("rule '%s': match without condition", scope.id.c_str()) : false;
		}
	} else if (kind == "schedule") {
		code = 2;
		std::string id = member_text(json, "schedule");
		auto it = std::find(schedule_ids_.begin(), schedule_ids_.end(), id);
		if (it == schedule_ids_.end()) {
			return fail("rule '%s': no schedule '%s'", scope.id.c_str(), id.c_str());
		}
		ref = static_cast<uint16_t>(it - schedule_ids_.begin());
	} else if (kind == "timer") {
		code = 3;
		std::string id = member_text(json, "timer");
		auto it = std::find(scope.timers.begin(), scope.timers.end(), id);
		if (it == scope.timers.end()) {
			return fail("rule '%s': no timer '%s'", scope.id.c_str(), id.c_str());
		}
		ref = static_cast<uint16_t>(it - scope.timers.begin());
	} else if (kind == "start") {
		code = 4;
		flags = 0x07;
		if (member(json, "reasons", value)) {
			static const char* const kReasons[] = {"boot", "image", "enable"};
			flags = 0;
			for (const JsonSpan& reason : items(value)) {
				int bit = index_of(kReasons, 3, text_of(reason));
				if (bit >= 0) {
					flags = static_cast<uint8_t>(flags | (1u << bit));
				}
			}
		}
	} else if (kind == "parameter") {
		code = 5;
		std::string id = member_text(json, "parameter");
		auto it = std::find(scope.parameters.begin(), scope.parameters.end(), id);
		if (it == scope.parameters.end()) {
			return fail("rule '%s': no parameter '%s'", scope.id.c_str(), id.c_str());
		}
		ref = static_cast<uint16_t>(it - scope.parameters.begin());
	} else {
		return fail("rule '%s': unknown trigger kind '%s'", scope.id.c_str(), kind.c_str());
	}
	Table& t = table(kTriggers);
	t.u8(record, 0, code);
	t.u8(record, 1, flags);
	t.u16(record, 2, ref);
	t.u16(record, 4, ref2);
	return true;
}

bool Compiler::timeline(const JsonSpan& json, RuleScope& scope, uint16_t& index) {
	static const char* const kSuns[] = {"dawn", "sunrise", "sunset", "dusk"};
	Table& timelines = table(kTimelines);
	size_t t = timelines.add();
	index = static_cast<uint16_t>(t);
	std::vector<JsonSpan> plans = member_items(json, "plans");
	std::vector<JsonSpan> assignments = member_items(json, "assignments");
	std::vector<std::string> plan_ids;
	timelines.u16(t, 0, table(kPlans).count());
	timelines.u16(t, 2, table(kAssignments).count());
	timelines.u8(t, 4, plans.size());
	timelines.u8(t, 5, assignments.size());
	timelines.u8(t, 6, member_flag(json, "adjustable", true) ? 1 : 0);
	for (const JsonSpan& plan : plans) {
		size_t p = table(kPlans).add();
		plan_ids.push_back(member_text(plan, "id"));
		put_string(table(kPlans), p, 0, plan_ids.back());
		JsonSpan value;
		if (member(plan, "label", value)) {
			put_label(table(kPlans), p, 4, label(value));
		}
		std::vector<JsonSpan> points = member_items(plan, "points");
		table(kPlans).u16(p, 8, table(kPoints).count());
		table(kPlans).u8(p, 10, points.size());
		for (const JsonSpan& point : points) {
			Table& pt = table(kPoints);
			size_t k = pt.add();
			auto it = std::find(scope.states.begin(), scope.states.end(), member_text(point, "state"));
			if (it == scope.states.end()) {
				return fail("rule '%s': a timeline point names no state of the rule", scope.id.c_str());
			}
			pt.u8(k, 0, it - scope.states.begin());
			uint8_t flags = 0;
			if (member(point, "time", value)) {
				pt.i32(k, 4, seconds_of_clock(value));
			} else {
				pt.u8(k, 1, 1 + std::max(0, index_of(kSuns, 4, member_text(point, "sun"))));
				pt.i32(k, 4, static_cast<int64_t>(member_number(point, "offset")) / 1000);
				if (member(point, "notBefore", value)) {
					flags |= 0x01;
					pt.i32(k, 8, seconds_of_clock(value));
				}
				if (member(point, "notAfter", value)) {
					flags |= 0x02;
					pt.i32(k, 12, seconds_of_clock(value));
				}
			}
			pt.u8(k, 2, flags);
		}
	}
	for (const JsonSpan& assignment : assignments) {
		Table& at = table(kAssignments);
		size_t k = at.add();
		auto it = std::find(plan_ids.begin(), plan_ids.end(), member_text(assignment, "plan"));
		if (it == plan_ids.end()) {
			return fail("rule '%s': an assignment names no plan of the timeline", scope.id.c_str());
		}
		at.u8(k, 0, it - plan_ids.begin());
		uint8_t days = 0;
		for (const JsonSpan& d : member_items(assignment, "days")) {
			days = static_cast<uint8_t>(days | (1u << (static_cast<unsigned>(number_of(d, 1)) - 1)));
		}
		at.u8(k, 1, days);
	}
	return true;
}

// ---- Blocks (9.15) -------------------------------------------------------------------------------

/// A parameter's value from the source (spec/blocks.md 4): a choice by its value, a duration in ms, a
/// time of day in seconds since midnight, a boolean 0 or 1.
bool block_parameter(const blocks::ParamDecl& decl, const JsonSpan& json, double& out) {
	if (decl.type == blocks::ParamType::boolean) {
		bool b = false;
		if (json.kind != Kind::boolean || !json_bool(json, b)) {
			return false;
		}
		out = b ? 1 : 0;
		return true;
	}
	if (decl.type == blocks::ParamType::time_of_day) {
		if (json.kind != Kind::object) {
			return false;
		}
		out = seconds_of_clock(json);
		return true;
	}
	if (json.kind == Kind::string) {
		std::string id = text_of(json);
		for (size_t c = 0; c < decl.choice_count; ++c) {
			if (id == decl.choices[c].id) {
				out = decl.choices[c].value;
				return true;
			}
		}
		return false;
	}
	return json.kind == Kind::number && json_number(json, out);
}

bool Compiler::blocks(const JsonSpan& root) {
	static const char* const kAggregations[] = {"mean", "min", "max", "any", "all"};
	std::vector<JsonSpan> list = member_items(root, "blocks");
	for (size_t b = 0; b < list.size(); ++b) {
		const JsonSpan& item = list[b];
		Table& t = table(kBlocks);
		size_t r = t.add();
		std::string id = member_text(item, "id");
		put_string(t, r, 0, id);
		put_string(t, r, 4, member_text(item, "type"));
		JsonSpan value;
		if (member(item, "label", value)) {
			put_label(t, r, 8, label(value));
		}
		uint32_t major = 0, minor = 0;
		parse_version(member_text(item, "version"), major, minor);
		t.u16(r, 12, major);
		t.u16(r, 14, minor);
		t.u16(r, 16, table(kPorts).count());
		t.u16(r, 18, table(kBlockParameters).count());
		uint8_t flags = 0;
		flags |= member_flag(item, "enabled", true) ? 0x01 : 0;
		flags |= member_flag(item, "enabledAdjustable", true) ? 0x02 : 0;
		t.u8(r, 22, flags);
		const std::map<uint16_t, uint16_t>& dps = block_dps_[b];
		t.u16(r, 24, dps.at(0));
		const blocks::Manifest* m = manifest_of(registry_, item);
		if (m == nullptr) {
			// A type this registry does not have: the device refuses it by its type (10.8); the block
			// has its enable switch and neither ports nor parameters.
			continue;
		}
		JsonSpan inputs, outputs, aggregation, parameters;
		member(item, "inputs", inputs);
		member(item, "outputs", outputs);
		member(item, "aggregation", aggregation);
		member(item, "parameters", parameters);
		// The ports in the manifest's order, bound or not.
		for (size_t p = 0; p < m->port_count; ++p) {
			const blocks::PortDecl& decl = m->ports[p];
			Table& ports = table(kPorts);
			size_t k = ports.add();
			put_string(ports, k, 0, decl.id);
			ports.u16(k, 4, table(kBound).count());
			std::vector<std::string> names;
			JsonSpan bound;
			if (member(decl.direction == blocks::Direction::in ? inputs : outputs, decl.id, bound)) {
				if (bound.kind == Kind::array) {
					for (const JsonSpan& name : items(bound)) {
						names.push_back(text_of(name));
					}
				} else {
					names.push_back(text_of(bound));
				}
			}
			for (const std::string& name : names) {
				int dp = datapoint(name);
				if (dp < 0) {
					return fail("blocks[%u].%s: no datapoint '%s'", static_cast<unsigned>(b), decl.id, name.c_str());
				}
				size_t q = table(kBound).add();
				table(kBound).u16(q, 0, dp);
			}
			table(kPorts).u8(k, 6, names.size());
			uint8_t aggregate = 0;
			uint16_t mirror = kNone;
			if (decl.direction == blocks::Direction::out) {
				mirror = dps.at(static_cast<uint16_t>(0x200 + p));
			} else if (names.size() > 1) {
				mirror = dps.at(static_cast<uint16_t>(0x300 + p));
				int a = index_of(kAggregations, 5, member_text(aggregation, decl.id));
				aggregate = a >= 0 ? static_cast<uint8_t>(a + 1) : static_cast<uint8_t>(decl.default_aggregation);
			}
			table(kPorts).u8(k, 7, aggregate);
			table(kPorts).u16(k, 8, mirror);
		}
		table(kBlocks).u8(r, 20, m->port_count);
		// The parameters in the manifest's order.
		for (size_t p = 0; p < m->param_count; ++p) {
			const blocks::ParamDecl& decl = m->params[p];
			Table& params = table(kBlockParameters);
			size_t k = params.add();
			put_string(params, k, 0, decl.id);
			params.u16(k, 4, decl.adjustable ? dps.at(static_cast<uint16_t>(0x100 + p)) : kNone);
			JsonSpan given;
			if (member(parameters, decl.id, given)) {
				double v = 0;
				if (!block_parameter(decl, given, v)) {
					return fail("blocks[%u].parameters.%s: not a value of the parameter", static_cast<unsigned>(b), decl.id);
				}
				params.u8(k, 6, 1);
				params.f64(k, 8, v);
			}
		}
		table(kBlocks).u8(r, 21, m->param_count);
	}
	return true;
}

// ---- The control pages (9.16: the body of GET /api/pages) --------------------------------------

void write_label(JsonWriter& json, const Label& label) {
	if (label.size() == 1 && label[0].first.empty()) {
		json.string(label[0].second.c_str(), label[0].second.size());
		return;
	}
	json.begin_object();
	for (const auto& entry : label) {
		json.key(entry.first.c_str()).string(entry.second.c_str(), entry.second.size());
	}
	json.end_object();
}

bool Compiler::ui(const JsonSpan& root) {
	JsonSpan ui_member;
	std::vector<JsonSpan> pages;
	if (member(root, "ui", ui_member)) {
		pages = member_items(ui_member, "pages");
	}
	if (pages.empty()) {
		return true; // no control pages: no section (the device shows its overview)
	}
	if (!pages_body(pages, kUi)) {
		return false;
	}
	// The pages of minimum role anonymous (3.9, 9.16, format 1.1): their own body and what they show.
	std::vector<JsonSpan> anonymous;
	for (size_t p = 0; p < pages.size(); ++p) {
		std::string role = member_text(pages[p], "minimumRole");
		if (role == "anonymous") {
			anonymous.push_back(pages[p]);
		} else if (!role.empty() && role != "operator") {
			return fail("ui.pages[%u].minimumRole: '%s' is neither anonymous nor operator", static_cast<unsigned>(p),
				role.c_str());
		}
	}
	if (anonymous.empty()) {
		return true;
	}
	return pages_body(anonymous, kUiAnonymous) && anonymous_tables(anonymous);
}

bool Compiler::anonymous_tables(const std::vector<JsonSpan>& pages) {
	std::map<uint16_t, uint8_t> datapoints; // index -> flags (bit 0 operate)
	std::set<uint16_t> functions;
	std::string failure;
	auto show = [&](const JsonSpan& object, bool operate) {
		int dp = datapoint(member_text(object, "datapoint"));
		if (dp < 0) {
			failure = member_text(object, "datapoint");
			return;
		}
		datapoints[static_cast<uint16_t>(dp)] |= operate ? 1 : 0;
	};
	for (const JsonSpan& page : pages) {
		for (const JsonSpan& section : member_items(page, "sections")) {
			for (const JsonSpan& element : member_items(section, "elements")) {
				JsonSpan chart;
				if (has_member(element, "function")) {
					std::string id = member_text(element, "function");
					auto it = std::find(function_ids_.begin(), function_ids_.end(), id);
					if (it == function_ids_.end()) {
						return fail("ui: no function '%s'", id.c_str());
					}
					uint16_t f = static_cast<uint16_t>(it - function_ids_.begin());
					functions.insert(f);
					// Every role of the function, operated: the tile is the function's control.
					Table& t = table(kFunctions);
					Table& roles = table(kRoles);
					size_t first = t.get16(f, 14);
					size_t count = t.get8(f, 18);
					for (size_t k = first; k < first + count; ++k) {
						datapoints[roles.get16(k, 4)] |= 1;
					}
				} else if (has_member(element, "datapoint")) {
					show(element, !member_flag(element, "readOnly", false));
				} else if (member(element, "chart", chart)) {
					for (const JsonSpan& series : member_items(chart, "series")) {
						show(series, false);
					}
					for (const JsonSpan& reference : member_items(chart, "references")) {
						if (has_member(reference, "datapoint")) {
							show(reference, false);
						}
					}
				}
			}
		}
	}
	if (!failure.empty()) {
		return fail("ui: no datapoint '%s'", failure.c_str());
	}
	Table& t = table(kUiAnonymousDatapoints);
	for (const auto& entry : datapoints) {
		size_t r = t.add();
		t.u16(r, 0, entry.first);
		t.u8(r, 2, entry.second);
	}
	Table& f = table(kUiAnonymousFunctions);
	for (uint16_t index : functions) {
		f.u16(f.add(), 0, index);
	}
	return true;
}

bool Compiler::pages_body(const std::vector<JsonSpan>& pages, uint16_t section) {
	static const char* const kRanges[] = {"1h", "24h", "7d", "30d", "1y"};
	auto parse_label = [](const JsonSpan& object, Label& out) {
		out.clear();
		JsonSpan value;
		if (!member(object, "label", value)) {
			return false;
		}
		if (value.kind == Kind::string) {
			out.emplace_back(std::string(), text_of(value));
		} else {
			for (auto& m : members_of(value)) {
				out.emplace_back(m.first, text_of(m.second));
			}
		}
		return !out.empty();
	};
	for (int pass = 0; pass < 2; ++pass) {
		std::vector<char> buffer;
		JsonWriter counter = JsonWriter::counter();
		JsonWriter real(nullptr, 0);
		if (pass == 1) {
			buffer.resize(table(section).bytes.size() + 1);
			real = JsonWriter(buffer.data(), buffer.size());
		}
		JsonWriter& json = pass == 0 ? counter : real;
		Label label_value;
		std::string failure;
		auto datapoint_of = [&](const JsonSpan& object) -> int {
			int dp = datapoint(member_text(object, "datapoint"));
			if (dp < 0 && failure.empty()) {
				failure = member_text(object, "datapoint");
			}
			return dp < 0 ? 0 : dp;
		};
		json.begin_object();
		json.key("pages").begin_array();
		for (const JsonSpan& page : pages) {
			json.begin_object().member("id", member_text(page, "id").c_str());
			if (parse_label(page, label_value)) {
				json.key("label");
				write_label(json, label_value);
			}
			if (has_member(page, "icon")) {
				json.member("icon", member_text(page, "icon").c_str());
			}
			json.member("sections", static_cast<uint32_t>(member_items(page, "sections").size())).end_object();
		}
		json.end_array();
		json.key("sections").begin_array();
		for (size_t p = 0; p < pages.size(); ++p) {
			for (const JsonSpan& section : member_items(pages[p], "sections")) {
				json.begin_object().member("page", static_cast<uint32_t>(p));
				if (parse_label(section, label_value)) {
					json.key("label");
					write_label(json, label_value);
				}
				json.member("elements", static_cast<uint32_t>(member_items(section, "elements").size())).end_object();
			}
		}
		json.end_array();
		std::vector<JsonSpan> charts;      // the chart members, by element
		std::vector<uint32_t> chart_elements;
		json.key("elements").begin_array();
		uint32_t section_index = 0;
		uint32_t element_index = 0;
		for (const JsonSpan& page : pages) {
			for (const JsonSpan& section : member_items(page, "sections")) {
				for (const JsonSpan& element : member_items(section, "elements")) {
					json.begin_object().member("section", section_index);
					JsonSpan chart;
					if (has_member(element, "function")) {
						json.member("function", member_text(element, "function").c_str());
					} else if (has_member(element, "datapoint")) {
						json.member("datapoint", static_cast<uint32_t>(datapoint_of(element)));
						if (member_flag(element, "readOnly", false)) {
							json.member("readOnly", true);
						}
					} else if (member(element, "chart", chart)) {
						charts.push_back(chart);
						chart_elements.push_back(element_index);
						json.key("chart").begin_object();
						if (parse_label(chart, label_value)) {
							json.key("label");
							write_label(json, label_value);
						}
						JsonSpan value;
						json.key("width");
						if (member(chart, "width", value) && value.kind == Kind::number) {
							json.number(static_cast<uint32_t>(number_of(value)));
						} else {
							json.string("full");
						}
						json.member("height", static_cast<uint32_t>(member_number(chart, "height", 2)));
						int range = has_member(chart, "range") ? index_of(kRanges, 5, member_text(chart, "range")) : 1;
						range = range < 0 ? 1 : range;
						json.member("range", kRanges[range]);
						unsigned presets = 0;
						for (const JsonSpan& preset : member_items(chart, "presets")) {
							int k = index_of(kRanges, 5, text_of(preset));
							presets |= k >= 0 ? 1u << k : 0u;
						}
						if (presets == 0) {
							presets = 1u << range;
						}
						json.key("presets").begin_array();
						for (size_t k = 0; k < 5; ++k) {
							if ((presets & (1u << k)) != 0) {
								json.string(kRanges[k]);
							}
						}
						json.end_array();
						json.key("axes").begin_object();
						JsonSpan axes;
						bool has_axes = member(chart, "axes", axes);
						for (const char* side : {"left", "right"}) {
							json.key(side).begin_object();
							JsonSpan limits;
							bool has_limits = has_axes && member(axes, side, limits);
							for (const char* bound : {"min", "max"}) {
								json.key(bound);
								if (has_limits && member(limits, bound, value) && value.kind == Kind::number) {
									json.number(number_of(value));
								} else {
									json.null();
								}
							}
							json.end_object();
						}
						json.end_object().end_object();
					}
					// The element's label behind its target (E-Home's member order).
					if (parse_label(element, label_value)) {
						json.key("label");
						write_label(json, label_value);
					}
					json.end_object();
					++element_index;
				}
				++section_index;
			}
		}
		json.end_array();
		json.key("series").begin_array();
		for (size_t c = 0; c < charts.size(); ++c) {
			for (const JsonSpan& series : member_items(charts[c], "series")) {
				int dp = datapoint_of(series);
				json.begin_object().member("element", chart_elements[c]).member("datapoint", static_cast<uint32_t>(dp));
				if (parse_label(series, label_value)) {
					json.key("label");
					write_label(json, label_value);
				}
				json.member("axis", member_text(series, "axis") == "right" ? "right" : "left");
				std::string consolidation = member_text(series, "consolidation");
				if (consolidation.empty()) {
					ValueType type = static_cast<ValueType>(datapoints_[static_cast<size_t>(dp)].type);
					consolidation = type == ValueType::unsigned_int || type == ValueType::signed_int ? "last" : "mean";
				}
				json.member("consolidation", consolidation.c_str()).end_object();
			}
		}
		json.end_array();
		json.key("references").begin_array();
		for (size_t c = 0; c < charts.size(); ++c) {
			for (const JsonSpan& reference : member_items(charts[c], "references")) {
				json.begin_object().member("element", chart_elements[c]);
				JsonSpan value;
				if (member(reference, "value", value)) {
					json.member("value", number_of(value));
				} else {
					json.member("datapoint", static_cast<uint32_t>(datapoint_of(reference)));
				}
				if (parse_label(reference, label_value)) {
					json.key("label");
					write_label(json, label_value);
				}
				json.member("axis", member_text(reference, "axis") == "right" ? "right" : "left").end_object();
			}
		}
		json.end_array();
		json.end_object();
		if (!failure.empty()) {
			return fail("ui: no datapoint '%s'", failure.c_str());
		}
		if (pass == 0) {
			table(section).bytes.resize(json.length());
		} else {
			if (json.overflowed()) {
				return fail("ui: the text did not fit");
			}
			std::memcpy(table(section).bytes.data(), buffer.data(), json.length());
		}
	}
	return true;
}

// ---- image, settings, datapoints (9.6, 9.7) ------------------------------------------------------

void Compiler::image_and_settings(const JsonSpan& root) {
	JsonSpan image;
	member(root, "image", image);
	Table& t = table(kImage);
	size_t r = t.add();
	put_string(t, r, 0, member_text(image, "projectId"));
	t.u32(r, 4, static_cast<uint32_t>(member_number(image, "revision")));
	put_string(t, r, 8, member_text(image, "schemaVersion"));
	put_string(t, r, 12, member_text(image, "name"));
	put_string(t, r, 16, member_text(image, "generator"));
	put_string(t, r, 20, member_text(image, "generatedAt"));
	JsonSpan history;
	bool has_history = member(root, "history", history);
	t.u32(r, 24, has_history && has_member(history, "budget") ? static_cast<uint32_t>(member_number(history, "budget"))
		: history_ram_);
	t.u32(r, 28, has_history && has_member(history, "archiveBudget")
		? static_cast<uint32_t>(member_number(history, "archiveBudget")) : history_flash_);

	JsonSpan settings;
	if (!member(root, "settings", settings)) {
		return;
	}
	JsonSpan ntp;
	JsonSpan location;
	bool has_ntp = member(settings, "ntp", ntp);
	bool has_location = member(settings, "location", location);
	if (!has_member(settings, "tz") && !has_ntp && !has_location) {
		return; // fixes no setting
	}
	Table& s = table(kSettings);
	size_t k = s.add();
	put_string(s, k, 0, member_text(settings, "tz"));
	put_string(s, k, 4, has_ntp ? member_text(ntp, "server") : std::string());
	if (has_location) {
		uint8_t flags = 0x01;
		if (has_member(location, "elev")) {
			flags |= 0x02;
			s.i32(k, 12, static_cast<int64_t>(member_number(location, "elev")));
		}
		s.u8(k, 8, flags);
		s.f64(k, 16, member_number(location, "lat"));
		s.f64(k, 24, member_number(location, "lon"));
	}
}

void Compiler::datapoint_table() {
	if (datapoints_.empty()) {
		return;
	}
	Table& t = table(kDatapoints);
	for (const Datapoint& dp : datapoints_) {
		size_t r = t.add();
		put_string(t, r, 0, dp.name);
		put_label(t, r, 4, dp.label);
		put_string(t, r, 8, dp.unit);
		t.u8(r, 12, dp.type);
		t.u8(r, 13, dp.flags);
		t.u8(r, 14, dp.owner);
		t.u16(r, 16, dp.owner == kOwnerNone ? kNone : dp.owner_index);
		t.u16(r, 18, dp.owner == kOwnerNone ? 0 : dp.owner_member);
		t.u16(r, 20, dp.history);
	}
}

// ---- The image (9.2, 9.3, 9.5, 9.17, 9.18) -------------------------------------------------------

void put32(std::vector<uint8_t>& out, size_t at, uint32_t v) {
	for (size_t i = 0; i < 4; ++i) {
		out[at + i] = static_cast<uint8_t>(v >> (8 * i));
	}
}

void put16(std::vector<uint8_t>& out, size_t at, uint16_t v) {
	out[at] = static_cast<uint8_t>(v);
	out[at + 1] = static_cast<uint8_t>(v >> 8);
}

bool Compiler::assemble(std::vector<uint8_t>& out, const uint8_t* seed) {
	// The strings: every distinct one once, sorted bytewise, the empty string at 0.
	std::set<std::string> strings;
	strings.insert(std::string());
	for (const StringFix& f : string_fixes_) {
		strings.insert(f.text);
	}
	for (const LabelFix& f : label_fixes_) {
		for (const auto& entry : labels_[static_cast<size_t>(f.label)]) {
			strings.insert(entry.first);
			strings.insert(entry.second);
		}
	}
	std::map<std::string, uint32_t> offsets;
	std::vector<uint8_t>& string_bytes = tables_[kStrings].bytes;
	tables_[kStrings].id = kStrings;
	for (const std::string& s : strings) {
		offsets[s] = static_cast<uint32_t>(string_bytes.size());
		string_bytes.insert(string_bytes.end(), s.begin(), s.end());
		string_bytes.push_back(0);
	}
	for (const StringFix& f : string_fixes_) {
		std::vector<uint8_t>& bytes = tables_[f.section].bytes;
		put32(bytes, f.position, offsets[f.text]);
	}
	// The labels: the entries of every reference anew, in the order the references occur (sections by
	// id, records in order, fields in order).
	std::vector<LabelFix> fixes = label_fixes_;
	std::stable_sort(fixes.begin(), fixes.end(), [](const LabelFix& a, const LabelFix& b) {
		return a.section != b.section ? a.section < b.section : a.position < b.position;
	});
	for (const LabelFix& f : fixes) {
		Table& labels = table(kLabels);
		const Label& label = labels_[static_cast<size_t>(f.label)];
		uint32_t first = static_cast<uint32_t>(labels.count());
		for (const auto& entry : label) {
			size_t e = labels.add();
			labels.u32(e, 0, offsets[entry.first]);
			labels.u32(e, 4, offsets[entry.second]);
		}
		put32(tables_[f.section].bytes, f.position, first | (static_cast<uint32_t>(label.size()) << 24));
	}
	// The sections in ascending order of their ids, each at the next multiple of 8; no empty table.
	std::vector<uint16_t> ids;
	for (auto& entry : tables_) {
		if (!entry.second.bytes.empty() || entry.first == kImage || entry.first == kStrings) {
			ids.push_back(entry.first);
		}
	}
	size_t header = 32;
	size_t position = header + 16 * ids.size();
	std::vector<std::pair<size_t, size_t>> placed;
	for (uint16_t id : ids) {
		position = (position + 7) & ~static_cast<size_t>(7);
		placed.emplace_back(position, tables_[id].bytes.size());
		position += tables_[id].bytes.size();
	}
	size_t signed_end = (position + 7) & ~static_cast<size_t>(7);
	size_t length = signed_end + (seed != nullptr ? 64 : 0);
	out.assign(signed_end, 0);
	const uint8_t magic[4] = {0x48, 0x43, 0x49, 0x4D};
	std::memcpy(out.data(), magic, 4);
	put16(out, 4, 1);
	// Format 1.1 only for an image with its sections (spec/runtime-image.md 8): the anonymous pages.
	put16(out, 6, tables_.count(kUiAnonymous) != 0 && !tables_[kUiAnonymous].bytes.empty() ? 1 : 0);
	put32(out, 8, static_cast<uint32_t>(length));
	put32(out, 16, seed != nullptr ? 1 : 0);
	put16(out, 20, static_cast<uint16_t>(header));
	put16(out, 22, static_cast<uint16_t>(ids.size()));
	for (size_t k = 0; k < ids.size(); ++k) {
		size_t at = header + 16 * k;
		put16(out, at, ids[k]);
		put16(out, at + 2, 0);
		put32(out, at + 4, static_cast<uint32_t>(placed[k].first));
		put32(out, at + 8, static_cast<uint32_t>(placed[k].second));
		put16(out, at + 12, entry_size(ids[k]));
		const std::vector<uint8_t>& bytes = tables_[ids[k]].bytes;
		if (!bytes.empty()) {
			std::memcpy(out.data() + placed[k].first, bytes.data(), bytes.size());
		}
	}
	put32(out, 12, persist::crc32(0, out.data(), out.size()));
	if (seed != nullptr) {
		uint8_t message[52];
		std::memcpy(message, "homecontrol-image-v2", 20);
		Sha256::hash(out.data(), out.size(), message + 20);
		uint8_t signature[kEd25519SignatureSize];
		ed25519_sign(seed, message, sizeof(message), signature);
		out.insert(out.end(), signature, signature + sizeof(signature));
	}
	return true;
}

bool Compiler::compile(const char* json, size_t length, std::vector<uint8_t>& out, const uint8_t* seed) {
	JsonSpan root;
	const char* reason = nullptr;
	size_t offset = 0;
	if (!json_parse(json, length, root, &reason, kJsonMaxDepth, &offset)) {
		return fail("not JSON: %s at byte %u", reason != nullptr ? reason : "invalid", static_cast<unsigned>(offset));
	}
	if (root.kind != Kind::object || !has_member(root, "image")) {
		return fail("not an image source: the member 'image' is missing");
	}
	// The datapoint model (9.7): the declared ones, the time master's, the rules', the schedules',
	// the blocks'.
	if (!declared_datapoints(root)) {
		return false;
	}
	time_master(root);
	rule_datapoints(root);
	schedule_datapoints(root);
	block_datapoints(root);
	for (size_t i = 0; i < datapoints_.size(); ++i) {
		index_.emplace(datapoints_[i].name, static_cast<uint16_t>(i));
	}
	if (!knx_tables()) {
		return false;
	}
	if (has_time_master_) {
		JsonSpan knx, tm;
		member(root, "knx", knx);
		member(knx, "timeMaster", tm);
		Table& t = table(kKnxTimeMaster);
		size_t r = t.add();
		t.u16(r, 0, time_master_dp_[0]);
		t.u16(r, 2, time_master_dp_[1]);
		t.u16(r, 4, time_master_dp_[2]);
		t.u8(r, 6, (member_flag(tm, "atMidnight", true) ? 0x01 : 0) | (member_flag(tm, "onSynchronized", true) ? 0x02 : 0));
		t.u32(r, 8, static_cast<uint32_t>(member_number(tm, "interval", 600)));
	}
	zigbee_table();
	history_tables();
	if (!locations_and_functions(root) || !external(root)) {
		return false;
	}
	mqtt(root);
	if (!schedules(root) || !rules(root) || !blocks(root) || !ui(root)) {
		return false;
	}
	image_and_settings(root);
	datapoint_table();
	return assemble(out, seed);
}

// ---- The corpus cases (13.1) ---------------------------------------------------------------------

std::string raw(const JsonSpan& span) { return std::string(span.data, span.length); }

std::string quoted(const std::string& text) {
	JsonWriter counter = JsonWriter::counter();
	counter.string(text.c_str(), text.size());
	std::vector<char> buffer(counter.length() + 1);
	JsonWriter json(buffer.data(), buffer.size());
	json.string(text.c_str(), text.size());
	return std::string(buffer.data(), json.length());
}

/// The `schedules` of a case or a step as image entries: an id or `{"id", "output"}` is an entry of
/// kind `once` at 1970-01-01 00:00:00, not enabled and not adjustable, labelled with its id; a full
/// entry (with `kind`) stays as it is.
std::string case_schedules(const JsonSpan& list) {
	std::string out = "[";
	bool first = true;
	for (const JsonSpan& item : items(list)) {
		out += first ? "" : ",";
		first = false;
		if (item.kind == Kind::object && has_member(item, "kind")) {
			out += raw(item);
			continue;
		}
		std::string id = item.kind == Kind::string ? text_of(item) : member_text(item, "id");
		out += "{\"id\":" + quoted(id) + ",\"label\":" + quoted(id) +
			",\"kind\":\"once\",\"enabled\":false,\"enabledAdjustable\":false,"
			"\"at\":{\"year\":1970,\"month\":1,\"day\":1,\"hour\":0,\"minute\":0,\"second\":0}";
		JsonSpan output;
		if (item.kind == Kind::object && member(item, "output", output)) {
			out += ",\"output\":" + raw(output);
		}
		out += "}";
	}
	return out + "]";
}

} // namespace

bool compile_image(const char* json, size_t length, std::vector<uint8_t>& out, std::string* error,
	const CompileOptions& options) {
	Compiler compiler(options.registry != nullptr ? *options.registry : blocks::BlockRegistry::device(), false);
	out.clear();
	bool ok = compiler.compile(json, length, out, options.sign_seed);
	if (!ok) {
		out.clear();
	}
	if (error != nullptr) {
		*error = compiler.error;
	}
	return ok;
}

bool compile_case(const char* case_json, size_t length, int step, std::vector<uint8_t>& out, std::string* error,
	const CompileOptions& options) {
	out.clear();
	auto refuse = [&error](const char* reason) {
		if (error != nullptr) {
			*error = reason;
		}
		return false;
	};
	JsonSpan root;
	if (!json_parse(case_json, length, root, nullptr, kJsonMaxDepth) || root.kind != Kind::object) {
		return refuse("the case is not a JSON object");
	}
	// The source member by member (13.1).
	std::string source = "{\"image\":{\"schemaVersion\":\"1.7\",\"projectId\":";
	source += quoted(options.case_name != nullptr ? options.case_name : "case");
	source += ",\"revision\":" + std::to_string(step < 0 ? 0 : step + 1) + "},\"datapoints\":[";
	bool first = true;
	for (const JsonSpan& dp : member_items(root, "datapoints")) {
		bool writable = member_flag(dp, "writable", true);
		source += first ? "" : ",";
		first = false;
		source += "{\"id\":" + quoted(member_text(dp, "name")) + ",\"type\":" + quoted(member_text(dp, "type"));
		source += writable ? ",\"writable\":true,\"transmit\":true" : "";
		source += member_flag(dp, "application", false) ? ",\"virtual\":{}" : "";
		source += member_flag(dp, "persist", false) ? ",\"persist\":true" : "";
		source += "}";
	}
	source += "]";
	JsonSpan rules, blocks, schedules;
	bool has_rules = false, has_blocks = false, has_schedules = false;
	if (step < 0) {
		has_rules = member(root, "rules", rules);
		has_blocks = member(root, "blocks", blocks);
		has_schedules = member(root, "schedules", schedules);
	} else {
		std::vector<JsonSpan> timeline = member_items(root, "timeline");
		JsonSpan image;
		if (static_cast<size_t>(step) >= timeline.size() || !member(timeline[static_cast<size_t>(step)], "image", image)) {
			return refuse("the step is no image step");
		}
		has_rules = member(image, "rules", rules);
		has_blocks = member(image, "blocks", blocks);
		has_schedules = member(image, "schedules", schedules);
		// A step without schedules keeps those of the image before it.
		for (int k = step - 1; k >= 0 && !has_schedules; --k) {
			JsonSpan earlier;
			if (member(timeline[static_cast<size_t>(k)], "image", earlier)) {
				has_schedules = member(earlier, "schedules", schedules);
			}
		}
		if (!has_schedules) {
			has_schedules = member(root, "schedules", schedules);
		}
	}
	if (has_schedules) {
		source += ",\"schedules\":" + case_schedules(schedules);
	}
	if (has_rules) {
		source += ",\"rules\":" + raw(rules);
	}
	if (has_blocks) {
		source += ",\"blocks\":" + raw(blocks);
	}
	source += "}";
	Compiler compiler(options.registry != nullptr ? *options.registry : blocks::BlockRegistry::device(), true);
	bool ok = compiler.compile(source.data(), source.size(), out, options.sign_seed);
	if (!ok) {
		out.clear();
	}
	if (error != nullptr) {
		*error = compiler.error;
	}
	return ok;
}

} // namespace hc::test
