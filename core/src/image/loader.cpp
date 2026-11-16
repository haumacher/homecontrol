/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The plan of a binary runtime image (spec/runtime-image.md 9, 10.8; issue #87): the components'
// specs, built from the mapped image the validator accepted.
#include "hc/image/loader.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

#include "hc/dpt.h"
#include "hc/network_settings.h"
#include "hc/tz.h"

namespace hc {
namespace image {

using namespace bin;

// ---- Common ----------------------------------------------------------------------------------

const char* source_name(Source source) {
	switch (source) {
		case Source::boot: return "boot";
		case Source::api: return "api";
		case Source::console: return "console";
		case Source::file: return "file";
	}
	return "?";
}

const char* report_kind_name(Report::Kind kind) {
	switch (kind) {
		case Report::Kind::none: return "none";
		case Report::Kind::format: return "format";
		case Report::Kind::version: return "version";
		case Report::Kind::too_large: return "too large";
		case Report::Kind::unsigned_image: return "unsigned";
		case Report::Kind::damaged: return "damaged";
		case Report::Kind::bad_signature: return "bad signature";
		case Report::Kind::invalid: return "invalid";
		case Report::Kind::no_memory: return "no memory";
		case Report::Kind::apply: return "apply";
		case Report::Kind::busy: return "busy";
	}
	return "?";
}

const char* report_kind_code(Report::Kind kind) {
	switch (kind) {
		case Report::Kind::none: return "";
		case Report::Kind::format: return "image.format";
		case Report::Kind::version: return "image.version";
		case Report::Kind::too_large: return "image.too_large";
		case Report::Kind::unsigned_image: return "image.unsigned";
		case Report::Kind::damaged: return "image.damaged";
		case Report::Kind::bad_signature: return "image.bad_signature";
		case Report::Kind::invalid: return "image.invalid";
		case Report::Kind::no_memory: return "image.no_memory";
		case Report::Kind::apply: return "image.apply_failed";
		case Report::Kind::busy: return "image.busy";
	}
	return "image.invalid";
}

int report_kind_status(Report::Kind kind) {
	switch (kind) {
		case Report::Kind::none: return 200;
		case Report::Kind::format: return 400;
		case Report::Kind::version: return 422;
		case Report::Kind::too_large: return 413;
		case Report::Kind::unsigned_image: return 403;
		case Report::Kind::damaged: return 422;
		case Report::Kind::bad_signature: return 403;
		case Report::Kind::invalid: return 422;
		case Report::Kind::no_memory: return 507;
		case Report::Kind::apply: return 500;
		case Report::Kind::busy: return 503;
	}
	return 422;
}

bool report_kind_untouched(Report::Kind kind) {
	return kind == Report::Kind::format || kind == Report::Kind::version || kind == Report::Kind::too_large ||
		kind == Report::Kind::unsigned_image || kind == Report::Kind::busy;
}

void Report::add(Kind failure, const char* path, const char* format, ...) {
	if (kind == Kind::none) {
		kind = failure;
	}
	Refusal* slot = nullptr;
	if (count == 0) {
		slot = &first_;
	} else if (count < kMaxRefusals) {
		if (more_ == nullptr) {
			more_ = new (std::nothrow) Refusal[kMaxRefusals];
			if (more_ != nullptr) {
				more_[0] = first_;
				refusals = more_;
			}
		}
		if (more_ != nullptr) {
			slot = &more_[count];
		}
	}
	if (slot != nullptr) {
		++count;
		Refusal& r = *slot;
		std::snprintf(r.path, sizeof(r.path), "%s", path != nullptr ? path : "");
		va_list args;
		va_start(args, format);
		std::vsnprintf(r.reason, sizeof(r.reason), format, args);
		va_end(args);
	}
	++total;
}

// ---- Arena -----------------------------------------------------------------------------------

void* Arena::allocate(size_t size) {
	size = (size + 7) & ~static_cast<size_t>(7);
	if (size == 0) {
		size = 8;
	}
	constexpr size_t kHeader = (sizeof(Block) + 7) & ~static_cast<size_t>(7);
	Block* block = head_;
	if (block == nullptr || block->size - block->used < size) {
		bool own = size > kBlockSize / 4;
		size_t capacity = own ? size : kBlockSize;
		void* memory = ::operator new(kHeader + capacity, std::nothrow);
		if (memory == nullptr) {
			return nullptr;
		}
		block = static_cast<Block*>(memory);
		block->size = capacity;
		block->used = 0;
		if (own && head_ != nullptr) {
			block->next = head_->next;
			head_->next = block;
		} else {
			block->next = head_;
			head_ = block;
		}
		bytes_ += kHeader + capacity;
	}
	char* base = reinterpret_cast<char*>(block) + kHeader + block->used;
	block->used += size;
	used_ += size;
	std::memset(base, 0, size);
	return base;
}

char* Arena::copy(const char* text, size_t length) {
	char* out = static_cast<char*>(allocate(length + 1));
	if (out != nullptr) {
		std::memcpy(out, text, length);
		out[length] = '\0';
	}
	return out;
}

void Arena::release() {
	while (head_ != nullptr) {
		Block* next = head_->next;
		::operator delete(static_cast<void*>(head_));
		head_ = next;
	}
	bytes_ = 0;
	used_ = 0;
}

// ---- Value types, charts -----------------------------------------------------------------------

const char* value_type_label(ValueType type) {
	switch (type) {
		case ValueType::boolean: return "boolean";
		case ValueType::unsigned_int: return "unsigned";
		case ValueType::signed_int: return "signed";
		case ValueType::real: return "real";
		case ValueType::string: return "string";
		case ValueType::time_of_day: return "timeOfDay";
		case ValueType::date: return "date";
		case ValueType::date_time: return "dateTime";
		case ValueType::enumeration: return "enumeration";
		case ValueType::raw: return "raw";
		case ValueType::controlled: return "controlled";
		case ValueType::color: return "color";
		default: return "none";
	}
}

namespace {

constexpr const char* kRangeNames[kChartRanges] = {"1h", "24h", "7d", "30d", "1y"};
constexpr uint32_t kRangeSeconds[kChartRanges] = {3600, 86400, 7 * 86400, 30 * 86400, 365 * 86400};
constexpr const char* kConsolidationNames[] = {"mean", "last", "band", "consumption"};

} // namespace

const char* chart_range_name(size_t range) {
	return range < kChartRanges ? kRangeNames[range] : nullptr;
}

uint32_t chart_range_seconds(size_t range) {
	return range < kChartRanges ? kRangeSeconds[range] : 0;
}

int chart_range_of(const char* name) {
	for (size_t i = 0; i < kChartRanges; ++i) {
		if (std::strcmp(kRangeNames[i], name) == 0) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

const char* consolidation_name(Consolidation consolidation) {
	size_t i = static_cast<size_t>(consolidation);
	return i < sizeof(kConsolidationNames) / sizeof(kConsolidationNames[0]) ? kConsolidationNames[i] : "?";
}

bool consolidation_of(const char* name, Consolidation& out) {
	for (size_t i = 0; i < sizeof(kConsolidationNames) / sizeof(kConsolidationNames[0]); ++i) {
		if (std::strcmp(kConsolidationNames[i], name) == 0) {
			out = static_cast<Consolidation>(i);
			return true;
		}
	}
	return false;
}

// ---- The plan --------------------------------------------------------------------------------

namespace {

constexpr const char* kTag = "hc.image";

bool numeric(ValueType type) {
	return type == ValueType::unsigned_int || type == ValueType::signed_int || type == ValueType::real;
}

/// The time master's DPTs by member (spec/runtime-image.md 9.10).
constexpr uint16_t kTimeMasterDpt[3][2] = {{10, 1}, {11, 1}, {19, 1}};

struct Builder {
	const View& view;
	Plan& plan;
	Report& report;
	LoadProbe* probe;

	void refuse(const char* path, const char* format, ...) HC_PRINTF_FORMAT(3, 4) {
		char reason[Report::kMaxReason + 1];
		va_list args;
		va_start(args, format);
		std::vsnprintf(reason, sizeof(reason), format, args);
		va_end(args);
		report.add(Report::Kind::invalid, path, "%s", reason);
	}
	bool no_memory(const char* what) {
		report.add(Report::Kind::no_memory, "", "no memory for %s", what);
		return false;
	}
	void phase(const char* name) {
		if (probe != nullptr) {
			probe->load_phase(name);
		}
	}
	const DatapointRec& dp(size_t i) const { return view.at<DatapointRec>(kDatapoints, i); }
	const char* name(size_t i) const { return view.str(dp(i).name); }

	void info();
	void settings();
	bool datapoints();
	bool knx();
	bool zigbee();
	bool schedules();
	void history(size_t cap, size_t archive_cap);
	bool external();
};

void Builder::info() {
	ImageInfo& info = plan.info;
	const ImageRec& image = view.image();
	std::snprintf(info.project_id, sizeof(info.project_id), "%s", view.str(image.project_id));
	std::snprintf(info.schema_version, sizeof(info.schema_version), "%s", view.str(image.schema_version));
	std::snprintf(info.name, sizeof(info.name), "%s", view.str(image.name));
	std::snprintf(info.generator, sizeof(info.generator), "%s", view.str(image.generator));
	std::snprintf(info.generated_at, sizeof(info.generated_at), "%s", view.str(image.generated_at));
	info.revision = image.revision;
	info.format_major = view.header().major;
	info.format_minor = view.header().minor;
	info.bytes = view.length();
	info.crc = view.header().crc;
	info.is_signed = view.is_signed();
	info.datapoints = view.count(kDatapoints);
	info.knx_datapoints = 0;
	for (size_t i = 0; i < view.count(kDatapoints); ++i) {
		info.knx_datapoints += dp(i).owner == kOwnerKnx ? 1 : 0;
	}
	info.zigbee_datapoints = view.count(kZigbeeBindings);
	info.rules = view.count(kRules);
	info.schedules = view.count(kSchedules);
	info.blocks = view.count(kBlocks);
	info.locations = view.count(kLocations);
	info.functions = view.count(kFunctions);
	info.history_rings = view.count(kHistory);
	info.time_master = view.count(kKnxTimeMaster) > 0;
}

void Builder::settings() {
	if (view.count(kSettings) == 0) {
		return;
	}
	const SettingsRec& s = view.at<SettingsRec>(kSettings, 0);
	if (s.tz != 0) {
		const char* error = nullptr;
		if (!posix_tz_valid(view.str(s.tz), &error)) {
			refuse("settings[0].tz", "tz \"%s\" is not a POSIX TZ string: %s", view.str(s.tz),
				error != nullptr ? error : "invalid");
		} else {
			plan.tz = view.str(s.tz);
		}
	}
	if (s.ntp != 0) {
		const char* error = nullptr;
		if (!host_name_valid(view.str(s.ntp), &error)) {
			refuse("settings[0].ntp", "server \"%s\" is not a host name: %s", view.str(s.ntp),
				error != nullptr ? error : "invalid");
		} else {
			plan.ntp = view.str(s.ntp);
		}
	}
	if ((s.flags & 0x01) != 0) {
		plan.has_location = true;
		plan.location.latitude = s.lat;
		plan.location.longitude = s.lon;
		plan.location.elevation_m = (s.flags & 0x02) != 0 ? static_cast<double>(s.elev) : 0;
	}
}

bool Builder::datapoints() {
	size_t n = view.count(kDatapoints);
	plan.datapoint_count = n;
	plan.datapoints = plan.arena.array<DatapointSpec>(n);
	plan.bindings = plan.arena.array<Binding>(n);
	if (n > 0 && (plan.datapoints == nullptr || plan.bindings == nullptr)) {
		return no_memory("the datapoint table");
	}
	for (size_t i = 0; i < n; ++i) {
		const DatapointRec& r = dp(i);
		plan.datapoints[i] = DatapointSpec{view.str(r.name), static_cast<ValueType>(r.type),
			static_cast<uint8_t>(r.flags & ~kDatapointVirtual)};
		Binding b = Binding::none;
		switch (r.owner) {
			case kOwnerKnx: b = Binding::knx; break;
			case kOwnerScheduler: b = Binding::schedule; break;
			case kOwnerTimeMaster: b = Binding::time_master; break;
			case kOwnerZigbee: b = Binding::zigbee; break;
			case kOwnerRule: b = Binding::rule; break;
			case kOwnerBlock: b = Binding::block; break;
			default: b = Binding::none; break;
		}
		plan.bindings[i] = b;
	}
	// The model checks the names (unique, printable) itself; a duplicate refuses here with the path.
	DatapointStatus status = DatapointModel::validate(plan.datapoints, n);
	if (status != DatapointStatus::ok) {
		refuse("datapoints", "the datapoint model refuses the image's datapoints: %s", datapoint_status_name(status));
		return false;
	}
	return true;
}

bool Builder::knx() {
	size_t bindings = view.count(kKnxBindings);
	size_t mappings = 0;
	size_t listens = 0;
	for (size_t b = 0; b < bindings; ++b) {
		const KnxBindingRec& r = view.at<KnxBindingRec>(kKnxBindings, b);
		if (dp(r.datapoint).owner == kOwnerKnx) {
			++mappings;
			listens += r.listen_count;
		}
	}
	plan.mappings = plan.arena.array<knx::KnxMapping>(mappings);
	knx::GroupAddress* pool = plan.arena.array<knx::GroupAddress>(listens);
	if ((mappings > 0 && plan.mappings == nullptr) || (listens > 0 && pool == nullptr)) {
		return no_memory("the KNX mapping");
	}
	auto address = [this](uint16_t index) {
		return knx::GroupAddress{view.at<KnxAddressRec>(kKnxAddresses, index).address};
	};
	char path[40];
	char text[Dpt::kMaxText + 1];
	for (size_t b = 0; b < bindings; ++b) {
		const KnxBindingRec& r = view.at<KnxBindingRec>(kKnxBindings, b);
		const DatapointRec& d = dp(r.datapoint);
		Dpt dpt;
		dpt.main = r.dpt_main;
		dpt.sub = r.dpt_sub;
		dpt.format(text, sizeof(text));
		std::snprintf(path, sizeof(path), "knx.bindings[%u].dptMain", static_cast<unsigned>(b));
		if (d.owner == kOwnerTimeMaster) {
			uint16_t member = d.owner_member;
			if (member > 2 || dpt.main != kTimeMasterDpt[member][0] || dpt.sub != kTimeMasterDpt[member][1]) {
				refuse(path, "unsupported: DPT %s for the time master's %s, which sends %u.%03u", text,
					member == 0 ? "time" : member == 1 ? "date" : "dateTime",
					static_cast<unsigned>(kTimeMasterDpt[member > 2 ? 0 : member][0]),
					static_cast<unsigned>(kTimeMasterDpt[member > 2 ? 0 : member][1]));
			}
			continue;
		}
		if (!dpt.supported()) {
			refuse(path, "unsupported: DPT %s is not supported by this firmware", text);
			continue;
		}
		ValueType type = dpt.value_type();
		ValueType want = static_cast<ValueType>(d.type);
		if (type != want && !(numeric(type) && numeric(want))) {
			refuse(path, "DPT %s carries %s values, the datapoint is %s", text, value_type_label(type),
				value_type_label(want));
			continue;
		}
		knx::KnxMapping& m = plan.mappings[plan.mapping_count++];
		m.datapoint = r.datapoint;
		m.dpt = dpt;
		m.has_send = r.send != kNone;
		if (m.has_send) {
			m.send = address(r.send);
		}
		m.listen = pool;
		m.listen_count = r.listen_count;
		for (size_t k = 0; k < r.listen_count; ++k) {
			*pool++ = address(view.at<uint16_t>(kKnxListen, r.first_listen + k));
		}
	}
	if (view.count(kKnxTimeMaster) > 0) {
		const TimeMasterRec& tm = view.at<TimeMasterRec>(kKnxTimeMaster, 0);
		knx::TimeMasterConfig& config = plan.time_master;
		auto output = [&](uint16_t datapoint, knx::TimeMasterConfig::Output& out) {
			if (datapoint == kNone) {
				return;
			}
			const DatapointRec& d = dp(datapoint);
			const KnxBindingRec& r = view.at<KnxBindingRec>(kKnxBindings, d.owner_index);
			out.enabled = r.send != kNone;
			if (out.enabled) {
				out.address = address(r.send);
			}
			out.datapoint = view.str(d.name);
		};
		output(tm.time, config.time);
		output(tm.date, config.date);
		output(tm.date_time, config.date_time);
		config.interval_s = tm.interval;
		config.at_midnight = (tm.flags & 0x01) != 0;
		config.on_synchronized = (tm.flags & 0x02) != 0;
		plan.has_time_master = true;
	}
	return true;
}

bool Builder::zigbee() {
	size_t n = view.count(kZigbeeBindings);
	plan.zigbee = plan.arena.array<zigbee::ZigbeeOverride>(n);
	if (n > 0 && plan.zigbee == nullptr) {
		return no_memory("the Zigbee bindings");
	}
	char path[48];
	for (size_t z = 0; z < n; ++z) {
		const ZigbeeBindingRec& r = view.at<ZigbeeBindingRec>(kZigbeeBindings, z);
		const DatapointRec& d = dp(r.datapoint);
		zigbee::ZigbeeOverride binding;
		binding.name = view.str(d.name);
		binding.ieee = r.ieee;
		binding.endpoint = r.endpoint;
		binding.cluster = r.cluster;
		binding.attribute = r.attribute;
		binding.client = (r.flags & 0x01) != 0;
		binding.command_cluster = r.command == 0xFFFF ? zigbee::cluster::kNone : r.command;
		const char* why = nullptr;
		std::optional<zigbee::ZigbeePoint> point = zigbee::override_point(binding, &why);
		std::snprintf(path, sizeof(path), "zigbee.bindings[%u].cluster", static_cast<unsigned>(z));
		if (!point) {
			if (binding.client) {
				refuse(path, "client cluster %u: %s", static_cast<unsigned>(binding.cluster), why);
			} else {
				refuse(path, "cluster %u attribute %u: %s", static_cast<unsigned>(binding.cluster),
					static_cast<unsigned>(binding.attribute), why);
			}
			continue;
		}
		const zigbee::FunctionInfo& info = zigbee::function_info(point->function);
		ValueType type = static_cast<ValueType>(d.type);
		if (info.value_type != type && !(numeric(info.value_type) && numeric(type)) &&
			!(info.value_type == ValueType::enumeration && numeric(type)) &&
			!(numeric(info.value_type) && type == ValueType::enumeration)) {
			refuse(path, "the function %s carries %s values, the datapoint is %s", info.key,
				value_type_label(info.value_type), value_type_label(type));
			continue;
		}
		if ((d.flags & DatapointFlags::kTransmit) != 0 && !point->commandable()) {
			std::snprintf(path, sizeof(path), "zigbee.bindings[%u].command", static_cast<unsigned>(z));
			refuse(path, "a transmitting Zigbee datapoint needs the cluster its commands go to (command); %s",
				binding.client ? "a client binding (button commands) cannot be commanded"
							   : "without it the binding is read-only");
			continue;
		}
		if ((r.flags & 0x0E) != 0) {
			zigbee::ReportingOverride reporting;
			if ((r.flags & 0x02) != 0) {
				reporting.set |= zigbee::ReportingOverride::kMin;
				reporting.min_s = r.report_min;
			}
			if ((r.flags & 0x04) != 0) {
				reporting.set |= zigbee::ReportingOverride::kMax;
				reporting.max_s = r.report_max;
			}
			if ((r.flags & 0x08) != 0) {
				reporting.set |= zigbee::ReportingOverride::kChange;
				reporting.change = r.report_change;
			}
			zigbee::Reporting effective;
			zigbee::ReportingMember which = zigbee::ReportingMember::whole;
			if (!zigbee::resolve_reporting(point->function, reporting, effective, &which, &why)) {
				const char* field = which == zigbee::ReportingMember::max ? "reportMax"
					: which == zigbee::ReportingMember::change			 ? "reportChange"
																		 : "reportMin";
				std::snprintf(path, sizeof(path), "zigbee.bindings[%u].%s", static_cast<unsigned>(z), field);
				refuse(path, "%s", why != nullptr ? why : "?");
				continue;
			}
			binding.reporting = reporting;
		}
		plan.zigbee[plan.zigbee_count++] = binding;
	}
	return true;
}

bool Builder::schedules() {
	size_t n = view.count(kSchedules);
	size_t dates = view.count(kScheduleDates);
	size_t bounds = view.count(kScheduleBounds);
	plan.schedules = plan.arena.array<ScheduleSpec>(n);
	Date* date_pool = plan.arena.array<Date>(dates);
	ScheduleBound* bound_pool = plan.arena.array<ScheduleBound>(bounds);
	if ((n > 0 && plan.schedules == nullptr) || (dates > 0 && date_pool == nullptr) ||
		(bounds > 0 && bound_pool == nullptr)) {
		return no_memory("the schedules");
	}
	for (size_t i = 0; i < dates; ++i) {
		const ScheduleDateRec& d = view.at<ScheduleDateRec>(kScheduleDates, i);
		date_pool[i].year = d.year;
		date_pool[i].month = d.month;
		date_pool[i].day = d.day;
	}
	for (size_t i = 0; i < bounds; ++i) {
		const ScheduleBoundRec& b = view.at<ScheduleBoundRec>(kScheduleBounds, i);
		bound_pool[i].member = b.member;
		bound_pool[i].min = b.min;
		bound_pool[i].max = b.max;
		bound_pool[i].step = b.step;
	}
	auto clock = [](uint32_t seconds) {
		LocalClockTime t;
		t.hour = static_cast<uint8_t>(seconds / 3600);
		t.minute = static_cast<uint8_t>(seconds % 3600 / 60);
		t.second = static_cast<uint8_t>(seconds % 60);
		return t;
	};
	constexpr SunEvent kSun[] = {SunEvent::dawn, SunEvent::sunrise, SunEvent::sunset, SunEvent::dusk};
	for (size_t i = 0; i < n; ++i) {
		const ScheduleRec& r = view.at<ScheduleRec>(kSchedules, i);
		ScheduleSpec& s = plan.schedules[i];
		s.id = view.str(r.id);
		s.label = r.label != 0 ? view.str(r.label) : s.id;
		s.kind = static_cast<ScheduleKind>(r.kind);
		s.enabled = (r.flags & 0x01) != 0;
		s.enabled_adjustable = (r.flags & 0x02) != 0;
		s.missed = (r.flags & 0x04) != 0 ? MissedPolicy::catch_up : MissedPolicy::skip;
		s.output = r.output != kNone ? name(r.output) : nullptr;
		s.days = r.days;
		s.time = clock(r.time);
		s.month = r.month;
		s.day = r.day;
		s.dates = r.date_count > 0 ? date_pool + r.first_date : nullptr;
		s.date_count = r.date_count;
		s.sun = kSun[r.sun <= 3 ? r.sun : 1];
		s.offset_s = r.offset;
		s.has_not_before = (r.flags & 0x08) != 0;
		s.not_before = clock(r.not_before);
		s.has_not_after = (r.flags & 0x10) != 0;
		s.not_after = clock(r.not_after);
		s.every_minutes = r.every;
		s.from = clock(r.from);
		s.until = clock(r.until);
		s.at_date.year = r.at_year;
		s.at_date.month = r.at_month;
		s.at_date.day = r.at_day;
		s.at_time = clock(r.at_time);
		s.adjustable = r.adjustable;
		s.bounds = r.bound_count > 0 ? bound_pool + r.first_bound : nullptr;
		s.bound_count = r.bound_count;
	}
	plan.schedule_count = n;
	return true;
}

void Builder::history(size_t cap, size_t archive_cap) {
	size_t n = view.count(kHistory);
	size_t archives = view.count(kArchives);
	plan.history = plan.arena.array<HistorySpec>(n);
	ArchiveSpec* pool = plan.arena.array<ArchiveSpec>(archives);
	if ((n > 0 && plan.history == nullptr) || (archives > 0 && pool == nullptr)) {
		no_memory("the history");
		return;
	}
	for (size_t i = 0; i < archives; ++i) {
		const ArchiveRec& a = view.at<ArchiveRec>(kArchives, i);
		pool[i] = ArchiveSpec{a.resolution, a.retention};
	}
	size_t needed = 0;
	size_t flash = 0;
	for (size_t i = 0; i < n; ++i) {
		const HistoryRec& r = view.at<HistoryRec>(kHistory, i);
		HistorySpec& h = plan.history[i];
		h.datapoint = r.datapoint;
		h.retention_s = r.retention;
		h.spacing_s = r.spacing;
		h.archives = r.archive_count > 0 ? pool + r.first_archive : nullptr;
		h.archive_count = r.archive_count;
		needed += History::ram_bytes(h);
		flash += History::flash_bytes(h);
	}
	plan.history_count = n;
	const ImageRec& image = view.image();
	plan.history_budget = image.history_budget;
	plan.archive_budget = image.archive_budget;
	if (needed > plan.history_budget) {
		refuse("image[0].historyBudget", "the history needs %u bytes, more than the budget of %u",
			static_cast<unsigned>(needed), static_cast<unsigned>(plan.history_budget));
	} else if (plan.history_budget > cap) {
		refuse("image[0].historyBudget", "the image asks for %u bytes of history RAM, the device allows at most %u",
			static_cast<unsigned>(plan.history_budget), static_cast<unsigned>(cap));
	}
	if (flash > plan.archive_budget) {
		refuse("image[0].archiveBudget", "the archives need %u bytes, more than the budget of %u",
			static_cast<unsigned>(flash), static_cast<unsigned>(plan.archive_budget));
	} else if (plan.archive_budget > archive_cap) {
		refuse("image[0].archiveBudget", "the image asks for %u bytes of archive flash, the device allows at most %u",
			static_cast<unsigned>(plan.archive_budget), static_cast<unsigned>(archive_cap));
	}
}

bool Builder::external() {
	size_t n = view.count(kExternalSources);
	size_t bindings = view.count(kExternalBindings);
	plan.external = plan.arena.array<ExternalSourceSpec>(n);
	ExternalBindingSpec* pool = plan.arena.array<ExternalBindingSpec>(bindings);
	if ((n > 0 && plan.external == nullptr) || (bindings > 0 && pool == nullptr)) {
		return no_memory("the external sources");
	}
	size_t variable_count = 0;
	const ExternalPresetVariable* variables = open_meteo_variables(variable_count);
	char path[48];
	for (size_t i = 0; i < bindings; ++i) {
		const ExternalBindingRec& r = view.at<ExternalBindingRec>(kExternalBindings, i);
		ExternalBindingSpec& b = pool[i];
		b.datapoint = name(r.datapoint);
		b.path = view.opt(r.path);
		b.variable = view.opt(r.variable);
		b.unit = view.opt(r.unit);
		b.scale = r.scale;
		b.offset = r.offset;
	}
	for (size_t i = 0; i < n; ++i) {
		const ExternalSourceRec& r = view.at<ExternalSourceRec>(kExternalSources, i);
		ExternalSourceSpec& s = plan.external[i];
		s.id = view.str(r.id);
		s.url = view.opt(r.url);
		s.preset = view.opt(r.preset);
		s.interval_s = r.interval;
		s.bindings = pool + r.first_binding;
		s.binding_count = r.binding_count;
		if (s.preset != nullptr) {
			if (std::strcmp(s.preset, "open-meteo") != 0) {
				std::snprintf(path, sizeof(path), "external.sources[%u].preset", static_cast<unsigned>(i));
				refuse(path, "unsupported: unknown preset '%s'", s.preset);
				continue;
			}
			for (size_t k = 0; k < r.binding_count; ++k) {
				const char* variable = s.bindings[k].variable;
				bool known = false;
				for (size_t v = 0; v < variable_count && variable != nullptr && !known; ++v) {
					known = std::strcmp(variables[v].name, variable) == 0;
				}
				if (!known) {
					std::snprintf(path, sizeof(path), "external.bindings[%u].variable",
						static_cast<unsigned>(r.first_binding + k));
					refuse(path, "unsupported: %s has no variable '%s'", s.preset, variable != nullptr ? variable : "");
				}
			}
		}
	}
	plan.external_count = n;
	return true;
}

} // namespace

bool Loader::build(const View& view, Plan& plan, Report& report) const {
	(void)clock_;
	plan.view = view;
	Builder b{view, plan, report, probe_};
	b.info();
	b.settings();
	if (!b.datapoints()) {
		return false;
	}
	b.phase("datapoints");
	if (!b.knx() || !b.zigbee() || !b.schedules()) {
		return false;
	}
	b.phase("schedules");
	b.history(history_cap_, archive_cap_);
	b.phase("history");
	if (!b.external()) {
		return false;
	}
	plan.has_mqtt = view.count(kMqtt) > 0;
	if (!report.ok()) {
		return false;
	}
	if (tables_only_) {
		return true;
	}
	rules::LoadResult rules = plan.rule_set.bind(view, language_);
	if (!rules.ok()) {
		if (rules.error == rules::LoadError::out_of_memory) {
			report.add(Report::Kind::no_memory, rules.path, "%s", rules.text);
		} else {
			b.refuse(rules.path, "%s: %s", rules::load_error_name(rules.error), rules.text);
		}
		return false;
	}
	b.phase("rules");
	blocks::BlockLoadResult blocks = plan.block_set.bind(view, *registry_);
	if (!blocks.ok()) {
		if (blocks.error == blocks::BlockError::out_of_memory) {
			report.add(Report::Kind::no_memory, blocks.path, "%s", blocks.text);
		} else {
			b.refuse(blocks.path, "%s: %s", blocks::block_error_name(blocks.error), blocks.text);
		}
		return false;
	}
	b.phase("blocks");
	if (!report.ok()) {
		Log::warn(kTag, "image refused: %u problem%s, first at %s: %s", static_cast<unsigned>(report.total),
			report.total == 1 ? "" : "s", report.count > 0 ? report.refusals[0].path : "",
			report.count > 0 ? report.refusals[0].reason : "");
		return false;
	}
	return true;
}

} // namespace image
} // namespace hc
