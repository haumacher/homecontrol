/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_IMAGE_LOADER_H
#define HC_IMAGE_LOADER_H

#include <cstddef>
#include <cstdint>

#include "hc/astro.h"
#include "hc/blocks/block_set.h"
#include "hc/datapoint.h"
#include "hc/history.h"
#include "hc/image/image.h"
#include "hc/image/load_probe.h"
#include "hc/json.h"
#include "hc/knx/adapter.h"
#include "hc/knx/time_master.h"
#include "hc/platform/clock.h"
#include "hc/platform/settings.h"
#include "hc/rules/rule_set.h"
#include "hc/scheduler.h"
#include "hc/zigbee/cluster_mapping.h"

namespace hc {
namespace image {

/// A bump allocator for the tables of one load: blocks from the heap, freed together. Allocation
/// failure returns nullptr (the loader reports it as no_memory).
class Arena {
public:
	Arena() = default;
	~Arena() { release(); }
	Arena(const Arena&) = delete;
	Arena& operator=(const Arena&) = delete;

	/// `size` bytes aligned to 8, zeroed; nullptr when the heap is exhausted.
	void* allocate(size_t size);

	template <class T>
	T* array(size_t count) {
		return count == 0 ? nullptr : static_cast<T*>(allocate(count * sizeof(T)));
	}

	/// A NUL-terminated copy of `length` bytes.
	char* copy(const char* text, size_t length);

	/// Frees every block.
	void release();

	/// Bytes taken from the heap (blocks with their headers).
	size_t bytes() const { return bytes_; }
	/// Bytes handed out (with their alignment): bytes() less the headers and the unused tails.
	size_t used() const { return used_; }

	/// The block size: small tables share blocks of this size; an allocation larger than a quarter
	/// of it gets a block of exactly its size, so a small arena (the plan of a small image, the MQTT
	/// table) takes about 1 KB rather than 4, and a large table wastes no block tail (issue #44).
	static constexpr size_t kBlockSize = 1024;

private:
	struct Block {
		Block* next;
		size_t size;
		size_t used;
	};

	Block* head_ = nullptr;
	size_t bytes_ = 0;
	size_t used_ = 0;
};

/// Who owns a datapoint of the image.
enum class Binding : uint8_t {
	none,        ///< An application value (`virtual`, or no technology): rules and the UI command it.
	knx,         ///< The KNX adapter (its `knx` member).
	schedule,    ///< A schedule entry's output.
	time_master, ///< The KNX time master (the datapoints its `knx.timeMaster` implies).
	zigbee,      ///< The Zigbee adapter (its `zigbee` member, spec/zigbee.md), through the provider interface.
	rule,        ///< The rule engine: a datapoint mirroring a rule parameter (`rule/<ruleId>/<parameterId>`, spec/rules.md 8.6).
	block,       ///< The block runtime: a mirror `block/<id>/<x>` (spec/blocks.md 6).
};

/// A validated image, ready to apply: the specs the components take, in tables of the arena. The
/// strings point into the arena; `rules` spans the image text, which must outlive the plan.
struct Plan {
	ImageInfo info;

	bool has_tz = false;
	char tz[Settings::kMaxStringLength + 1] = {};
	bool has_location = false;
	GeoLocation location;
	bool has_ntp = false;
	char ntp[Settings::kMaxStringLength + 1] = {};

	DatapointSpec* datapoints = nullptr;
	Binding* bindings = nullptr;
	size_t datapoint_count = 0;

	knx::KnxMapping* mappings = nullptr;
	size_t mapping_count = 0;

	/// The `zigbee` bindings (spec/zigbee.md), validated; each `name` is its datapoint's id. Handed to
	/// the dynamic spec providers at apply (ImageBindings).
	zigbee::ZigbeeOverride* zigbee = nullptr;
	size_t zigbee_count = 0;

	bool has_time_master = false;
	knx::TimeMasterConfig time_master;

	ScheduleSpec* schedules = nullptr;
	size_t schedule_count = 0;

	HistorySpec* history = nullptr;
	size_t history_count = 0;
	/// `history.budget`: the RAM of the rings and the archives' open and pending slots.
	size_t history_budget = 0;
	/// `history.archiveBudget`: the flash of the archives' segments.
	size_t archive_budget = 0;

