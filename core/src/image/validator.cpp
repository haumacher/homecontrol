/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The device's checks of a binary runtime image (spec/runtime-image.md section 10, issue #87).
#include "hc/image/validator.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "hc/datapoint.h"
#include "hc/knx/address.h"
#include "hc/rules/rule_set.h"
#include "hc/rules/timeline.h"

namespace hc::image {

using namespace bin;
using rules::Node;
using rules::NodeKind;

namespace {

/// A copy of the header: a receiver's buffer of the first bytes need not be aligned.
Header header_of(const uint8_t* bytes) {
	Header h;
	std::memcpy(&h, bytes, sizeof(h));
	return h;
}

size_t signed_end_of(const Header& h) {
	return (h.flags & kFlagSigned) != 0 ? h.length - kSignatureSize : h.length;
}

/// The nouns of a table in the reasons (`datapoint 7 does not exist (5 datapoints)`).
struct Noun {
	uint16_t id;
	const char* one;
	const char* many;
};
constexpr Noun kNouns[] = {
	{kLabels, "label entry", "label entries"},
	{kDatapoints, "datapoint", "datapoints"},
	{kLocations, "location", "locations"},
	{kFunctions, "function", "functions"},
	{kRoles, "role", "roles"},
	{kHistory, "history ring", "history rings"},
	{kArchives, "archive", "archives"},
	{kKnxAddresses, "address", "addresses"},
	{kKnxBindings, "KNX binding", "KNX bindings"},
	{kKnxListen, "listen entry", "listen entries"},
	{kKnxListeners, "listener", "listeners"},
	{kZigbeeBindings, "Zigbee binding", "Zigbee bindings"},
	{kExternalSources, "source", "sources"},
	{kExternalBindings, "binding", "bindings"},
	{kSchedules, "schedule", "schedules"},
	{kScheduleDates, "date", "dates"},
	{kScheduleBounds, "bound", "bounds"},
	{kRules, "rule", "rules"},
	{kRuleParameters, "parameter", "parameters"},
	{kRuleChoices, "choice", "choices"},
	{kRuleTimers, "timer", "timers"},
	{kRuleStates, "state", "states"},
	{kRuleReactions, "reaction", "reactions"},
	{kRuleTriggers, "trigger", "triggers"},
	{kRuleActions, "action", "actions"},
	{kRuleLogParts, "log part", "log parts"},
	{kRuleTimelines, "timeline", "timelines"},
	{kRulePlans, "plan", "plans"},
	{kRulePoints, "point", "points"},
	{kRuleAssignments, "assignment", "assignments"},
	{kNodes, "node", "nodes"},
	{kConstants, "constant", "constants"},
	{kBlocks, "block", "blocks"},
	{kBlockPorts, "port", "ports"},
	{kBlockBound, "bound datapoint", "bound datapoints"},
	{kBlockParameters, "parameter", "parameters"},
};

const Noun& noun_of(uint16_t id) {
	for (const Noun& n : kNouns) {
		if (n.id == id) {
			return n;
		}
	}
	static const Noun kRecord{0, "record", "records"};
	return kRecord;
}

constexpr const char* kOwnerNames[] = {"none", "KNX", "the scheduler", "the time master", "Zigbee", "the rule engine",
	"the block runtime", "external values"};

/// The rule-local context of an expression or a record.
struct RuleContext {
	size_t rule = 0;
	size_t parameters = 0;
	size_t timers = 0;
	size_t states = 0;
};

/// The arguments of the functions (spec 9.14): min and max.
struct Arity {
	uint8_t min;
	uint8_t max;
};
constexpr Arity kArity[] = {
	{3, 3}, {1, 1}, {2, 2}, {3, 3}, {4, 4},                                 // if known coalesce between hysteresis
	{1, 1}, {2, 8}, {2, 8}, {3, 3}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {2, 2}, {1, 1}, // abs .. toInt
	{0, 0}, {0, 0}, {0, 0}, {0, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {3, 3}, // now .. inWindow
	{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},                         // sunrise .. sunElevation
	{1, 1}, {1, 1}, {2, 2}, {1, 1}, {1, 1},                                 // sinceUpdate .. quality
	{1, 1}, {1, 1}, {0, 1},                                                 // running remaining state
	{1, 1}, {2, 2}, {2, 2}, {2, 2}, {1, 1},                                 // length .. toString
	{3, 3}, {4, 4}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {2, 2}, {1, 1}, {1, 1}, // rgb .. controlValue
};
static_assert(sizeof(kArity) / sizeof(kArity[0]) == static_cast<size_t>(rules::Fn::count), "arity per function");

bool utf8_valid(const uint8_t* p, size_t length, bool nul_allowed, size_t& bad) {
	size_t i = 0;
	while (i < length) {
		uint8_t c = p[i];
		if (c < 0x80) {
			if (c == 0 && !nul_allowed) {
				bad = i;
				return false;
			}
			++i;
			continue;
		}
		size_t n = 0;
		uint32_t cp = 0;
		uint32_t min = 0;
		if ((c & 0xE0) == 0xC0) {
			n = 1, cp = c & 0x1F, min = 0x80;
		} else if ((c & 0xF0) == 0xE0) {
			n = 2, cp = c & 0x0F, min = 0x800;
		} else if ((c & 0xF8) == 0xF0) {
			n = 3, cp = c & 0x07, min = 0x10000;
		} else {
			bad = i;
			return false;
		}
		if (i + n >= length + 0 && i + n > length - 1 + 1) {
			bad = i;
			return false;
		}
		for (size_t k = 1; k <= n; ++k) {
			if (i + k >= length || (p[i + k] & 0xC0) != 0x80) {
				bad = i;
				return false;
			}
			cp = (cp << 6) | (p[i + k] & 0x3F);
		}
		if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
			bad = i;
			return false;
		}
		i += n + 1;
	}
	return true;
}

class Checker {
public:
	Checker(const uint8_t* bytes, Report& report) : bytes_(bytes), report_(report), h_(header_of(bytes)) {
		hint_ = newer_hint(bytes);
	}

	bool run();

private:
	// ---- reporting ----
	void refuse(const char* path, const char* format, ...) HC_PRINTF_FORMAT(3, 4);
	bool full() const { return report_.full(); }

	// ---- the record being checked ----
	template <class T>
	const T& rec(uint16_t id, size_t i) const {
		return view_.at<T>(id, i);
	}
	size_t count(uint16_t id) const { return view_.count(id); }
	/// Sets the path prefix `<section>[i]` for the field checks.
	void at(uint16_t id, size_t i) {
		const SectionInfo* info = section_info(id);
		std::snprintf(prefix_, sizeof(prefix_), "%s[%u]", info != nullptr ? info->name : "?", static_cast<unsigned>(i));
	}
	const char* path(const char* field) {
		std::snprintf(path_, sizeof(path_), "%s.%s", prefix_, field);
		return path_;
	}

	// ---- field checks of 10.4 ----
	bool string(const char* field, uint32_t offset);
	bool id_string(const char* field, uint32_t offset);
	bool label(const char* field, uint32_t ref);
	bool index(const char* field, uint16_t table, uint32_t value, bool none_allowed);
	bool local(const char* field, const char* what, uint32_t value, size_t count, bool none_allowed);
	bool run(const char* field, uint16_t table, uint32_t first, uint32_t n);
	bool code(const char* field, uint32_t value, uint32_t max);
	bool flags(const char* field, uint32_t value, uint32_t allowed);
	bool boolean(const char* field, uint8_t value);
	bool range(const char* field, int64_t value, int64_t min, int64_t max);
	bool finite(const char* field, double value);
	bool real_range(const char* field, double value, double min, double max);
	bool limit(const char* field, size_t value, size_t max, const char* what);

	// ---- the passes ----
	bool sections();
	bool strings();
	void records();
	void image_record();
	void settings_record();
	void datapoints();
	void locations();
	void functions();
	void history();
	void knx();
	void zigbee();
	void external();
	void mqtt();
	void schedules();
	void rules();
	void constants();
	void blocks();
	void expressions();
	bool expression(uint16_t root, const RuleContext& rule, const char* where);
	void owners();
	void limits();
	bool constant_valid(const rules::Val& v, uint8_t type, char* reason, size_t size) const;

