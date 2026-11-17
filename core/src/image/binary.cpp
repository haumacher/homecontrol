/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/image/binary.h"

#include <cstring>

#include "hc/json.h"
#include "hc/value_store.h"

namespace hc::image::bin {

namespace {

// Sizes and alignments of 9.4; the rules' records are the engine's (hc/rules/rule_set.h).
constexpr SectionInfo kSections[] = {
	{kImage, "image", 32, 4, true},
	{kStrings, "strings", 0, 1, false},
	{kLabels, "labels", 8, 4, false},
	{kSettings, "settings", 32, 8, true},
	{kDatapoints, "datapoints", 24, 4, false},
	{kLocations, "locations", 12, 4, false},
	{kFunctions, "functions", 28, 4, false},
	{kRoles, "roles", 8, 4, false},
	{kHistory, "history", 16, 4, false},
	{kArchives, "archives", 8, 4, false},
	{kKnxAddresses, "knx.addresses", 8, 2, false},
	{kKnxBindings, "knx.bindings", 12, 2, false},
	{kKnxListen, "knx.listen", 2, 2, false},
	{kKnxListeners, "knx.listeners", 2, 2, false},
	{kKnxTimeMaster, "knx.timeMaster", 12, 4, true},
	{kZigbeeBindings, "zigbee.bindings", 32, 8, false},
	{kExternalSources, "external.sources", 20, 4, false},
	{kExternalBindings, "external.bindings", 32, 8, false},
	{kMqtt, "mqtt", 20, 4, true},
	{kSchedules, "schedules", 64, 4, false},
	{kScheduleDates, "schedules.dates", 4, 2, false},
	{kScheduleBounds, "schedules.bounds", 32, 8, false},
	{kRules, "rules", 28, 4, false},
	{kRuleParameters, "rules.parameters", 28, 4, false},
	{kRuleChoices, "rules.choices", 8, 4, false},
	{kRuleTimers, "rules.timers", 8, 4, false},
	{kRuleStates, "rules.states", 16, 4, false},
	{kRuleReactions, "rules.reactions", 16, 2, false},
	{kRuleTriggers, "rules.triggers", 8, 2, false},
	{kRuleActions, "rules.actions", 8, 2, false},
	{kRuleLogParts, "rules.logParts", 8, 4, false},
	{kRuleTimelines, "rules.timelines", 8, 2, false},
	{kRulePlans, "rules.plans", 12, 4, false},
	{kRulePoints, "rules.points", 16, 4, false},
	{kRuleAssignments, "rules.assignments", 2, 2, false},
	{kNodes, "nodes", 16, 8, false},
	{kConstants, "constants", 40, 8, false},
	{kBlocks, "blocks", 28, 4, false},
	{kBlockPorts, "blocks.ports", 12, 4, false},
	{kBlockBound, "blocks.bound", 2, 2, false},
	{kBlockParameters, "blocks.parameters", 16, 8, false},
	{kUi, "ui", 0, 1, false},
	{kUiAnonymous, "ui.anonymous", 0, 1, false},
	{kUiAnonymousDatapoints, "ui.anonymous.datapoints", 4, 2, false},
	{kUiAnonymousFunctions, "ui.anonymous.functions", 2, 2, false},
};
constexpr size_t kSectionCount = sizeof(kSections) / sizeof(kSections[0]);

size_t position(uint16_t id) {
	size_t lo = 0, hi = kSectionCount;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (kSections[mid].id < id) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return lo < kSectionCount && kSections[lo].id == id ? lo : kSectionCount;
}

const Table kEmpty;

char lower(char c) {
	return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

/// The tag equals the first `length` bytes of `language`, letters without case (spec/rules.md 2.2).
bool tag_matches(const char* tag, const char* language, size_t length) {
	if (std::strlen(tag) != length) {
		return false;
	}
	for (size_t i = 0; i < length; ++i) {
		if (lower(tag[i]) != lower(language[i])) {
			return false;
		}
	}
	return true;
}

} // namespace

const char* format_text() {
	return "1.1";
}

const SectionInfo* section_info(uint16_t id) {
	size_t i = position(id);
	return i < kSectionCount ? &kSections[i] : nullptr;
}

const SectionInfo* sections(size_t& count) {
	count = kSectionCount;
	return kSections;
}

uint32_t image_crc(const uint8_t* bytes, size_t signed_end) {
	static const uint8_t kZero[4] = {0, 0, 0, 0};
	uint32_t crc = persist::crc32(0, bytes, 12);
	crc = persist::crc32(crc, kZero, 4);
	return signed_end > 16 ? persist::crc32(crc, bytes + 16, signed_end - 16) : crc;
}

uint64_t fnv1a(const void* data, size_t length, uint64_t basis) {
	const uint8_t* p = static_cast<const uint8_t*>(data);
	uint64_t hash = basis;
	for (size_t i = 0; i < length; ++i) {
		hash ^= p[i];
		hash *= 0x100000001b3ULL;
	}
	return hash;
}

uint64_t fnv1a(const char* text) {
	return fnv1a(text, std::strlen(text));
}


void View::open(const uint8_t* bytes) {
	*this = View();
	bytes_ = bytes;
	const Header& h = header();
	const SectionEntry* entries = reinterpret_cast<const SectionEntry*>(bytes + h.header_size);
	for (size_t k = 0; k < h.section_count; ++k) {
		const SectionEntry& e = entries[k];
		size_t i = position(e.id);
		if (i >= kSectionCount || i >= kKnown) {
			continue; // unknown: skipped (section 11)
		}
		Table& t = tables_[i];
		t.base = bytes + e.offset;
		if (kSections[i].size == 0) {
			t.stride = 1;
			t.count = e.length;
		} else {
			t.stride = e.entry_size;
			t.count = e.entry_size != 0 ? e.length / e.entry_size : 0;
		}
	}
	const Table& strings = tables_[position(kStrings)];
	if (strings.count != 0) {
		strings_ = reinterpret_cast<const char*>(strings.base);
	}
}

const Table& View::table(uint16_t id) const {
	size_t i = position(id);
	return i < kSectionCount ? tables_[i] : kEmpty;
}

const LabelEntry* View::label(uint32_t ref, size_t& count) const {
	count = label_count(ref);
	if (ref == 0 || count == 0) {
		count = 0;
		return nullptr;
	}
	const Table& t = table(kLabels);
	return reinterpret_cast<const LabelEntry*>(t.base + static_cast<size_t>(t.stride) * label_first(ref));
}

bool View::label_is_map(uint32_t ref) const {
	size_t count = 0;
	const LabelEntry* entries = label(ref, count);
	return count > 0 && entries[0].tag != 0;
}

const char* View::label_text(uint32_t ref, const char* language) const {
	size_t count = 0;
	const LabelEntry* first = label(ref, count);
	if (count == 0) {
		return "";
	}
	const Table& t = table(kLabels);
	auto entry = [&](size_t i) -> const LabelEntry& {
		return *reinterpret_cast<const LabelEntry*>(reinterpret_cast<const uint8_t*>(first) + t.stride * i);
	};
	if (entry(0).tag == 0) {
		return str(entry(0).text);
	}
	if (language != nullptr && language[0] != '\0') {
		size_t length = std::strlen(language);
		for (size_t i = 0; i < count; ++i) {
			if (tag_matches(str(entry(i).tag), language, length)) {
				return str(entry(i).text);
			}
		}
		const char* dash = std::strchr(language, '-');
		if (dash != nullptr) {
			size_t primary = static_cast<size_t>(dash - language);
			for (size_t i = 0; i < count; ++i) {
				if (tag_matches(str(entry(i).tag), language, primary)) {
					return str(entry(i).text);
				}
			}
		}
	}
	for (size_t i = 0; i < count; ++i) {
		if (tag_matches(str(entry(i).tag), "en", 2)) {
			return str(entry(i).text);
		}
	}
	return str(entry(0).text);
}

const char* View::ui(size_t& length) const {
	const Table& t = table(kUi);
	length = t.count;
	return t.count != 0 ? reinterpret_cast<const char*>(t.base) : nullptr;
}

const char* View::ui_anonymous(size_t& length) const {
	const Table& t = table(kUiAnonymous);
	length = t.count;
	return t.count != 0 ? reinterpret_cast<const char*>(t.base) : nullptr;
}

bool View::anonymous_datapoint(size_t id, bool& operate) const {
	operate = false;
	if (table(kUiAnonymous).count == 0) {
		return false;
	}
	const Table& t = table(kUiAnonymousDatapoints);
	size_t lo = 0, hi = t.count;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		const auto& r = *reinterpret_cast<const AnonymousDatapointRec*>(t.base + static_cast<size_t>(t.stride) * mid);
		if (r.datapoint == id) {
			operate = (r.flags & kAnonymousOperate) != 0;
			return true;
		}
		if (r.datapoint < id) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return false;
}

bool View::anonymous_function(size_t index) const {
	if (table(kUiAnonymous).count == 0) {
		return false;
	}
	const Table& t = table(kUiAnonymousFunctions);
	size_t lo = 0, hi = t.count;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		uint16_t f = *reinterpret_cast<const uint16_t*>(t.base + static_cast<size_t>(t.stride) * mid);
		if (f == index) {
			return true;
		}
		if (f < index) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return false;
}

void write_label(JsonWriter& json, const View& view, uint32_t ref) {
	size_t count = 0;
	const LabelEntry* first = view.label(ref, count);
	if (count == 0) {
		json.null();
		return;
	}
	const Table& t = view.table(kLabels);
	auto entry = [&](size_t i) -> const LabelEntry& {
		return *reinterpret_cast<const LabelEntry*>(reinterpret_cast<const uint8_t*>(first) + t.stride * i);
	};
	if (entry(0).tag == 0) {
		json.string(view.str(entry(0).text));
		return;
	}
	json.begin_object();
	for (size_t i = 0; i < count; ++i) {
		json.member(view.str(entry(i).tag), view.str(entry(i).text));
	}
	json.end_object();
}

} // namespace hc::image::bin