	bool has_rules = false;
	JsonSpan rules;
	/// The rules as the loader's check compiled them (against a scratch model with the image's
	/// datapoints, which have the same ids in the device's model): the image service hands this set
	/// to the engine instead of compiling the rules a second time (homecontrol #66). Empty once
	/// handed over; `rules_compiled` says whether it holds the image's rules.
	rules::RuleSet rule_set;
	bool rules_compiled = false;

	/// The `blocks` member (spec/blocks.md), compiled by the loader's check against the scratch model
	/// (whose datapoints have the ids of the device's model): the image service hands the set to the
	/// block runtime. `blocks_compiled` says whether it holds the image's blocks.
	bool has_blocks = false;
	blocks::BlockSet block_set;
	bool blocks_compiled = false;

	/// The `external` member (spec/external-values.md), handed to ExternalValues after the model swap.
	bool has_external = false;
	JsonSpan external;
	/// The `mqtt` member (spec/mqtt.md), checked here and handed to the MQTT client after the model swap.
	bool has_mqtt = false;
	JsonSpan mqtt;

	Arena arena;

	/// The schedule ids, for the rule loader's `schedule` triggers.
	bool has_schedule(const char* id) const;
	/// Whether `datapoint` is the `output` of a schedule entry: the scheduler owns it, a rule may
	/// not write it (spec/rules.md 10.1, notWritable).
	bool schedule_owns_output(const char* datapoint) const;
	/// Whether a technology that can fetch values owns `datapoint`: a KNX or Zigbee binding, or a
	/// binding of an `external` source. False for an application value, a schedule output, the time
	/// master's and the rule parameters' datapoints (a rule's `read` of them is refused at load,
	/// spec/rules.md 10.1 notReadable); true for a name that is not the image's.
	bool technology_reads(const char* datapoint) const;
};

/// Parses and validates a runtime image (spec/runtime-image.md) into a Plan, without changing
/// anything on the device: every member is checked (the rules against a scratch datapoint model
/// with the image's datapoints), every refusal recorded with its JSON path and reason. Allocates
/// the plan's tables in its arena and, while checking the rules, a scratch model that is freed
/// before it returns; the compiled rule set stays in the plan (Plan::rule_set).
class Loader {
public:
	/// `history_cap` is the device's history cap in bytes (HistoryLimits::cap_bytes); `language`
	/// the language the rules' labels are resolved in (the device's `ui.lang`, as the engine reads
	/// it), so that the compiled set is the one the engine would build.
	Loader(const Clock& clock, size_t history_cap, const char* language = "en")
		: clock_(clock), history_cap_(history_cap), language_(language) {}

	/// The device's cap of `history.archiveBudget` (HistoryLimits::archive_cap_bytes; default the C6's).
	void set_archive_cap(size_t bytes) { archive_cap_ = bytes; }
	/// The block types the image may use (default: the device's built-in library).
	void set_block_registry(const blocks::BlockRegistry& registry) { registry_ = &registry; }
	/// Told the end of each phase of parse() (nullptr: none; homecontrol #70).
	void set_probe(LoadProbe* probe) { probe_ = probe; }

	/// True when the image is valid; `plan` then holds it. Otherwise `report` names the refusals.
	bool parse(const char* text, size_t length, Plan& plan, Report& report) const;

private:
	const Clock& clock_;
	size_t history_cap_;
	size_t archive_cap_ = HistoryLimits().archive_cap_bytes;
	const char* language_;
	const blocks::BlockRegistry* registry_ = &blocks::BlockRegistry::device();
	LoadProbe* probe_ = nullptr;
};

/// The value type named in the image ("boolean", "unsigned", "signed", "real", "string",
/// "timeOfDay", "date", "dateTime", "enumeration", "raw", "controlled", "color"); none if unknown.
ValueType value_type_of(const char* name);

/// The image's name of a value type (the inverse of value_type_of()).
const char* value_type_label(ValueType type);

} // namespace image
} // namespace hc

#endif // HC_IMAGE_LOADER_H