	const uint8_t* bytes_;
	Report& report_;
	const Header h_;
	const char* hint_ = "";
	View view_;
	size_t string_bytes_ = 0;
	char prefix_[48] = {};
	char path_[Report::kMaxPath + 1] = {};
};

void Checker::refuse(const char* path, const char* format, ...) {
	char reason[Report::kMaxReason + 1];
	va_list args;
	va_start(args, format);
	std::vsnprintf(reason, sizeof(reason), format, args);
	va_end(args);
	report_.add(Report::Kind::invalid, path, "%s", reason);
}

bool Checker::string(const char* field, uint32_t offset) {
	const char* strings = view_.str(0);
	if (offset >= string_bytes_ || (offset != 0 && strings[offset - 1] != '\0')) {
		refuse(path(field), "malformed: %u is no string (not the start of one of the %u bytes of strings)",
			static_cast<unsigned>(offset), static_cast<unsigned>(string_bytes_));
		return false;
	}
	return true;
}

bool Checker::id_string(const char* field, uint32_t offset) {
	if (!string(field, offset)) {
		return false;
	}
	if (view_.str(offset)[0] == '\0') {
		refuse(path(field), "malformed: empty, it is required");
		return false;
	}
	return true;
}

bool Checker::label(const char* field, uint32_t ref) {
	if (ref == 0) {
		return true;
	}
	uint32_t first = label_first(ref);
	uint32_t n = label_count(ref);
	size_t entries = count(kLabels);
	if (n == 0) {
		refuse(path(field), "malformed: label reference 0x%08x names no entry", static_cast<unsigned>(ref));
		return false;
	}
	if (n > 16) {
		refuse(path(field), "limitExceeded: a label of %u languages, at most 16", static_cast<unsigned>(n));
		return false;
	}
	if (static_cast<size_t>(first) + n > entries) {
		refuse(path(field), "malformed: label entries %u..%u do not exist (%u entries)", static_cast<unsigned>(first),
			static_cast<unsigned>(first + n - 1), static_cast<unsigned>(entries));
		return false;
	}
	const char* strings = view_.str(0);
	bool empty_tag = false;
	for (size_t i = 0; i < n; ++i) {
		const LabelEntry& e = rec<LabelEntry>(kLabels, first + i);
		for (uint32_t offset : {e.tag, e.text}) {
			if (offset >= string_bytes_ || (offset != 0 && strings[offset - 1] != '\0')) {
				refuse(path(field), "malformed: label entry %u: %u is no string (not the start of one of the %u bytes of strings)",
					static_cast<unsigned>(first + i), static_cast<unsigned>(offset), static_cast<unsigned>(string_bytes_));
				return false;
			}
		}
		empty_tag = empty_tag || strings[e.tag] == '\0';
	}
	if (empty_tag && n > 1) {
		refuse(path(field), "malformed: a language map with an empty tag");
		return false;
	}
	return true;
}

bool Checker::index(const char* field, uint16_t table, uint32_t value, bool none_allowed) {
	if (none_allowed && value == kNone) {
		return true;
	}
	size_t n = count(table);
	if (value >= n) {
		const Noun& noun = noun_of(table);
		refuse(path(field), "malformed: %s %u does not exist (%u %s)", noun.one, static_cast<unsigned>(value),
			static_cast<unsigned>(n), noun.many);
		return false;
	}
	return true;
}

bool Checker::local(const char* field, const char* what, uint32_t value, size_t n, bool none_allowed) {
	if (none_allowed && (value == kNone || value == kNone8)) {
		return true;
	}
	if (value >= n) {
		refuse(path(field), "malformed: %s %u does not exist (the rule has %u)", what, static_cast<unsigned>(value),
			static_cast<unsigned>(n));
		return false;
	}
	return true;
}

bool Checker::run(const char* field, uint16_t table, uint32_t first, uint32_t n) {
	if (n == 0) {
		return true;
	}
	size_t total = count(table);
	if (static_cast<size_t>(first) + n > total) {
		const SectionInfo* info = section_info(table);
		refuse(path(field), "malformed: %s %u..%u do not exist (%u %s)", info != nullptr ? info->name : "?",
			static_cast<unsigned>(first), static_cast<unsigned>(first + n - 1), static_cast<unsigned>(total),
			noun_of(table).many);
		return false;
	}
	return true;
}

bool Checker::code(const char* field, uint32_t value, uint32_t max) {
	if (value > max) {
		refuse(path(field), "malformed: %s %u is unknown (0..%u)%s", field, static_cast<unsigned>(value),
			static_cast<unsigned>(max), hint_);
		return false;
	}
	return true;
}

bool Checker::flags(const char* field, uint32_t value, uint32_t allowed) {
	if ((value & ~allowed) != 0) {
		refuse(path(field), "malformed: unknown bits 0x%x in %s%s", static_cast<unsigned>(value & ~allowed), field, hint_);
		return false;
	}
	return true;
}

bool Checker::boolean(const char* field, uint8_t value) {
	if (value > 1) {
		refuse(path(field), "malformed: %u is not a boolean (0 or 1)", static_cast<unsigned>(value));
		return false;
	}
	return true;
}

bool Checker::range(const char* field, int64_t value, int64_t min, int64_t max) {
	if (value < min || value > max) {
		refuse(path(field), "malformed: %s %lld is outside %lld..%lld", field, static_cast<long long>(value),
			static_cast<long long>(min), static_cast<long long>(max));
		return false;
	}
	return true;
}

bool Checker::finite(const char* field, double value) {
	if (!std::isfinite(value)) {
		refuse(path(field), "malformed: %s is not a finite number", field);
		return false;
	}
	return true;
}

bool Checker::real_range(const char* field, double value, double min, double max) {
	if (!std::isfinite(value) || value < min || value > max) {
		refuse(path(field), "malformed: %s %g is outside %g..%g", field, value, min, max);
		return false;
	}
	return true;
}

bool Checker::limit(const char* field, size_t value, size_t max, const char* what) {
	if (value > max) {
		refuse(path(field), "limitExceeded: %u %s, at most %u", static_cast<unsigned>(value), what,
			static_cast<unsigned>(max));
		return false;
	}
	return true;
}

// ---- 10.3 ----------------------------------------------------------------------------------------

bool Checker::sections() {
	size_t end = signed_end_of(h_);
	const SectionEntry* entries = reinterpret_cast<const SectionEntry*>(bytes_ + h_.header_size);
	size_t table_end = h_.header_size + static_cast<size_t>(h_.section_count) * sizeof(SectionEntry);
	bool has_image = false, has_strings = false;
	size_t previous_end = table_end;
	for (size_t k = 0; k < h_.section_count; ++k) {
		const SectionEntry& e = entries[k];
		char p[40];
		auto field = [&](const char* name) {
			std::snprintf(p, sizeof(p), "sections[%u].%s", static_cast<unsigned>(k), name);
			return p;
		};
		if (k > 0 && e.id <= entries[k - 1].id) {
			if (e.id == entries[k - 1].id) {
				refuse(field("id"), "malformed: section 0x%02x twice", static_cast<unsigned>(e.id));
			} else {
				refuse(field("id"), "malformed: section 0x%02x after 0x%02x: the table is not in ascending order",
					static_cast<unsigned>(e.id), static_cast<unsigned>(entries[k - 1].id));
			}
			return false;
		}
		if ((e.flags & ~kSectionRequired) != 0) {
			refuse(field("flags"), "malformed: unknown flags 0x%x%s", static_cast<unsigned>(e.flags & ~kSectionRequired),
				hint_);
			return false;
		}
		const SectionInfo* info = section_info(e.id);
		if (info == nullptr && (e.flags & kSectionRequired) != 0) {
			refuse(field("id"), "malformed: section 0x%02x is required, but this firmware does not know it%s",
				static_cast<unsigned>(e.id), hint_);
			return false;
		}
		if (e.offset % 8 != 0) {
			refuse(field("offset"), "malformed: offset 0x%x is not a multiple of 8", static_cast<unsigned>(e.offset));
			return false;
		}
		if (e.offset < table_end) {
			refuse(field("offset"), "malformed: offset 0x%x lies inside the section table (it ends at 0x%x)",
				static_cast<unsigned>(e.offset), static_cast<unsigned>(table_end));
			return false;
		}
		if (e.offset < previous_end) {
			const SectionEntry& before = entries[k - 1];
			refuse(field("offset"), "malformed: offset 0x%x lies inside section 0x%02x (0x%x..0x%x)",
				static_cast<unsigned>(e.offset), static_cast<unsigned>(before.id), static_cast<unsigned>(before.offset),
				static_cast<unsigned>(before.offset + before.length - 1));
			return false;
		}
		if (static_cast<size_t>(e.offset) + e.length > end) {
			refuse(field("length"), "malformed: the section ends at 0x%x, beyond the signed bytes (0x%x)",
				static_cast<unsigned>(e.offset + e.length), static_cast<unsigned>(end));
			return false;
		}
		if (k + 1 < h_.section_count && e.offset + e.length > entries[k + 1].offset &&
			entries[k + 1].offset >= e.offset && entries[k + 1].offset % 8 == 0) {
			// 9.3: not beyond the next section's offset.
			refuse(field("length"), "malformed: the section ends at 0x%x, beyond the next section's offset (0x%x)",
				static_cast<unsigned>(e.offset + e.length), static_cast<unsigned>(entries[k + 1].offset));
			return false;
		}
		previous_end = static_cast<size_t>(e.offset) + e.length;
		if (info == nullptr) {
			continue; // unknown and optional: skipped (section 11)
		}
		if (info->size == 0) {
			if (e.entry_size != 0) {
				refuse(field("entrySize"), "malformed: a byte section has entry size 0");
				return false;
			}
		} else {
			if (e.entry_size < info->size) {
				refuse(field("entrySize"), "malformed: entry size %u for %s, which %s at least %u",
					static_cast<unsigned>(e.entry_size), info->name, info->single ? "needs" : "need",
					static_cast<unsigned>(info->size));
				return false;
			}
			if (e.entry_size % info->align != 0) {
				refuse(field("entrySize"), "malformed: entry size %u is not a multiple of %u",
					static_cast<unsigned>(e.entry_size), static_cast<unsigned>(info->align));
				return false;
			}
			if (e.length % e.entry_size != 0) {
				refuse(field("entrySize"), "malformed: length %u is not a multiple of the entry size %u",
					static_cast<unsigned>(e.length), static_cast<unsigned>(e.entry_size));
				return false;
			}
			size_t records = e.length / e.entry_size;
			if (info->single && records != 1) {
				refuse(field("length"), "malformed: the section %s holds %u records, it has one", info->name,
					static_cast<unsigned>(records));
				return false;
			}
			size_t max = e.id == kLabels ? 16777215u : 65535u;
			if (records > max) {
				refuse(field("length"), "limitExceeded: %u records, at most %u", static_cast<unsigned>(records),
					static_cast<unsigned>(max));
				return false;
			}
		}
		has_image = has_image || e.id == kImage;
		has_strings = has_strings || e.id == kStrings;
	}
	if (!has_image) {
		refuse("sections", "malformed: the section image (0x01) is missing");
		return false;
	}
	if (!has_strings) {
		refuse("sections", "malformed: the section strings (0x02) is missing");
		return false;
	}
	return true;
}

// ---- 10.4 ----------------------------------------------------------------------------------------

bool Checker::strings() {
	const Table& t = view_.table(kStrings);
	const uint8_t* p = t.base;
	size_t n = t.count;
	string_bytes_ = n;
	if (n == 0 || p[0] != 0) {
		refuse("strings", "malformed: offset 0 is not the empty string");
		return false;
	}
	if (p[n - 1] != 0) {
		refuse("strings", "malformed: the last byte is not a NUL");
		return false;
	}
	size_t bad = 0;
	if (!utf8_valid(p, n, true, bad)) {
		refuse("strings", "malformed: invalid UTF-8 at offset 0x%x", static_cast<unsigned>(bad));
		return false;
	}
	size_t start = 0;
	for (size_t i = 0; i < n; ++i) {
		if (p[i] == 0) {
			if (i - start > kMaxString) {
				refuse("strings", "limitExceeded: the string at offset 0x%x has %u bytes, at most %u",
					static_cast<unsigned>(start), static_cast<unsigned>(i - start), static_cast<unsigned>(kMaxString));
				return false;
			}
			start = i + 1;
		}
	}
	size_t length = 0;
	const char* ui = view_.ui(length);
	if (ui != nullptr && !utf8_valid(reinterpret_cast<const uint8_t*>(ui), length, false, bad)) {
		if (ui[bad] == '\0') {
			refuse("ui", "malformed: a NUL at offset 0x%x", static_cast<unsigned>(bad));
		} else {
			refuse("ui", "malformed: invalid UTF-8 at offset 0x%x", static_cast<unsigned>(bad));
		}
		return false;
	}
	return true;
}

// ---- 10.5 ----------------------------------------------------------------------------------------

void Checker::image_record() {
	const ImageRec& r = rec<ImageRec>(kImage, 0);
	at(kImage, 0);
	id_string("projectId", r.project_id);
	id_string("schemaVersion", r.schema_version);
	string("name", r.name);
	string("generator", r.generator);
	string("generatedAt", r.generated_at);
}

void Checker::settings_record() {
	if (count(kSettings) == 0) {
		return;
	}
	const SettingsRec& r = rec<SettingsRec>(kSettings, 0);
	at(kSettings, 0);
	string("tz", r.tz);
	string("ntp", r.ntp);
	if (flags("flags", r.flags, 0x03) && (r.flags & 0x01) != 0) {
		real_range("lat", r.lat, -90, 90);
		real_range("lon", r.lon, -180, 180);
		if ((r.flags & 0x02) != 0) {
			range("elev", r.elev, -500, 9000);
		}
	}
}

void Checker::datapoints() {
	for (size_t i = 0; i < count(kDatapoints) && !full(); ++i) {
		const DatapointRec& r = rec<DatapointRec>(kDatapoints, i);
		at(kDatapoints, i);
		id_string("name", r.name);
		label("label", r.label);
		string("unit", r.unit);
		range("type", r.type, 1, 12);
		code("owner", r.owner, 7);
		if (index("history", kHistory, r.history, true)) {
			bool flagged = (r.flags & DatapointFlags::kHistory) != 0;
			if (flagged != (r.history != kNone)) {
				refuse(path("history"), "malformed: the history flag and the history index disagree");
			}
		}
	}
}

void Checker::locations() {
	size_t n = count(kLocations);
	for (size_t i = 0; i < n && !full(); ++i) {
		const LocationRec& r = rec<LocationRec>(kLocations, i);
		at(kLocations, i);
		id_string("id", r.id);
		label("label", r.label);
		code("kind", r.kind, 5);
		if (index("parent", kLocations, r.parent, true)) {
			// The parents form no cycle: a chain longer than the table is one.
			uint16_t p = r.parent;
			size_t steps = 0;
			while (p != kNone && p < n && steps <= n) {
				p = rec<LocationRec>(kLocations, p).parent;
				++steps;
			}
			if (steps > n) {
				refuse(path("parent"), "malformed: the parents of location %u form a cycle", static_cast<unsigned>(i));
			}
		}
	}
}

void Checker::functions() {
	for (size_t i = 0; i < count(kFunctions) && !full(); ++i) {
		const FunctionRec& r = rec<FunctionRec>(kFunctions, i);
		at(kFunctions, i);
		id_string("id", r.id);
		label("label", r.label);
		string("unit", r.unit);
		index("room", kLocations, r.room, true);
		run("firstRole", kRoles, r.first_role, r.role_count);
		limit("roleCount", r.role_count, 16, "roles of a function");
		code("kind", r.kind, 7);
		range("scene", r.scene, 0, 64);
		for (size_t k = 0; k < 6; ++k) {
			flags("presses", r.presses[k], 0x3F);
		}
	}
	for (size_t i = 0; i < count(kRoles) && !full(); ++i) {
		const RoleRec& r = rec<RoleRec>(kRoles, i);
		at(kRoles, i);
		id_string("name", r.name);
		index("datapoint", kDatapoints, r.datapoint, false);
	}
}

void Checker::history() {
	for (size_t i = 0; i < count(kHistory) && !full(); ++i) {
		const HistoryRec& r = rec<HistoryRec>(kHistory, i);
		at(kHistory, i);
		if (index("datapoint", kDatapoints, r.datapoint, false)) {
			uint8_t type = rec<DatapointRec>(kDatapoints, r.datapoint).type;
			if (type < 1 || type > 4) {
				refuse(path("datapoint"), "malformed: a history of a %s datapoint: only boolean, unsigned, signed and real",
					value_type_name(static_cast<ValueType>(type <= 12 ? type : 0)));
			}
		}
		if (range("spacing", r.spacing, 10, 86400) && range("retention", r.retention, r.spacing, 2678400)) {
			uint64_t samples = (static_cast<uint64_t>(r.retention) + r.spacing - 1) / r.spacing + 1;
			if (samples > 2048) {
				refuse(path("retention"), "limitExceeded: %u samples, at most 2048", static_cast<unsigned>(samples));
			}
		}
		if (run("firstArchive", kArchives, r.first_archive, r.archive_count)) {
			if (limit("archiveCount", r.archive_count, 4, "archives")) {
				for (size_t k = 0; k < r.archive_count; ++k) {
					const ArchiveRec& a = rec<ArchiveRec>(kArchives, r.first_archive + k);
					at(kArchives, r.first_archive + k);
					if (!range("resolution", a.resolution, 60, 2678400)) {
						continue;
					}
					if (a.resolution <= r.spacing) {
						refuse(path("resolution"), "malformed: resolution %u is not above the ring's spacing %u",
							static_cast<unsigned>(a.resolution), static_cast<unsigned>(r.spacing));
						continue;
					}
					if (range("retention", a.retention, a.resolution, 315360000)) {
						uint64_t slots = (static_cast<uint64_t>(a.retention) + a.resolution - 1) / a.resolution + 1;
						if (slots > 2048) {
							refuse(path("retention"), "limitExceeded: %u slots, at most 2048", static_cast<unsigned>(slots));
						}
					}
				}
			}
		}
	}
}

void Checker::knx() {
	char a_text[knx::GroupAddress::kMaxText];
	char b_text[knx::GroupAddress::kMaxText];
	size_t addresses = count(kKnxAddresses);
	for (size_t i = 0; i < addresses && !full(); ++i) {
		const KnxAddressRec& r = rec<KnxAddressRec>(kKnxAddresses, i);
		at(kKnxAddresses, i);
		if (i > 0) {
			uint16_t before = rec<KnxAddressRec>(kKnxAddresses, i - 1).address;
			if (r.address <= before) {
				knx::GroupAddress{r.address}.format(a_text, sizeof(a_text));
				knx::GroupAddress{before}.format(b_text, sizeof(b_text));
				if (r.address == before) {
					refuse(path("address"), "malformed: address %s twice", a_text);
				} else {
					refuse(path("address"), "malformed: address %s follows %s", a_text, b_text);
				}
			}
		}
		if (run("firstListener", kKnxListeners, r.first_listener, r.listener_count)) {
			for (size_t k = 0; k < r.listener_count; ++k) {
				uint16_t b = rec<uint16_t>(kKnxListeners, r.first_listener + k);
				if (k > 0 && b <= rec<uint16_t>(kKnxListeners, r.first_listener + k - 1)) {
					refuse(path("firstListener"), "malformed: the listeners of an address do not ascend strictly");
					break;
				}
			}
		}
	}
	size_t bindings = count(kKnxBindings);
	for (size_t i = 0; i < bindings && !full(); ++i) {
		const KnxBindingRec& r = rec<KnxBindingRec>(kKnxBindings, i);
		at(kKnxBindings, i);
		if (index("datapoint", kDatapoints, r.datapoint, false) && i > 0 &&
			r.datapoint <= rec<KnxBindingRec>(kKnxBindings, i - 1).datapoint) {
			refuse(path("datapoint"), "malformed: the KNX bindings are not in ascending order of their datapoints");
		}
		index("send", kKnxAddresses, r.send, true);
		if (run("firstListen", kKnxListen, r.first_listen, r.listen_count) &&
			limit("listenCount", r.listen_count, 8, "listening addresses")) {
			for (size_t k = 0; k < r.listen_count; ++k) {
				uint16_t a = rec<uint16_t>(kKnxListen, r.first_listen + k);
				if (a >= addresses) {
					continue; // refused with knx.listen
				}
				bool twice = a == r.send;
				for (size_t q = 0; q < k && !twice; ++q) {
					twice = rec<uint16_t>(kKnxListen, r.first_listen + q) == a;
				}
				if (twice) {
					knx::GroupAddress{rec<KnxAddressRec>(kKnxAddresses, a).address}.format(a_text, sizeof(a_text));
					refuse(path("firstListen"), "malformed: address %s is listed twice", a_text);
				}
			}
		}
	}
	for (size_t i = 0; i < count(kKnxListen) && !full(); ++i) {
		at(kKnxListen, i);
		index("address", kKnxAddresses, rec<uint16_t>(kKnxListen, i), false);
	}
	for (size_t i = 0; i < count(kKnxListeners) && !full(); ++i) {
		at(kKnxListeners, i);
		index("binding", kKnxBindings, rec<uint16_t>(kKnxListeners, i), false);
	}
	if (count(kKnxTimeMaster) > 0) {
		const TimeMasterRec& r = rec<TimeMasterRec>(kKnxTimeMaster, 0);
		at(kKnxTimeMaster, 0);
		index("time", kDatapoints, r.time, true);
		index("date", kDatapoints, r.date, true);
		index("dateTime", kDatapoints, r.date_time, true);
		if (r.time == kNone && r.date == kNone && r.date_time == kNone) {
			refuse(path("time"), "malformed: a time master without an address");
		}
		flags("flags", r.flags, 0x03);
		range("interval", r.interval, 60, 86400);
	}
}

void Checker::zigbee() {
	for (size_t i = 0; i < count(kZigbeeBindings) && !full(); ++i) {
		const ZigbeeBindingRec& r = rec<ZigbeeBindingRec>(kZigbeeBindings, i);
		at(kZigbeeBindings, i);
		index("datapoint", kDatapoints, r.datapoint, false);
		range("endpoint", r.endpoint, 1, 240);
		if (flags("flags", r.flags, 0x0F) && (r.flags & 0x08) != 0) {
			finite("reportChange", r.report_change);
		}
	}
}

void Checker::external() {
	for (size_t i = 0; i < count(kExternalSources) && !full(); ++i) {
		const ExternalSourceRec& r = rec<ExternalSourceRec>(kExternalSources, i);
		at(kExternalSources, i);
		id_string("id", r.id);
		if (string("url", r.url) && string("preset", r.preset) && (r.url == 0) == (r.preset == 0)) {
			refuse(path("url"), "malformed: a source has a url or a preset, not both");
		}
		range("interval", r.interval, 300, 86400);
		if (run("firstBinding", kExternalBindings, r.first_binding, r.binding_count) && r.binding_count == 0) {
			refuse(path("firstBinding"), "malformed: a source without bindings");
		}
	}
	for (size_t i = 0; i < count(kExternalBindings) && !full(); ++i) {
		const ExternalBindingRec& r = rec<ExternalBindingRec>(kExternalBindings, i);
		at(kExternalBindings, i);
		index("datapoint", kDatapoints, r.datapoint, false);
		string("path", r.path);
		string("variable", r.variable);
		string("unit", r.unit);
		if (finite("scale", r.scale) && r.scale == 0) {
			refuse(path("scale"), "malformed: scale 0");
		}
		finite("offset", r.offset);
	}
}

void Checker::mqtt() {
	if (count(kMqtt) == 0) {
		return;
	}
	const MqttRec& r = rec<MqttRec>(kMqtt, 0);
	at(kMqtt, 0);
	id_string("broker", r.broker);
	string("username", r.username);
	string("baseTopic", r.base_topic);
	string("discoveryPrefix", r.discovery_prefix);
	range("historyInterval", r.history_interval, 1, 1440);
	flags("flags", r.flags, 0x0F);
}

bool valid_date(int64_t year, int64_t month, int64_t day, int64_t min_year, int64_t max_year) {
	return year >= min_year && year <= max_year && month >= 1 && month <= 12 && day >= 1 &&
		rules::valid_date(static_cast<int32_t>(year), static_cast<unsigned>(month), static_cast<unsigned>(day));
}

void Checker::schedules() {
	for (size_t i = 0; i < count(kSchedules) && !full(); ++i) {
		const ScheduleRec& r = rec<ScheduleRec>(kSchedules, i);
		at(kSchedules, i);
		id_string("id", r.id);
		string("label", r.label);
		bool kind_ok = code("kind", r.kind, 5);
		flags("flags", r.flags, 0x1F);
		flags("days", r.days, 0x7F);
		if (kind_ok && (r.kind == 0 || r.kind == 3 || r.kind == 4) && r.days == 0) {
			refuse(path("days"), "malformed: no day");
		}
		code("sun", r.sun, 3);
		index("output", kDatapoints, r.output, true);
		index("timeDatapoint", kDatapoints, r.time_datapoint, true);
		flags("adjustable", r.adjustable, 0x1FF);
		if (r.kind == 4) {
			range("every", r.every, 1, 720);
		}
		run("firstDate", kScheduleDates, r.first_date, r.date_count);
		if (r.kind == 2) {
			range("dateCount", r.date_count, 1, 128);
		}
		if (run("firstBound", kScheduleBounds, r.first_bound, r.bound_count)) {
			limit("boundCount", r.bound_count, 9, "bounds");
			for (size_t k = 0; k < r.bound_count; ++k) {
				const ScheduleBoundRec& b = rec<ScheduleBoundRec>(kScheduleBounds, r.first_bound + k);
				at(kScheduleBounds, r.first_bound + k);
				if (b.member == 0 || (b.member & (b.member - 1)) != 0 || (b.member & r.adjustable) == 0) {
					refuse(path("member"), "malformed: member 0x%x is not one bit of the entry's adjustable 0x%x",
						static_cast<unsigned>(b.member), static_cast<unsigned>(r.adjustable));
				}
				if (b.min > b.max) {
					refuse(path("min"), "malformed: min %lld above max %lld", static_cast<long long>(b.min),
						static_cast<long long>(b.max));
				}
				if (b.step < 0) {
					refuse(path("step"), "malformed: step %lld is negative", static_cast<long long>(b.step));
				}
			}
			at(kSchedules, i);
		}
		if (r.kind == 1) {
			range("month", r.month, 1, 12);
			range("day", r.day, 1, 31);
		}
		range("offset", r.offset, -43200, 43200);
		range("time", r.time, 0, 86399);
		range("notBefore", r.not_before, 0, 86399);
		range("notAfter", r.not_after, 0, 86399);
		range("from", r.from, 0, 86399);
		range("until", r.until, 0, 86399);
		range("atTime", r.at_time, 0, 86399);
		if (r.kind == 5 && !valid_date(r.at_year, r.at_month, r.at_day, 1970, 2155)) {
			refuse(path("atYear"), "malformed: %u-%u-%u is not a valid date", static_cast<unsigned>(r.at_year),
				static_cast<unsigned>(r.at_month), static_cast<unsigned>(r.at_day));
		}
	}
	for (size_t i = 0; i < count(kScheduleDates) && !full(); ++i) {
		const ScheduleDateRec& d = rec<ScheduleDateRec>(kScheduleDates, i);
		at(kScheduleDates, i);
		if (!valid_date(d.year, d.month, d.day, 1970, 2155)) {
			refuse(path("year"), "malformed: %u-%u-%u is not a valid date", static_cast<unsigned>(d.year),
				static_cast<unsigned>(d.month), static_cast<unsigned>(d.day));
		}
	}
}

bool Checker::constant_valid(const rules::Val& v, uint8_t type, char* reason, size_t size) const {
	using rules::Type;
	(void)type;
	auto fail = [&](const char* format, auto... args) {
		if constexpr (sizeof...(args) == 0) {
			std::snprintf(reason, size, "%s", format);
		} else {
			std::snprintf(reason, size, format, args...);
		}
		return false;
	};
	if (static_cast<uint8_t>(v.type) > 11) {
		return fail("malformed: type %u is unknown (0..11)%s", static_cast<unsigned>(v.type), hint_);
	}
	// Raw bytes: a bool of the record that is neither 0 nor 1 must not be loaded as a bool.
	const uint8_t present = reinterpret_cast<const uint8_t*>(&v)[1];
	if (present != 1) {
		return fail("malformed: present %u, a constant is present (1)", static_cast<unsigned>(present));
	}
	switch (v.type) {
		case Type::string:
			if (v.length > 28) {
				return fail("malformed: a string constant of %u bytes, at most 28", static_cast<unsigned>(v.length));
			}
			if (std::memchr(v.u.bytes, 0, v.length) != nullptr || v.u.bytes[v.length] != '\0') {
				return fail("malformed: a string constant with a NUL in its bytes or without one after them");
			}
			break;
		case Type::raw:
			if (v.length > 14) {
				return fail("malformed: a raw constant of %u bytes, at most 14", static_cast<unsigned>(v.length));
			}
			break;
		default:
			if (v.length != 0) {
				return fail("malformed: length %u of a constant that is no string", static_cast<unsigned>(v.length));
			}
			break;
	}
	const uint8_t* raw = reinterpret_cast<const uint8_t*>(&v.u);
	switch (v.type) {
		case Type::boolean:
			if (raw[0] > 1) {
				return fail("malformed: %u is not a boolean (0 or 1)", static_cast<unsigned>(raw[0]));
			}
			break;
		case Type::real:
			if (!std::isfinite(v.u.r)) {
				return fail("malformed: a real constant that is not finite");
			}
			break;
		case Type::time_of_day:
			if (v.u.tod.day_of_week > 7 || v.u.tod.hour > 23 || v.u.tod.minute > 59 || v.u.tod.second > 59) {
				return fail("malformed: a time of day out of its range");
			}
			break;
		case Type::date:
			if (v.u.date.year < 1900 || v.u.date.year > 2155 || v.u.date.month < 1 || v.u.date.month > 12 ||
				v.u.date.day < 1 || v.u.date.day > 31) {
				return fail("malformed: a date out of its range");
			}
			break;
		case Type::date_time:
			if (v.u.dt.year < 1900 || v.u.dt.year > 2155 || v.u.dt.month < 1 || v.u.dt.month > 12 || v.u.dt.day < 1 ||
				v.u.dt.day > 31 || v.u.dt.hour > 24 || v.u.dt.minute > 59 || v.u.dt.second > 59 ||
				v.u.dt.day_of_week > 7) {
				return fail("malformed: a date and time out of its range");
			}
			break;
		case Type::controlled:
			if (raw[0] > 1) {
				return fail("malformed: %u is not a boolean (0 or 1)", static_cast<unsigned>(raw[0]));
			}
			break;
		case Type::color:
			if (v.u.color.valid > 0x0F) {
				return fail("malformed: unknown bits 0x%x in valid%s", static_cast<unsigned>(v.u.color.valid & ~0x0F),
					hint_);
			}
			break;
		default: break;
	}
	return true;
}

void Checker::constants() {
	char reason[Report::kMaxReason + 1];
	for (size_t i = 0; i < count(kConstants) && !full(); ++i) {
		const rules::Val& v = rec<rules::Val>(kConstants, i);
		at(kConstants, i);
		if (!constant_valid(v, static_cast<uint8_t>(v.type), reason, sizeof(reason))) {
			refuse(path("type"), "%s", reason);
		}
	}
}

void Checker::rules() {
	using namespace rules;
	size_t n = count(kRules);
	for (size_t i = 0; i < n && !full(); ++i) {
		const Rule& r = rec<Rule>(kRules, i);
		at(kRules, i);
		id_string("id", r.id);
		label("name", r.name);
		run("firstParameter", kRuleParameters, r.first_parameter, r.parameter_count);
		if (range("parameterCount", r.parameter_count, 1, 255)) {
			limit("parameterCount", r.parameter_count, 8, "parameters of a rule"); // 10.8
		}
		run("firstTimer", kRuleTimers, r.first_timer, r.timer_count);
		limit("timerCount", r.timer_count, 8, "timers");
		run("firstState", kRuleStates, r.first_state, r.state_count);
		limit("stateCount", r.state_count, 16, "states");
		run("firstReaction", kRuleReactions, r.first_reaction, r.reaction_count);
		limit("reactionCount", r.reaction_count, 16, "reactions");
		if ((r.initial_state == kNone8) != (r.state_count == 0)) {
			refuse(path("initialState"), "malformed: initial state %u, the rule has %u states",
				static_cast<unsigned>(r.initial_state), static_cast<unsigned>(r.state_count));
		} else if (r.state_count > 0) {
			local("initialState", "state", r.initial_state, r.state_count, false);
		}
		if (index("timeline", kRuleTimelines, r.timeline, true) && r.timeline != kNone && r.state_count == 0) {
			refuse(path("timeline"), "malformed: a timeline in a rule without states");
		}
		index("stateDatapoint", kDatapoints, r.state_datapoint, true);
	}
	// The records of each rule in its context.
	for (size_t i = 0; i < n && !full(); ++i) {
		const Rule& r = rec<Rule>(kRules, i);
		if (static_cast<size_t>(r.first_parameter) + r.parameter_count > count(kRuleParameters) ||
			static_cast<size_t>(r.first_timer) + r.timer_count > count(kRuleTimers) ||
			static_cast<size_t>(r.first_state) + r.state_count > count(kRuleStates) ||
			static_cast<size_t>(r.first_reaction) + r.reaction_count > count(kRuleReactions)) {
			continue; // refused above
		}
		RuleContext cx{i, r.parameter_count, r.timer_count, r.state_count};
		for (size_t p = 0; p < r.parameter_count; ++p) {
			size_t q = r.first_parameter + p;
			const Parameter& d = rec<Parameter>(kRuleParameters, q);
			at(kRuleParameters, q);
			id_string("id", d.id);
			label("label", d.label);
			string("unit", d.unit);
			uint8_t type = static_cast<uint8_t>(d.type);
			if (type > 11 || !(type <= 5 || type == 8 || type == 10)) {
				refuse(path("type"), "malformed: type %u is unknown (0..5, 8, 10)%s", static_cast<unsigned>(type), hint_);
				continue;
			}
			if (p == 0 && d.type != Type::boolean) {
				refuse(path("type"), "malformed: parameter 0 of a rule is its enable switch, a boolean");
			}
			boolean("adjustable", reinterpret_cast<const uint8_t*>(&d)[13]); // the bool's byte, not the bool
			run("firstChoice", kRuleChoices, d.first_choice, d.choice_count);
			limit("choiceCount", d.choice_count, 16, "choices");
			auto typed = [&](const char* field, uint16_t c, bool none_allowed, bool step) {
				if (!index(field, kConstants, c, none_allowed) || c == kNone) {
					return;
				}
				Type ct = rec<Val>(kConstants, c).type;
				bool fits = step ? (ct == Type::real || ct == Type::integer || ct == Type::duration) : ct == d.type;
				if (!fits) {
					refuse(path(field), "malformed: constant %u is a %s, the parameter %s", static_cast<unsigned>(c),
						type_name(ct), step ? "needs a real, integer or duration" : type_name(d.type));
				}
			};
			typed("default", d.default_value, false, false);
			typed("min", d.min, true, false);
			typed("max", d.max, true, false);
			typed("step", d.step, true, true);
			index("datapoint", kDatapoints, d.datapoint, false);
			if (static_cast<size_t>(d.first_choice) + d.choice_count <= count(kRuleChoices)) {
				for (size_t k = 0; k < d.choice_count; ++k) {
					const Choice& c = rec<Choice>(kRuleChoices, d.first_choice + k);
					at(kRuleChoices, d.first_choice + k);
					label("label", c.label);
					if (index("value", kConstants, c.value, false) && rec<Val>(kConstants, c.value).type != d.type) {
						refuse(path("value"), "malformed: constant %u is a %s, the parameter %s",
							static_cast<unsigned>(c.value), type_name(rec<Val>(kConstants, c.value).type),
							type_name(d.type));
					}
				}
			}
		}
		for (size_t t = 0; t < r.timer_count; ++t) {
			const Timer& timer = rec<Timer>(kRuleTimers, r.first_timer + t);
			at(kRuleTimers, r.first_timer + t);
			id_string("id", timer.id);
			if (index("duration", kNodes, timer.duration, false)) {
				expression(timer.duration, cx, path("duration"));
			}
		}
		auto action_list = [&](uint16_t first, uint8_t n_actions) {
			for (size_t k = 0; k < n_actions && !full(); ++k) {
				size_t ai = first + k;
				const Action& a = rec<Action>(kRuleActions, ai);
				at(kRuleActions, ai);
				if (!code("kind", static_cast<uint8_t>(a.kind), 6)) {
					continue;
				}
				switch (a.kind) {
					case ActionKind::write:
						flags("flags", a.flags, 0x01);
						index("target", kDatapoints, a.target, false);
						if (index("expr", kNodes, a.expr, false)) {
							expression(a.expr, cx, path("expr"));
						}
						break;
					case ActionKind::start_timer:
					case ActionKind::restart_timer:
						flags("flags", a.flags, 0);
						local("target", "timer", a.target, cx.timers, false);
						if (index("expr", kNodes, a.expr, true) && a.expr != kNone) {
							expression(a.expr, cx, path("expr"));
						}
						break;
					case ActionKind::cancel_timer:
						flags("flags", a.flags, 0);
						local("target", "timer", a.target, cx.timers, false);
						break;
					case ActionKind::set_parameter:
						flags("flags", a.flags, 0);
						local("target", "parameter", a.target, cx.parameters, false);
						if (index("expr", kNodes, a.expr, false)) {
							expression(a.expr, cx, path("expr"));
						}
						break;
					case ActionKind::log:
						code("flags", a.flags, 2);
						if (range("count", a.count, 1, 65535) && limit("count", a.count, 8, "log parts") &&
							run("target", kRuleLogParts, a.target, a.count)) {
							for (size_t q = 0; q < a.count; ++q) {
								const LogPart& part = rec<LogPart>(kRuleLogParts, a.target + q);
								at(kRuleLogParts, a.target + q);
								bool text = part.text != 0 && string("text", part.text) && view_.str(part.text)[0] != '\0';
								bool expr = part.expr != kNone;
								if (text == expr) {
									refuse(path("text"), "malformed: a log part is a text or an expression");
								} else if (expr && index("expr", kNodes, part.expr, false)) {
									expression(part.expr, cx, path("expr"));
								}
							}
						}
						break;
					case ActionKind::read:
						flags("flags", a.flags, 0);
						index("target", kDatapoints, a.target, false);
						break;
				}
			}
		};
		for (size_t s = 0; s < r.state_count; ++s) {
			const State& st = rec<State>(kRuleStates, r.first_state + s);
			at(kRuleStates, r.first_state + s);
			id_string("id", st.id);
			label("label", st.label);
			bool entry_ok = run("firstEntry", kRuleActions, st.entry.first, st.entry.count) &&
				limit("entryCount", st.entry.count, 16, "actions");
			bool exit_ok = run("firstExit", kRuleActions, st.exit.first, st.exit.count) &&
				limit("exitCount", st.exit.count, 16, "actions");
			if (entry_ok) {
				action_list(st.entry.first, st.entry.count);
			}
			if (exit_ok) {
				action_list(st.exit.first, st.exit.count);
			}
		}
		for (size_t k = 0; k < r.reaction_count && !full(); ++k) {
			size_t ri = r.first_reaction + k;
			const Reaction& re = rec<Reaction>(kRuleReactions, ri);
			at(kRuleReactions, ri);
			bool triggers_ok = run("firstTrigger", kRuleTriggers, re.first_trigger, re.trigger_count) &&
				range("triggerCount", re.trigger_count, 1, 255) && limit("triggerCount", re.trigger_count, 8, "triggers of a reaction");
			local("to", "state", re.to, cx.states, true);
			if (index("condition", kNodes, re.condition, true) && re.condition != kNone) {
				expression(re.condition, cx, path("condition"));
			}
			at(kRuleReactions, ri);
			if (re.from_mask != 0xFFFF && cx.states < 16 && (re.from_mask >> cx.states) != 0) {
				unsigned bad = 0;
				while (((re.from_mask >> bad) & 1u) == 0 || bad < cx.states) {
					++bad;
				}
				refuse(path("from"), "malformed: from names state %u, the rule has %u", bad,
					static_cast<unsigned>(cx.states));
			}
			bool actions_ok = run("firstAction", kRuleActions, re.actions.first, re.actions.count) &&
				limit("actionCount", re.actions.count, 16, "actions");
			bool else_ok = run("firstElse", kRuleActions, re.else_actions.first, re.else_actions.count) &&
				limit("elseCount", re.else_actions.count, 16, "actions");
			if (triggers_ok) {
				for (size_t q = 0; q < re.trigger_count; ++q) {
					size_t ti = re.first_trigger + q;
					const Trigger& t = rec<Trigger>(kRuleTriggers, ti);
					at(kRuleTriggers, ti);
					if (!code("kind", static_cast<uint8_t>(t.kind), 6)) {
						continue;
					}
					switch (t.kind) {
						case TriggerKind::datapoint:
							flags("flags", t.flags, 0x07);
							index("ref", kDatapoints, t.ref, false);
							break;
						case TriggerKind::match:
							flags("flags", t.flags, 0);
							if (index("ref", kNodes, t.ref, false)) {
								expression(t.ref, cx, path("ref"));
							}
							break;
						case TriggerKind::schedule:
							flags("flags", t.flags, 0);
							index("ref", kSchedules, t.ref, false);
							break;
						case TriggerKind::timer:
							flags("flags", t.flags, 0);
							local("ref", "timer", t.ref, cx.timers, false);
							break;
						case TriggerKind::start:
							flags("flags", t.flags, 0x07);
							break;
						case TriggerKind::parameter:
							flags("flags", t.flags, 0);
							local("ref", "parameter", t.ref, cx.parameters, false);
							break;
						case TriggerKind::silence:
							flags("flags", t.flags, 0);
							index("ref", kDatapoints, t.ref, false);
							if (index("ref2", kNodes, t.ref2, false)) {
								expression(t.ref2, cx, path("ref2"));
							}
							break;
					}
				}
			}
			if (actions_ok) {
				action_list(re.actions.first, re.actions.count);
			}
			if (else_ok) {
				action_list(re.else_actions.first, re.else_actions.count);
			}
		}
		// The timeline (spec 2.8) of a rule with states.
		if (r.timeline != kNone && r.timeline < count(kRuleTimelines) && r.state_count > 0) {
			struct TimelineRec {
				uint16_t first_plan, first_assignment;
				uint8_t plan_count, assignment_count, flags, reserved;
			};
			struct PlanRec {
				uint32_t id, label;
				uint16_t first_point;
				uint8_t point_count, reserved;
			};
			struct AssignmentRec {
				uint8_t plan, days;
			};
			const TimelineRec& tl = rec<TimelineRec>(kRuleTimelines, r.timeline);
			at(kRuleTimelines, r.timeline);
			bool plans_ok = run("firstPlan", kRulePlans, tl.first_plan, tl.plan_count) &&
				range("planCount", tl.plan_count, 1, 255) && limit("planCount", tl.plan_count, 8, "plans of a timeline");
			bool assignments_ok = run("firstAssignment", kRuleAssignments, tl.first_assignment, tl.assignment_count) &&
				range("assignmentCount", tl.assignment_count, 1, 255) && limit("assignmentCount", tl.assignment_count, 8, "assignments of a timeline");
			flags("flags", tl.flags, 0x01);
			size_t points = 0;
			if (plans_ok) {
				for (size_t p = 0; p < tl.plan_count; ++p) {
					const PlanRec& plan = rec<PlanRec>(kRulePlans, tl.first_plan + p);
					at(kRulePlans, tl.first_plan + p);
					id_string("id", plan.id);
					if (label("label", plan.label)) {
						limit("label", label_count(plan.label), rules::TimelineLimits::kPlanLabelLanguages,
							"languages of a plan's label");
					}
					if (!run("firstPoint", kRulePoints, plan.first_point, plan.point_count) ||
						!limit("pointCount", plan.point_count, 16, "points of a plan")) {
						continue;
					}
					points += plan.point_count;
					for (size_t q = 0; q < plan.point_count; ++q) {
						const TimelinePoint& pt = rec<TimelinePoint>(kRulePoints, plan.first_point + q);
						at(kRulePoints, plan.first_point + q);
						local("state", "state", pt.state, cx.states, false);
						code("sun", pt.sun, 4);
						flags("flags", pt.flags, 0x03);
						if (pt.sun == 0) {
							range("seconds", pt.seconds, 0, 86399);
						} else {
							range("seconds", pt.seconds, -43200, 43200);
						}
						if ((pt.flags & 0x01) != 0) {
							range("notBefore", pt.not_before, 0, 86399);
						}
						if ((pt.flags & 0x02) != 0) {
							range("notAfter", pt.not_after, 0, 86399);
						}
					}
				}
				at(kRuleTimelines, r.timeline);
				limit("firstPlan", points, 64, "points of a timeline");
			}
			if (assignments_ok) {
				for (size_t q = 0; q < tl.assignment_count; ++q) {
					const AssignmentRec& a = rec<AssignmentRec>(kRuleAssignments, tl.first_assignment + q);
					at(kRuleAssignments, tl.first_assignment + q);
					if (a.plan >= tl.plan_count) {
						refuse(path("plan"), "malformed: plan %u does not exist (the timeline has %u)",
							static_cast<unsigned>(a.plan), static_cast<unsigned>(tl.plan_count));
					}
					flags("days", a.days, 0x7F);
				}
			}
		}
	}
}

// ---- 10.6 ----------------------------------------------------------------------------------------

bool Checker::expression(uint16_t root, const RuleContext& rule, const char* where) {
	using namespace rules;
	(void)where;
	char p[40];
	auto node_path = [&](size_t n, const char* field) {
		if (field == nullptr) {
			std::snprintf(p, sizeof(p), "nodes[%u]", static_cast<unsigned>(n));
		} else {
			std::snprintf(p, sizeof(p), "nodes[%u].%s", static_cast<unsigned>(n), field);
		}
		return p;
	};
	size_t total = count(kNodes);
	if (root >= total) {
		return false; // refused by the caller's index check
	}
	const Node& r = rec<Node>(kNodes, root);
	if (r.size == 0 || static_cast<size_t>(root) + r.size > total) {
		refuse(node_path(root, "size"), "malformed: the expression at node %u reaches beyond the %u nodes",
			static_cast<unsigned>(root), static_cast<unsigned>(total));
		return false;
	}
	if (r.size > Limits::kNodesPerExpression) {
		refuse(node_path(root, "size"), "limitExceeded: %u nodes in an expression, at most %u",
			static_cast<unsigned>(r.size), static_cast<unsigned>(Limits::kNodesPerExpression));
		return false;
	}
	size_t end = static_cast<size_t>(root) + r.size;
	uint8_t parents[Limits::kNodesPerExpression] = {};
	uint8_t depth[Limits::kNodesPerExpression] = {};
	bool timer_arg[Limits::kNodesPerExpression] = {};
	bool ok = true;
	depth[0] = 1;
	for (size_t n = root; n < end; ++n) {
		const Node& node = rec<Node>(kNodes, n);
		size_t local_n = n - root;
		uint8_t kind = static_cast<uint8_t>(node.kind);
		if (kind > 6) {
			refuse(node_path(n, "kind"), "malformed: kind %u is unknown (0..6)%s", static_cast<unsigned>(kind), hint_);
			return false;
		}
		if (static_cast<uint8_t>(node.type) > 11) {
			refuse(node_path(n, "type"), "malformed: type %u is unknown (0..11)%s", static_cast<unsigned>(node.type),
				hint_);
			return false;
		}
		size_t want_min = 0, want_max = 0;
		switch (node.kind) {
			case NodeKind::unary:
				if (node.op > 1) {
					refuse(node_path(n, "op"), "malformed: op %u is unknown (0..1)%s", static_cast<unsigned>(node.op), hint_);
					return false;
				}
				want_min = want_max = 1;
				break;
			case NodeKind::binary:
				if (node.op < 2 || node.op > 14) {
					refuse(node_path(n, "op"), "malformed: op %u is unknown (2..14)%s", static_cast<unsigned>(node.op), hint_);
					return false;
				}
				want_min = want_max = 2;
				break;
			case NodeKind::call:
				if (node.op >= static_cast<uint8_t>(Fn::count)) {
					refuse(node_path(n, "op"), "malformed: op %u is unknown (0..%u)%s", static_cast<unsigned>(node.op),
						static_cast<unsigned>(Fn::count) - 1, hint_);
					return false;
				}
				want_min = kArity[node.op].min;
				want_max = kArity[node.op].max;
				break;
			default: break;
		}
		if (node.argc < want_min || node.argc > want_max) {
			if (node.kind == NodeKind::call) {
				if (want_min == want_max) {
					refuse(node_path(n, "argc"), "malformed: %s() takes %u arguments, the node has %u",
						function_name(static_cast<Fn>(node.op)), static_cast<unsigned>(want_min),
						static_cast<unsigned>(node.argc));
				} else {
					refuse(node_path(n, "argc"), "malformed: %s() takes %u..%u arguments, the node has %u",
						function_name(static_cast<Fn>(node.op)), static_cast<unsigned>(want_min),
						static_cast<unsigned>(want_max), static_cast<unsigned>(node.argc));
				}
			} else {
				refuse(node_path(n, "argc"), "malformed: a node of kind %u has %u children, not %u",
					static_cast<unsigned>(kind), static_cast<unsigned>(node.argc), static_cast<unsigned>(want_min));
			}
			return false;
		}
		if (node.argc > 0) {
			if (node.a == kNone || node.a <= n || static_cast<size_t>(node.a) + node.argc > end) {
				refuse(node_path(n, "a"), "malformed: the children %u..%u of node %u lie outside its expression (nodes %u..%u, after the node)",
					static_cast<unsigned>(node.a), static_cast<unsigned>(node.a + node.argc - 1),
					static_cast<unsigned>(n), static_cast<unsigned>(root), static_cast<unsigned>(end - 1));
				return false;
			}
			for (size_t c = node.a; c < static_cast<size_t>(node.a) + node.argc; ++c) {
				size_t lc = c - root;
				if (++parents[lc] > 1) {
					refuse(node_path(c, nullptr), "malformed: node %u is a child of two nodes", static_cast<unsigned>(c));
					return false;
				}
				depth[lc] = static_cast<uint8_t>(depth[local_n] + 1);
				if (depth[lc] > Limits::kDepth) {
					refuse(node_path(root, nullptr), "limitExceeded: an expression deeper than %u",
						static_cast<unsigned>(Limits::kDepth));
					return false;
				}
			}
		} else if (node.kind == NodeKind::call && node.a != kNone) {
			refuse(node_path(n, "a"), "malformed: a call without arguments has a %u, not none", static_cast<unsigned>(node.a));
			return false;
		}
		if (local_n > 0 && parents[local_n] == 0) {
			refuse(node_path(n, nullptr), "malformed: node %u of the expression at node %u is no node of its tree",
				static_cast<unsigned>(n), static_cast<unsigned>(root));
			return false;
		}
		// The node's own fields.
		const uint8_t* lit = reinterpret_cast<const uint8_t*>(&node.lit);
		switch (node.kind) {
			case NodeKind::literal: {
				Type t = node.type;
				if (t == Type::string || t == Type::raw || t == Type::date_time) {
					if (node.a >= count(kConstants)) {
						refuse(node_path(n, "a"), "malformed: constant %u does not exist (%u constants)",
							static_cast<unsigned>(node.a), static_cast<unsigned>(count(kConstants)));
						ok = false;
					} else if (rec<Val>(kConstants, node.a).type != t) {
						refuse(node_path(n, "a"), "malformed: constant %u is a %s, the literal a %s",
							static_cast<unsigned>(node.a), type_name(rec<Val>(kConstants, node.a).type), type_name(t));
						ok = false;
					}
				} else if (t == Type::boolean && lit[0] > 1) {
					refuse(node_path(n, "lit"), "malformed: %u is not a boolean (0 or 1)", static_cast<unsigned>(lit[0]));
					ok = false;
				} else if (t == Type::time_of_day && (node.lit.tod.day_of_week > 7 || node.lit.tod.hour > 23 ||
					node.lit.tod.minute > 59 || node.lit.tod.second > 59)) {
					refuse(node_path(n, "lit"), "malformed: a time of day out of its range");
					ok = false;
				} else if (t == Type::date && (node.lit.date.year < 1900 || node.lit.date.year > 2155 ||
					node.lit.date.month < 1 || node.lit.date.month > 12 || node.lit.date.day < 1 || node.lit.date.day > 31)) {
					refuse(node_path(n, "lit"), "malformed: a date out of its range");
					ok = false;
				} else if (t == Type::real && !std::isfinite(node.lit.r)) {
					refuse(node_path(n, "lit"), "malformed: a real literal that is not finite");
					ok = false;
				} else if (t == Type::controlled && lit[0] > 1) {
					refuse(node_path(n, "lit"), "malformed: %u is not a boolean (0 or 1)", static_cast<unsigned>(lit[0]));
					ok = false;
				} else if (t == Type::color && lit[4] > 0x0F) {
					refuse(node_path(n, "lit"), "malformed: unknown bits 0x%x in valid%s", static_cast<unsigned>(lit[4] & ~0x0F),
						hint_);
					ok = false;
				}
				break;
			}
			case NodeKind::datapoint:
				if (node.a >= count(kDatapoints)) {
					refuse(node_path(n, "a"), "malformed: datapoint %u does not exist (%u datapoints)",
						static_cast<unsigned>(node.a), static_cast<unsigned>(count(kDatapoints)));
					ok = false;
				}
				break;
			case NodeKind::parameter:
				if (node.a >= rule.parameters) {
					refuse(node_path(n, "a"), "malformed: parameter %u does not exist (the rule has %u)",
						static_cast<unsigned>(node.a), static_cast<unsigned>(rule.parameters));
					ok = false;
				}
				break;
			case NodeKind::timer:
				if (node.a >= rule.timers) {
					refuse(node_path(n, "a"), "malformed: timer %u does not exist (the rule has %u)",
						static_cast<unsigned>(node.a), static_cast<unsigned>(rule.timers));
					ok = false;
				} else if (!timer_arg[local_n]) {
					refuse(node_path(n, "a"), "malformed: a timer node outside running() and remaining()");
					ok = false;
				}
				break;
			case NodeKind::call: {
				Fn fn = static_cast<Fn>(node.op);
				const Node* first = node.argc > 0 ? &rec<Node>(kNodes, node.a) : nullptr;
				if (fn >= Fn::since_update && fn <= Fn::quality) {
					if (first->kind != NodeKind::datapoint) {
						refuse(node_path(n, "a"), "malformed: the first argument of %s() must be a datapoint node",
							function_name(fn));
						ok = false;
					}
				} else if (fn == Fn::running || fn == Fn::remaining) {
					if (first->kind != NodeKind::timer) {
						refuse(node_path(n, "a"), "malformed: the first argument of %s() must be a timer node",
							function_name(fn));
						ok = false;
					} else {
						timer_arg[node.a - root] = true;
					}
				} else if (fn == Fn::state) {
					uint32_t target = node.lit.e;
					if (target >= count(kRules) || rec<Rule>(kRules, target).state_count == 0) {
						refuse(node_path(n, "lit"), "malformed: state() reads rule %u, which has no states",
							static_cast<unsigned>(target));
						ok = false;
					}
					if (first != nullptr && (first->kind != NodeKind::literal || first->type != Type::string)) {
						refuse(node_path(n, "a"), "malformed: the argument of state() must be a string literal");
						ok = false;
					}
				}
				break;
			}
			default: break;
		}
	}
	// Sizes, bottom up: the children lie after their parent.
	uint16_t sizes[Limits::kNodesPerExpression];
	for (size_t n = end; n-- > root;) {
		const Node& node = rec<Node>(kNodes, n);
		size_t sum = 1;
		for (size_t c = 0; c < node.argc; ++c) {
			sum += sizes[node.a + c - root];
		}
		sizes[n - root] = static_cast<uint16_t>(sum);
		if (node.size != sum) {
			refuse(node_path(n, "size"), "malformed: size %u, the subtree has %u nodes", static_cast<unsigned>(node.size),
				static_cast<unsigned>(sum));
			return false;
		}
	}
	return ok;
}

void Checker::expressions() {
	if (count(kNodes) > rules::Limits::kNodes) {
		refuse("nodes", "limitExceeded: %u nodes, at most %u", static_cast<unsigned>(count(kNodes)),
			static_cast<unsigned>(rules::Limits::kNodes));
	}
}

// ---- 10.7 ----------------------------------------------------------------------------------------

void Checker::owners() {
	using namespace rules;
	size_t datapoints = count(kDatapoints);
	auto mismatch = [&](size_t d, uint16_t table, size_t index, uint32_t names) {
		const SectionInfo* info = section_info(table);
		at(kDatapoints, d);
		refuse(path("owner"), "malformed: datapoint %u names %s %u as its owner, which names datapoint %u",
			static_cast<unsigned>(d), info->name, static_cast<unsigned>(index), static_cast<unsigned>(names));
	};
	for (size_t d = 0; d < datapoints && !full(); ++d) {
		const DatapointRec& r = rec<DatapointRec>(kDatapoints, d);
		at(kDatapoints, d);
		uint16_t oi = r.owner_index;
		uint16_t om = r.owner_member;
		switch (r.owner) {
			case kOwnerNone:
				break;
			case kOwnerKnx:
			case kOwnerTimeMaster:
				if (index("ownerIndex", kKnxBindings, oi, false)) {
					uint16_t names = rec<KnxBindingRec>(kKnxBindings, oi).datapoint;
					if (names != d) {
						mismatch(d, kKnxBindings, oi, names);
					} else if (r.owner == kOwnerTimeMaster) {
						const TimeMasterRec* tm = count(kKnxTimeMaster) > 0 ? &rec<TimeMasterRec>(kKnxTimeMaster, 0) : nullptr;
						uint16_t named = tm == nullptr ? kNone : om == 0 ? tm->time : om == 1 ? tm->date : om == 2 ? tm->date_time : kNone;
						if (named != d) {
							refuse(path("ownerMember"), "malformed: datapoint %u is member %u of the time master, which names datapoint %u",
								static_cast<unsigned>(d), static_cast<unsigned>(om), static_cast<unsigned>(named));
						}
					}
				}
				break;
			case kOwnerScheduler:
				if (index("ownerIndex", kSchedules, oi, false)) {
					const ScheduleRec& s = rec<ScheduleRec>(kSchedules, oi);
					uint16_t names = om == 0 ? s.output : om == 1 ? s.time_datapoint : kNone;
					if (names != d) {
						mismatch(d, kSchedules, oi, names);
					}
				}
				break;
			case kOwnerZigbee:
				if (index("ownerIndex", kZigbeeBindings, oi, false)) {
					uint16_t names = rec<ZigbeeBindingRec>(kZigbeeBindings, oi).datapoint;
					if (names != d) {
						mismatch(d, kZigbeeBindings, oi, names);
					}
				}
				break;
			case kOwnerRule:
				if (index("ownerIndex", kRules, oi, false)) {
					const Rule& rule = rec<Rule>(kRules, oi);
					uint16_t names = kNone;
					if (om == kRuleStateMember) {
						names = rule.state_datapoint;
					} else if (om < rule.parameter_count &&
						static_cast<size_t>(rule.first_parameter) + om < count(kRuleParameters)) {
						names = rec<Parameter>(kRuleParameters, rule.first_parameter + om).datapoint;
					}
					if (names != d) {
						mismatch(d, kRules, oi, names);
					}
				}
				break;
			case kOwnerBlock:
				if (index("ownerIndex", kBlocks, oi, false)) {
					const BlockRec& b = rec<BlockRec>(kBlocks, oi);
					uint8_t kind = static_cast<uint8_t>(om >> 8);
					uint8_t i = static_cast<uint8_t>(om & 0xFF);
					uint16_t names = kNone;
					if (kind == 0 && i == 0) {
						names = b.enabled_datapoint;
					} else if (kind == 1 && i < b.parameter_count &&
						static_cast<size_t>(b.first_parameter) + i < count(kBlockParameters)) {
						names = rec<BlockParameterRec>(kBlockParameters, b.first_parameter + i).datapoint;
					} else if ((kind == 2 || kind == 3) && i < b.port_count &&
						static_cast<size_t>(b.first_port) + i < count(kBlockPorts)) {
						names = rec<BlockPortRec>(kBlockPorts, b.first_port + i).mirror;
					}
					if (names != d) {
						mismatch(d, kBlocks, oi, names);
					}
				}
				break;
			case kOwnerExternal:
				if (index("ownerIndex", kExternalBindings, oi, false)) {
					uint16_t names = rec<ExternalBindingRec>(kExternalBindings, oi).datapoint;
					if (names != d) {
						mismatch(d, kExternalBindings, oi, names);
					}
				}
				break;
			default: break;
		}
	}
	// The reverse: every owner record's datapoint points back to it.
	auto back = [&](uint16_t table, size_t i, const char* field, uint16_t d, uint8_t owner, uint16_t member,
					 bool member_matters) {
		if (d == kNone || d >= datapoints || full()) {
			return;
		}
		const DatapointRec& r = rec<DatapointRec>(kDatapoints, d);
		bool owner_ok = r.owner == owner || (owner == kOwnerKnx && r.owner == kOwnerTimeMaster);
		if (!owner_ok || r.owner_index != i || (member_matters && r.owner_member != member)) {
			at(table, i);
			refuse(path(field), "malformed: %s %u names datapoint %u, whose owner is %s",
				section_info(table)->name, static_cast<unsigned>(i), static_cast<unsigned>(d),
				r.owner <= 7 ? kOwnerNames[r.owner] : "unknown");
		}
	};
	for (size_t i = 0; i < count(kKnxBindings); ++i) {
		back(kKnxBindings, i, "datapoint", rec<KnxBindingRec>(kKnxBindings, i).datapoint, kOwnerKnx, 0, false);
	}
	for (size_t i = 0; i < count(kZigbeeBindings); ++i) {
		back(kZigbeeBindings, i, "datapoint", rec<ZigbeeBindingRec>(kZigbeeBindings, i).datapoint, kOwnerZigbee, 0, true);
	}
	for (size_t i = 0; i < count(kExternalBindings); ++i) {
		back(kExternalBindings, i, "datapoint", rec<ExternalBindingRec>(kExternalBindings, i).datapoint, kOwnerExternal,
			0, true);
	}
	for (size_t i = 0; i < count(kSchedules); ++i) {
		const ScheduleRec& s = rec<ScheduleRec>(kSchedules, i);
		back(kSchedules, i, "output", s.output, kOwnerScheduler, 0, true);
		back(kSchedules, i, "timeDatapoint", s.time_datapoint, kOwnerScheduler, 1, true);
	}
	for (size_t i = 0; i < count(kRules); ++i) {
		const Rule& r = rec<Rule>(kRules, i);
		back(kRules, i, "stateDatapoint", r.state_datapoint, kOwnerRule, kRuleStateMember, true);
		if (static_cast<size_t>(r.first_parameter) + r.parameter_count <= count(kRuleParameters)) {
			for (size_t p = 0; p < r.parameter_count && !full(); ++p) {
				const Parameter& d = rec<Parameter>(kRuleParameters, r.first_parameter + p);
				if (d.datapoint == kNone || d.datapoint >= datapoints) {
					continue;
				}
				const DatapointRec& dr = rec<DatapointRec>(kDatapoints, d.datapoint);
				if (dr.owner != kOwnerRule || dr.owner_index != i || dr.owner_member != p) {
					at(kRuleParameters, r.first_parameter + p);
					refuse(path("datapoint"), "malformed: rules.parameters %u names datapoint %u, whose owner is %s",
						static_cast<unsigned>(r.first_parameter + p), static_cast<unsigned>(d.datapoint),
						dr.owner <= 7 ? kOwnerNames[dr.owner] : "unknown");
				}
			}
		}
	}
	for (size_t i = 0; i < count(kBlocks); ++i) {
		const BlockRec& b = rec<BlockRec>(kBlocks, i);
		back(kBlocks, i, "enabledDatapoint", b.enabled_datapoint, kOwnerBlock, 0, true);
		if (static_cast<size_t>(b.first_parameter) + b.parameter_count <= count(kBlockParameters)) {
			for (size_t p = 0; p < b.parameter_count; ++p) {
				uint16_t d = rec<BlockParameterRec>(kBlockParameters, b.first_parameter + p).datapoint;
				if (d != kNone && d < datapoints) {
					const DatapointRec& dr = rec<DatapointRec>(kDatapoints, d);
					if (dr.owner != kOwnerBlock || dr.owner_index != i || dr.owner_member != (0x100 | p)) {
						at(kBlockParameters, b.first_parameter + p);
						refuse(path("datapoint"), "malformed: blocks.parameters %u names datapoint %u, whose owner is %s",
							static_cast<unsigned>(b.first_parameter + p), static_cast<unsigned>(d),
							dr.owner <= 7 ? kOwnerNames[dr.owner] : "unknown");
					}
				}
			}
		}
		if (static_cast<size_t>(b.first_port) + b.port_count <= count(kBlockPorts)) {
			for (size_t p = 0; p < b.port_count; ++p) {
				uint16_t d = rec<BlockPortRec>(kBlockPorts, b.first_port + p).mirror;
				if (d != kNone && d < datapoints) {
					const DatapointRec& dr = rec<DatapointRec>(kDatapoints, d);
					uint16_t kind = dr.owner_member >> 8;
					if (dr.owner != kOwnerBlock || dr.owner_index != i || (kind != 2 && kind != 3) ||
						(dr.owner_member & 0xFF) != p) {
						at(kBlockPorts, b.first_port + p);
						refuse(path("mirror"), "malformed: blocks.ports %u names datapoint %u, whose owner is %s",
							static_cast<unsigned>(b.first_port + p), static_cast<unsigned>(d),
							dr.owner <= 7 ? kOwnerNames[dr.owner] : "unknown");
					}
				}
			}
		}
	}
	for (size_t i = 0; i < count(kHistory) && !full(); ++i) {
		uint16_t d = rec<HistoryRec>(kHistory, i).datapoint;
		if (d < datapoints && rec<DatapointRec>(kDatapoints, d).history != i) {
			at(kHistory, i);
			refuse(path("datapoint"), "malformed: history %u names datapoint %u, whose history is %u",
				static_cast<unsigned>(i), static_cast<unsigned>(d),
				static_cast<unsigned>(rec<DatapointRec>(kDatapoints, d).history));
		}
	}
	for (size_t d = 0; d < datapoints && !full(); ++d) {
		uint16_t h = rec<DatapointRec>(kDatapoints, d).history;
		if (h != kNone && h < count(kHistory) && rec<HistoryRec>(kHistory, h).datapoint != d) {
			at(kDatapoints, d);
			refuse(path("history"), "malformed: datapoint %u names history %u, which names datapoint %u",
				static_cast<unsigned>(d), static_cast<unsigned>(h),
				static_cast<unsigned>(rec<HistoryRec>(kHistory, h).datapoint));
		}
	}
	// The addresses and the bindings agree.
	size_t addresses = count(kKnxAddresses);
	size_t uses = 0;
	char text[knx::GroupAddress::kMaxText];
	auto listened = [&](size_t address, size_t binding) {
		const KnxAddressRec& a = rec<KnxAddressRec>(kKnxAddresses, address);
		if (static_cast<size_t>(a.first_listener) + a.listener_count > count(kKnxListeners)) {
			return true; // refused before
		}
		for (size_t k = 0; k < a.listener_count; ++k) {
			if (rec<uint16_t>(kKnxListeners, a.first_listener + k) == binding) {
				return true;
			}
		}
		return false;
	};
	for (size_t b = 0; b < count(kKnxBindings) && !full(); ++b) {
		const KnxBindingRec& r = rec<KnxBindingRec>(kKnxBindings, b);
		if (r.send != kNone && r.send < addresses) {
			++uses;
			if (!listened(r.send, b)) {
				at(kKnxBindings, b);
				knx::GroupAddress{rec<KnxAddressRec>(kKnxAddresses, r.send).address}.format(text, sizeof(text));
				refuse(path("send"), "malformed: knx.bindings %u sends on address %s, whose listeners do not name it",
					static_cast<unsigned>(b), text);
			}
		}
		if (static_cast<size_t>(r.first_listen) + r.listen_count > count(kKnxListen)) {
			continue;
		}
		for (size_t k = 0; k < r.listen_count; ++k) {
			uint16_t a = rec<uint16_t>(kKnxListen, r.first_listen + k);
			if (a >= addresses) {
				continue;
			}
			++uses;
			if (!listened(a, b)) {
				at(kKnxBindings, b);
				knx::GroupAddress{rec<KnxAddressRec>(kKnxAddresses, a).address}.format(text, sizeof(text));
				refuse(path("firstListen"), "malformed: knx.bindings %u listens on address %s, whose listeners do not name it",
					static_cast<unsigned>(b), text);
			}
		}
	}
	size_t listeners = 0;
	for (size_t a = 0; a < addresses; ++a) {
		listeners += rec<KnxAddressRec>(kKnxAddresses, a).listener_count;
	}
	if (listeners != uses && !full()) {
		refuse("knx.listeners", "malformed: %u listeners, the bindings use addresses %u times",
			static_cast<unsigned>(listeners), static_cast<unsigned>(uses));
	}
}

// ---- 10.8: the counted limits ------------------------------------------------------------------

void Checker::limits() {
	size_t declared = 0;
	for (size_t d = 0; d < count(kDatapoints); ++d) {
		const DatapointRec& r = rec<DatapointRec>(kDatapoints, d);
		if (r.owner == kOwnerNone || r.owner == kOwnerKnx || r.owner == kOwnerZigbee || r.owner == kOwnerExternal ||
			(r.owner == kOwnerScheduler && r.owner_member == 0)) {
			++declared;
		}
	}
	auto over = [&](const char* where, size_t value, size_t max, const char* what) {
		if (value > max) {
			refuse(where, "limitExceeded: %u %s, at most %u", static_cast<unsigned>(value), what,
				static_cast<unsigned>(max));
		}
	};
	over("datapoints", declared, 1024, "declared datapoints");
	over("locations", count(kLocations), 256, "locations");
	over("functions", count(kFunctions), 512, "functions");
	over("schedules", count(kSchedules), 128, "schedules");
	over("rules", count(kRules), 128, "rules");
	over("blocks", count(kBlocks), 32, "blocks");
	over("zigbee.bindings", count(kZigbeeBindings), 512, "Zigbee bindings");
	over("external.sources", count(kExternalSources), 8, "external sources");
	over("external.bindings", count(kExternalBindings), 32, "external bindings");
	size_t points = 0, plans = 0;
	struct TimelineRec {
		uint16_t first_plan, first_assignment;
		uint8_t plan_count, assignment_count, flags, reserved;
	};
	for (size_t t = 0; t < count(kRuleTimelines); ++t) {
		const TimelineRec& tl = rec<TimelineRec>(kRuleTimelines, t);
		plans += tl.plan_count;
	}
	points = count(kRulePoints);
	over("rules.points", points, rules::TimelineLimits::kImagePoints, "points of all timelines");
	over("rules.plans", plans, rules::TimelineLimits::kImagePlans, "plans of all timelines");
}

// ---- the run -------------------------------------------------------------------------------------

void Checker::records() {
	image_record();
	settings_record();
	datapoints();
	locations();
	functions();
	history();
	knx();
	zigbee();
	external();
	mqtt();
	schedules();
	constants();
	rules();
	expressions();
	blocks();
	owners();
	limits();
}

void Checker::blocks() {
	for (size_t i = 0; i < count(kBlocks) && !full(); ++i) {
		const BlockRec& r = rec<BlockRec>(kBlocks, i);
		at(kBlocks, i);
		id_string("id", r.id);
		id_string("type", r.type);
		label("label", r.label);
		run("firstPort", kBlockPorts, r.first_port, r.port_count);
		run("firstParameter", kBlockParameters, r.first_parameter, r.parameter_count);
		flags("flags", r.flags, 0x03);
		index("enabledDatapoint", kDatapoints, r.enabled_datapoint, false);
	}
	for (size_t i = 0; i < count(kBlockPorts) && !full(); ++i) {
		const BlockPortRec& r = rec<BlockPortRec>(kBlockPorts, i);
		at(kBlockPorts, i);
		id_string("id", r.id);
		// The count against the port's `multiple` (at most 32) is the Loader's (10.8 forbidden):
		// the run's bounds are what reading needs.
		run("firstBound", kBlockBound, r.first_bound, r.bound_count);
		code("aggregation", r.aggregation, 5);
		index("mirror", kDatapoints, r.mirror, true);
	}
	for (size_t i = 0; i < count(kBlockBound) && !full(); ++i) {
		at(kBlockBound, i);
		index("datapoint", kDatapoints, rec<uint16_t>(kBlockBound, i), false);
	}
	for (size_t i = 0; i < count(kBlockParameters) && !full(); ++i) {
		const BlockParameterRec& r = rec<BlockParameterRec>(kBlockParameters, i);
		at(kBlockParameters, i);
		id_string("id", r.id);
		index("datapoint", kDatapoints, r.datapoint, true);
		flags("flags", r.flags, 0x01);
		finite("value", r.value);
	}
}

bool Checker::run() {
	if (!sections()) {
		return false;
	}
	view_.open(bytes_);
	if (!strings()) {
		return false;
	}
	size_t before = report_.total;
	records();
	return report_.total == before;
}

} // namespace

const char* newer_hint(const uint8_t* bytes) {
	static char text[112];
	const Header& h = header_of(bytes);
	if (h.major != kFormatMajor || h.minor <= kFormatMinor) {
		return "";
	}
	std::snprintf(text, sizeof(text),
		" (the image's format %u.%u is newer than this firmware reads, up to %u.%u: update the firmware)",
		static_cast<unsigned>(h.major), static_cast<unsigned>(h.minor), static_cast<unsigned>(kFormatMajor),
		static_cast<unsigned>(kFormatMinor));
	return text;
}

namespace {

bool check_magic(const uint8_t* head, Report& report) {
	if (std::memcmp(head, kMagic, 4) == 0) {
		return true;
	}
	report.add(Report::Kind::format, "header.magic",
		"not a runtime image: the magic is %02X %02X %02X %02X, not 48 43 49 4D (\"HCIM\")%s",
		static_cast<unsigned>(head[0]), static_cast<unsigned>(head[1]), static_cast<unsigned>(head[2]),
		static_cast<unsigned>(head[3]),
		head[0] == '{' ? ": a JSON text (E-Home compiles the image; send the binary form)" : "");
	return false;
}

/// The checks of 10.1 after the magic; `body` the announced body (receipt) or the slot size (boot).
bool check_rest(const uint8_t* head, size_t body, bool at_boot, Report& report) {
	const Header& h = header_of(head);
	if (h.major != kFormatMajor) {
		report.add(Report::Kind::version, "header.major", "format %u.%u: this firmware reads format %u (up to %u.%u)",
			static_cast<unsigned>(h.major), static_cast<unsigned>(h.minor), static_cast<unsigned>(kFormatMajor),
			static_cast<unsigned>(kFormatMajor), static_cast<unsigned>(kFormatMinor));
		return false;
	}
	if ((h.flags & ~kFlagSigned) != 0) {
		report.add(Report::Kind::version, "header.flags", "unknown flags 0x%x%s",
			static_cast<unsigned>(h.flags & ~kFlagSigned), newer_hint(head));
		return false;
	}
	if (at_boot ? h.length > body : h.length != body) {
		if (at_boot) {
			report.add(Report::Kind::format, "header.length", "the header says %u bytes, the slot holds %u",
				static_cast<unsigned>(h.length), static_cast<unsigned>(body));
		} else {
			report.add(Report::Kind::format, "header.length", "the header says %u bytes, the body has %u",
				static_cast<unsigned>(h.length), static_cast<unsigned>(body));
		}
		return false;
	}
	if (h.length % 8 != 0) {
		report.add(Report::Kind::format, "header.length", "length %u is not a multiple of 8",
			static_cast<unsigned>(h.length));
		return false;
	}
	size_t minimum = h.header_size + ((h.flags & kFlagSigned) != 0 ? kSignatureSize : 0);
	if (h.header_size < kHeaderSize || h.header_size % 8 != 0) {
		report.add(Report::Kind::format, "header.headerSize", "header size %u, at least 32 and a multiple of 8",
			static_cast<unsigned>(h.header_size));
		return false;
	}
	if (h.length < minimum) {
		if ((h.flags & kFlagSigned) != 0) {
			report.add(Report::Kind::format, "header.length", "length %u leaves no room for the signature",
				static_cast<unsigned>(h.length));
		} else {
			report.add(Report::Kind::format, "header.length", "length %u is shorter than the header (%u)",
				static_cast<unsigned>(h.length), static_cast<unsigned>(h.header_size));
		}
		return false;
	}
	size_t end = signed_end_of(h);
	size_t table_end = h.header_size + static_cast<size_t>(h.section_count) * sizeof(SectionEntry);
	if (table_end > end) {
		report.add(Report::Kind::format, "header.sectionCount",
			"the section table of %u entries ends at %u, beyond the %u signed bytes",
			static_cast<unsigned>(h.section_count), static_cast<unsigned>(table_end), static_cast<unsigned>(end));
		return false;
	}
	return true;
}

} // namespace

bool check_header(const uint8_t* head, size_t available, size_t length, Report& report) {
	size_t have = available < length ? available : length;
	if (length < kHeaderSize || have < kHeaderSize) {
		if (have >= 4 && !check_magic(head, report)) {
			return false;
		}
		report.add(Report::Kind::format, "header", "not a runtime image: %u bytes, the header alone has 32",
			static_cast<unsigned>(length));
		return false;
	}
	return check_magic(head, report) && check_rest(head, length, false, report);
}

bool check_slot_header(const uint8_t* slot, size_t slot_size, bool& erased, Report& report) {
	erased = slot_size >= 4 && slot[0] == 0xFF && slot[1] == 0xFF && slot[2] == 0xFF && slot[3] == 0xFF;
	if (erased) {
		return false;
	}
	if (slot_size < kHeaderSize) {
		report.add(Report::Kind::format, "header", "not a runtime image: %u bytes, the header alone has 32",
			static_cast<unsigned>(slot_size));
		return false;
	}
	return check_magic(slot, report) && check_rest(slot, slot_size, true, report);
}

bool check_crc(const uint8_t* bytes, Report& report) {
	const Header& h = header_of(bytes);
	uint32_t crc = image_crc(bytes, signed_end_of(h));
	if (crc != h.crc) {
		report.add(Report::Kind::damaged, "header.crc",
			"the CRC-32 is %08x, the bytes give %08x: the image was damaged on the way or in the slot",
			static_cast<unsigned>(h.crc), static_cast<unsigned>(crc));
		return false;
	}
	return true;
}

bool check_structure(const uint8_t* bytes, Report& report) {
	Checker checker(bytes, report);
	return checker.run();
}

} // namespace hc::image
