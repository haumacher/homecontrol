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
#include "hc/external_values.h"
#include "hc/history.h"
#include "hc/image/binary.h"
#include "hc/image/image.h"
#include "hc/image/load_probe.h"
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
	/// of it gets a block of exactly its size.
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

/// Who owns a datapoint of the image (spec/runtime-image.md 9.7 `owner`).
enum class Binding : uint8_t {
	none,        ///< An application value (`virtual`, or no technology), or an external source's.
	knx,         ///< The KNX adapter.
	schedule,    ///< A schedule entry's output or time datapoint.
	time_master, ///< The KNX time master.
	zigbee,      ///< The Zigbee adapter, through the provider interface.
	rule,        ///< The rule engine: a parameter or state datapoint.
	block,       ///< The block runtime: a mirror `block/<id>/<x>`.
};

/// A validated image, ready to apply: the specs the components take, built from the mapped image
/// (`view`, which must stay mapped while the plan lives). The strings point into the image; the
/// tables are in the arena. Built once per load and kept while the image runs (the providers'
/// reloads bind the components again from it).
struct Plan {
	ImageInfo info;
	bin::View view;

	const char* tz = nullptr;  ///< nullptr: the device keeps its own.
	bool has_location = false;
	GeoLocation location;
	const char* ntp = nullptr; ///< nullptr: the device keeps its own.

	DatapointSpec* datapoints = nullptr;
	Binding* bindings = nullptr;
	size_t datapoint_count = 0;

	knx::KnxMapping* mappings = nullptr;
	size_t mapping_count = 0;

	/// The Zigbee bindings (spec/zigbee.md); each `name` is its datapoint's id. Handed to the dynamic
	/// spec providers at apply (ImageBindings).
	zigbee::ZigbeeOverride* zigbee = nullptr;
	size_t zigbee_count = 0;

	bool has_time_master = false;
	knx::TimeMasterConfig time_master;

	ScheduleSpec* schedules = nullptr;
	size_t schedule_count = 0;

	HistorySpec* history = nullptr;
	size_t history_count = 0;
	/// The RAM of the rings and the archives' open slots, and the flash of the archives.
	size_t history_budget = 0;
	size_t archive_budget = 0;

	/// The rules, read in place (rules::RuleSet::bind()); handed to the engine at apply.
	rules::RuleSet rule_set;
	/// The blocks, checked against this firmware's types (spec/runtime-image.md 10.8).
	blocks::BlockSet block_set;

	ExternalSourceSpec* external = nullptr;
	size_t external_count = 0;

	bool has_mqtt = false;

	Arena arena;
};

/// Builds the Plan of a structurally valid image (validator.h) and makes the checks of
/// spec/runtime-image.md 10.8 that need this firmware's catalogues: the DPTs, the Zigbee function
/// table, the presets of the external values, the block types, the history caps, the settings'
/// validators, the persistence keys. Every refusal is recorded with its path and reason.
class Loader {
public:
	/// `history_cap` is the device's history cap in bytes (HistoryLimits::cap_bytes); `language` the
	/// language the rules' labels are resolved in (the device's `ui.lang`).
	Loader(const Clock& clock, size_t history_cap, const char* language = "en")
		: clock_(clock), history_cap_(history_cap), language_(language) {}

	/// The device's cap of the archives' flash (HistoryLimits::archive_cap_bytes; default the C6's).
	void set_archive_cap(size_t bytes) { archive_cap_ = bytes; }
	/// The block types the image may use (default: the device's built-in library).
	void set_block_registry(const blocks::BlockRegistry& registry) { registry_ = &registry; }
	/// Only the components' tables: no rules and no blocks are bound (a reload for the providers).
	void set_tables_only(bool only) { tables_only_ = only; }
	/// Told the end of each phase of build() (nullptr: none).
	void set_probe(LoadProbe* probe) { probe_ = probe; }

	/// True when the image is accepted; `plan` then holds it. Otherwise `report` names the refusals.
	bool build(const bin::View& view, Plan& plan, Report& report) const;

private:
	const Clock& clock_;
	size_t history_cap_;
	size_t archive_cap_ = HistoryLimits().archive_cap_bytes;
	const char* language_;
	const blocks::BlockRegistry* registry_ = &blocks::BlockRegistry::device();
	LoadProbe* probe_ = nullptr;
	bool tables_only_ = false;
};

/// The image's name of a value type ("boolean", "unsigned", ..., "color"); "none" for none.
const char* value_type_label(ValueType type);

} // namespace image
} // namespace hc

#endif // HC_IMAGE_LOADER_H
