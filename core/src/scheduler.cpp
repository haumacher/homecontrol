/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/scheduler.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <new>

#include "hc/log.h"
#include "hc/tz.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.sched";

constexpr int64_t kNone = INT64_MIN;

/// Version of the persisted record format (the first field of the value).
constexpr char kRecordVersion = '1';

constexpr const char* kRecordPrefix = "sch.";

/// Longest astro offset.
constexpr int32_t kMaxOffsetS = 12 * 3600;

/// Most steps when walking over missed firings.
constexpr int kMaxWalk = 2000;

/// Length of a formatted local time.
constexpr size_t kTimeText = 64;

bool id_valid(const char* id) {
	if (id == nullptr) {
		return false;
	}
	size_t length = 0;
	for (const char* p = id; *p != '\0'; ++p, ++length) {
		char c = *p;
		bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
		if (!ok) {
			return false;
		}
	}
	return length >= 1 && length <= Scheduler::kMaxId;
}

bool label_valid(const char* label) {
	if (label == nullptr) {
		return false;
	}
	size_t length = std::strlen(label);
	if (length < 1 || length > Scheduler::kMaxLabel) {
		return false;
	}
	for (const char* p = label; *p != '\0'; ++p) {
		if (static_cast<unsigned char>(*p) < 0x20 || *p == 0x7F) {
			return false;
		}
	}
	return true;
}

bool time_valid(const LocalClockTime& t) {
	return t.hour <= 23 && t.minute <= 59 && t.second <= 59;
}

uint32_t seconds_of(const LocalClockTime& t) {
	return t.hour * 3600u + t.minute * 60u + t.second;
}

bool date_valid(const Date& d) {
	return d.year >= 1970 && d.year <= 2155 && d.day >= 1 && d.day <= days_in_month(d.year, d.month);
}

int64_t days_of(const Date& d) {
	return days_from_civil(d.year, d.month, d.day);
}

bool date_less(const Date& a, const Date& b) {
	return days_of(a) < days_of(b);
}

/// Parses a signed decimal int64 from `p` up to `;` (empty: kNone). Advances past the `;`.
bool parse_field(const char*& p, int64_t& out) {
	if (*p == ';') {
		out = kNone;
		++p;
		return true;
	}
	bool negative = false;
	if (*p == '-') {
		negative = true;
		++p;
	}
	int64_t value = 0;
	int digits = 0;
	while (*p >= '0' && *p <= '9') {
		if (++digits > 18) {
			return false;
		}
		value = value * 10 + (*p - '0');
		++p;
	}
	if (digits == 0 || *p != ';') {
		return false;
	}
	++p;
	out = negative ? -value : value;
	return true;
}

} // namespace

const char* schedule_kind_name(ScheduleKind kind) {
	switch (kind) {
	case ScheduleKind::weekly: return "weekly";
	case ScheduleKind::yearly: return "yearly";
	case ScheduleKind::dates: return "dates";
	case ScheduleKind::astro: return "astro";
	case ScheduleKind::interval: return "interval";
	case ScheduleKind::once: return "once";
	}
	return "?";
}

const char* missed_policy_name(MissedPolicy policy) {
	return policy == MissedPolicy::catch_up ? "catchUp" : "skip";
}

const char* schedule_status_name(ScheduleStatus status) {
	switch (status) {
	case ScheduleStatus::ok: return "ok";
	case ScheduleStatus::malformed: return "malformed";
	case ScheduleStatus::duplicate_id: return "duplicateId";
	case ScheduleStatus::unknown_reference: return "unknownReference";
	case ScheduleStatus::type_error: return "typeError";
	case ScheduleStatus::output_conflict: return "outputConflict";
	case ScheduleStatus::limit_exceeded: return "limitExceeded";
	case ScheduleStatus::out_of_memory: return "outOfMemory";
	case ScheduleStatus::busy: return "busy";
	}
	return "?";
}

struct Scheduler::Entry {
	ScheduleSpec spec;             ///< Pointers redirected into the scheduler's block.
	char key[kRecordKeySize] = {}; ///< Settings key of the record.
	int64_t last_fired = kNone;    ///< UTC of the last firing.
	int64_t accounted = kNone;     ///< The latest occurrence fired or skipped (UTC).
	int64_t next = kNone;          ///< The next due occurrence (UTC), kNone if none.
	uint16_t output = 0;
	bool has_output = false;
	bool enabled = true;
	bool persistable = true;
	bool astro_refusal_logged = false;
	bool stored_idle = false; ///< The stored record says the entry was idle.
};

void Scheduler::record_key(const char* id, char* buffer) {
	uint32_t hash = 2166136261u; // FNV-1a, 32 bit
	for (const char* p = id; *p != '\0'; ++p) {
		hash ^= static_cast<uint8_t>(*p);
		hash *= 16777619u;
	}
	std::snprintf(buffer, kRecordKeySize, "%s%08" PRIx32, kRecordPrefix, hash);
}

Scheduler::Scheduler(EventLoop& loop, const TimeSource& time, Settings& settings, DatapointModel* model)
	: loop_(loop), time_(time), settings_(settings), model_(model) {}

Scheduler::~Scheduler() {
	if (timer_.valid()) {
		loop_.cancel(timer_);
	}
	delete[] entries_;
	delete[] block_;
}

ScheduleStatus Scheduler::validate(const ScheduleSpec& spec, size_t index, size_t& dates) const {
	const char* id = spec.id != nullptr ? spec.id : "?";
	auto refuse = [&](const char* what, ScheduleStatus status = ScheduleStatus::malformed) {
		Log::error(kTag, "refusing schedules: entry %u (%s): %s", static_cast<unsigned>(index), id, what);
		return status;
	};
	if (!id_valid(spec.id)) {
		return refuse("id must be 1..32 characters of A-Z a-z 0-9 _ -");
	}
	if (!label_valid(spec.label)) {
		return refuse("label must be 1..63 printable bytes");
	}
	if (spec.missed != MissedPolicy::skip && spec.missed != MissedPolicy::catch_up) {
		return refuse("unknown missed policy");
	}
	bool uses_days = spec.kind == ScheduleKind::weekly || spec.kind == ScheduleKind::astro ||
		spec.kind == ScheduleKind::interval;
	if (uses_days && (spec.days == 0 || (spec.days & ~Weekdays::kAll) != 0)) {
		return refuse("days must name 1..7 days of the week");
	}
	switch (spec.kind) {
	case ScheduleKind::weekly:
		if (!time_valid(spec.time)) {
			return refuse("time out of range");
		}
		break;
	case ScheduleKind::yearly:
		if (!time_valid(spec.time)) {
			return refuse("time out of range");
		}
		if (spec.month < 1 || spec.month > 12 || spec.day < 1 || spec.day > days_in_month(2024, spec.month)) {
			return refuse("month/day is not a date");
		}
		break;
	case ScheduleKind::dates:
		if (!time_valid(spec.time)) {
			return refuse("time out of range");
		}
		if (spec.dates == nullptr || spec.date_count == 0) {
			return refuse("dates must list at least one date");
		}
		if (spec.date_count > kMaxDates) {
			return refuse("more dates than the limit of 128", ScheduleStatus::limit_exceeded);
		}
		for (size_t i = 0; i < spec.date_count; ++i) {
			if (!date_valid(spec.dates[i])) {
				return refuse("a date is not a valid date in 1970..2155");
			}
			for (size_t j = 0; j < i; ++j) {
				if (spec.dates[j] == spec.dates[i]) {
					return refuse("a date is listed twice", ScheduleStatus::duplicate_id);
				}
			}
		}
		dates += spec.date_count;
		break;
	case ScheduleKind::astro:
		if (spec.sun != SunEvent::dawn && spec.sun != SunEvent::sunrise && spec.sun != SunEvent::sunset &&
			spec.sun != SunEvent::dusk) {
			return refuse("unknown sun event");
		}
		if (spec.offset_s < -kMaxOffsetS || spec.offset_s > kMaxOffsetS) {
			return refuse("offset beyond 12 h");
		}
		if ((spec.has_not_before && !time_valid(spec.not_before)) ||
			(spec.has_not_after && !time_valid(spec.not_after))) {
			return refuse("clamp time out of range");
		}
		if (spec.has_not_before && spec.has_not_after && seconds_of(spec.not_before) > seconds_of(spec.not_after)) {
			return refuse("notBefore is later than notAfter");
		}
		break;
	case ScheduleKind::interval:
		if (spec.every_minutes < 1 || spec.every_minutes > 720) {
			return refuse("every must be 1 to 720 minutes");
		}
		if (!time_valid(spec.from) || !time_valid(spec.until)) {
			return refuse("from/until out of range");
		}
		if (seconds_of(spec.until) < seconds_of(spec.from)) {
			return refuse("until is earlier than from (a window across midnight needs two entries)");
		}
		break;
	case ScheduleKind::once:
		if (!date_valid(spec.at_date) || !time_valid(spec.at_time)) {
			return refuse("at is not a valid date and time");
		}
		break;
	default:
		return refuse("unknown kind");
	}
	return ScheduleStatus::ok;
}

ScheduleStatus Scheduler::load(const ScheduleSpec* specs, size_t count) {
	if (notifying_) {
		Log::error(kTag, "refusing schedules: load from within a firing");
		return ScheduleStatus::busy;
	}
	if (count > kMaxEntries) {
		Log::error(kTag, "refusing schedules: %u entries exceed the limit of %u", static_cast<unsigned>(count),
			static_cast<unsigned>(kMaxEntries));
		return ScheduleStatus::limit_exceeded;
	}
	if (count > 0 && specs == nullptr) {
		Log::error(kTag, "refusing schedules: no entries given");
		return ScheduleStatus::malformed;
	}
	size_t total_dates = 0;
	size_t string_bytes = 0;
	for (size_t i = 0; i < count; ++i) {
		const ScheduleSpec& spec = specs[i];
		ScheduleStatus status = validate(spec, i, total_dates);
		if (status != ScheduleStatus::ok) {
			return status;
		}
		for (size_t j = 0; j < i; ++j) {
			if (std::strcmp(specs[j].id, spec.id) == 0) {
				Log::error(kTag, "refusing schedules: entry %u: id %s used twice", static_cast<unsigned>(i), spec.id);
				return ScheduleStatus::duplicate_id;
			}
		}
		string_bytes += std::strlen(spec.id) + 1 + std::strlen(spec.label) + 1;
		if (spec.output != nullptr) {
			if (model_ == nullptr) {
				Log::error(kTag, "refusing schedules: entry %s: output %s, but the scheduler has no datapoint model",
					spec.id, spec.output);
				return ScheduleStatus::unknown_reference;
			}
			const Datapoint* dp = model_->find(spec.output);
			if (dp == nullptr) {
				Log::error(kTag, "refusing schedules: entry %s: output datapoint '%s' does not exist", spec.id,
					spec.output);
				return ScheduleStatus::unknown_reference;
			}
			if (dp->type() != ValueType::boolean) {
				Log::error(kTag, "refusing schedules: entry %s: output datapoint '%s' is %s, not boolean", spec.id,
					spec.output, value_type_name(dp->type()));
				return ScheduleStatus::type_error;
			}
			if (dp->technology() != nullptr && dp->technology() != this) {
				Log::error(kTag, "refusing schedules: entry %s: output datapoint '%s' is owned by %s", spec.id,
					spec.output, dp->technology()->name());
				return ScheduleStatus::output_conflict;
			}
			for (size_t j = 0; j < i; ++j) {
				if (specs[j].output != nullptr && std::strcmp(specs[j].output, spec.output) == 0) {
					Log::error(kTag, "refusing schedules: entries %s and %s share the output datapoint '%s'",
						specs[j].id, spec.id, spec.output);
					return ScheduleStatus::output_conflict;
				}
			}
		}
	}

	// Allocate: the entry table, and one block with the dates first (aligned) and the strings.
	Entry* entries = count > 0 ? new (std::nothrow) Entry[count] : nullptr;
	size_t block_size = total_dates * sizeof(Date) + string_bytes;
	char* block = block_size > 0 ? new (std::nothrow) char[block_size] : nullptr;
	if ((count > 0 && entries == nullptr) || (block_size > 0 && block == nullptr)) {
		delete[] entries;
		delete[] block;
		Log::error(kTag, "refusing schedules: out of memory for %u entries", static_cast<unsigned>(count));
		return ScheduleStatus::out_of_memory;
	}
	Date* date_cursor = reinterpret_cast<Date*>(block);
	char* string_cursor = block + total_dates * sizeof(Date);
	for (size_t i = 0; i < count; ++i) {
		Entry& entry = entries[i];
		const ScheduleSpec& spec = specs[i];
		entry.spec = spec;
		size_t id_length = std::strlen(spec.id) + 1;
		std::memcpy(string_cursor, spec.id, id_length);
		entry.spec.id = string_cursor;
		string_cursor += id_length;
		size_t label_length = std::strlen(spec.label) + 1;
		std::memcpy(string_cursor, spec.label, label_length);
		entry.spec.label = string_cursor;
		string_cursor += label_length;
		entry.spec.output = nullptr; // resolved to an id below
		if (spec.kind == ScheduleKind::dates) {
			// Sorted copy (insertion sort, at most kMaxDates).
			for (size_t d = 0; d < spec.date_count; ++d) {
				size_t at = d;
				while (at > 0 && date_less(spec.dates[d], date_cursor[at - 1])) {
					date_cursor[at] = date_cursor[at - 1];
					--at;
				}
				date_cursor[at] = spec.dates[d];
			}
			entry.spec.dates = date_cursor;
			date_cursor += spec.date_count;
		} else {
			entry.spec.dates = nullptr;
			entry.spec.date_count = 0;
		}
		if (spec.output != nullptr) {
			entry.output = model_->find(spec.output)->id();
			entry.has_output = true;
		}
		entry.enabled = spec.enabled;
		record_key(entry.spec.id, entry.key);
		for (size_t j = 0; j < i; ++j) {
			if (std::strcmp(entries[j].key, entry.key) == 0) {
				entry.persistable = false;
				Log::warn(kTag, "schedule %s shares its settings key %s with %s: its last firing and enable "
					"switch are not persisted", entry.spec.id, entry.key, entries[j].spec.id);
			}
		}
	}

	// Swap.
	if (timer_.valid()) {
		loop_.cancel(timer_);
		timer_ = TimerHandle();
	}
	delete[] entries_;
	delete[] block_;
	entries_ = entries;
	block_ = block;
	count_ = count;
	waiting_logged_ = false;

	for (size_t i = 0; i < count_; ++i) {
		restore(entries_[i]);
	}
	delete_orphan_records();
	bind_outputs();
	Log::info(kTag, "loaded %u schedule entries", static_cast<unsigned>(count_));
	evaluate();
	return ScheduleStatus::ok;
}

void Scheduler::clear() {
	if (notifying_) {
		Log::error(kTag, "refusing to clear the schedules from within a firing");
		return;
	}
	if (timer_.valid()) {
		loop_.cancel(timer_);
		timer_ = TimerHandle();
	}
	delete[] entries_;
	delete[] block_;
	entries_ = nullptr;
	block_ = nullptr;
	count_ = 0;
}

void Scheduler::restore(Entry& entry) {
	if (!entry.persistable) {
		return;
	}
	char value[Settings::kMaxStringLength + 1];
	SettingsStatus status = settings_.get_string(entry.key, value, sizeof(value));
	if (status == SettingsStatus::not_found) {
		return;
	}
	const char* p = value;
	int64_t last = kNone;
	int64_t accounted = kNone;
	bool parsed = status == SettingsStatus::ok && p[0] == kRecordVersion && p[1] == ';';
	if (parsed) {
		p += 2;
		parsed = parse_field(p, last) && parse_field(p, accounted);
	}
	char override_flag = 0;
	bool was_idle = false;
	if (parsed) {
		if (*p == '0' || *p == '1') {
			override_flag = *p;
			++p;
		}
		if (*p == 'i') {
			was_idle = true;
			++p;
		}
		parsed = *p == ';';
		++p;
	}
	if (!parsed) {
		Log::warn(kTag, "schedule %s: record %s unreadable (%s); starting without it", entry.spec.id, entry.key,
			status == SettingsStatus::ok ? "format" : settings_status_name(status));
		return;
	}
	if (std::strcmp(p, entry.spec.id) != 0) {
		Log::warn(kTag, "schedule %s: record %s belongs to schedule %s; starting without it", entry.spec.id,
			entry.key, p);
		return;
	}
	entry.last_fired = last;
	// An entry idle when its record was written stayed idle until the controller stopped (anything
	// else would have rewritten the record): it counts from now on.
	entry.accounted = was_idle ? kNone : accounted;
	entry.stored_idle = was_idle;
	if (override_flag != 0) {
		bool value_flag = override_flag == '1';
		if (!entry.spec.enabled_adjustable) {
			Log::warn(kTag, "schedule %s: enable override removed: the image fixes the switch", entry.spec.id);
			persist(entry);
		} else {
			entry.enabled = value_flag;
		}
	}
}

void Scheduler::persist(Entry& entry) {
	if (!entry.persistable) {
		return;
	}
	char value[Settings::kMaxStringLength + 1];
	char last[24] = "";
	char accounted[24] = "";
	if (entry.last_fired != kNone) {
		std::snprintf(last, sizeof(last), "%" PRId64, entry.last_fired);
	}
	if (entry.accounted != kNone) {
		std::snprintf(accounted, sizeof(accounted), "%" PRId64, entry.accounted);
	}
	const char* override_flag = entry.enabled == entry.spec.enabled ? "" : (entry.enabled ? "1" : "0");
	entry.stored_idle = is_idle(entry);
	std::snprintf(value, sizeof(value), "%c;%s;%s;%s%s;%s", kRecordVersion, last, accounted, override_flag,
		entry.stored_idle ? "i" : "", entry.spec.id);
	SettingsStatus status = settings_.set_string(entry.key, value);
	if (status != SettingsStatus::ok) {
		Log::error(kTag, "schedule %s: cannot store record %s (%s)", entry.spec.id, entry.key,
			settings_status_name(status));
	}
}

namespace {

/// Collects up to kBatch keys of records that belong to none of the entries.
class OrphanCollector : public SettingsVisitor {
public:
	static constexpr size_t kBatch = 8;

	OrphanCollector(const char (*keys)[Scheduler::kRecordKeySize], size_t key_stride, size_t count)
		: keys_(keys), stride_(key_stride), count_(count) {}

	void entry(const char* key, SettingType) override {
		if (std::strncmp(key, kRecordPrefix, std::strlen(kRecordPrefix)) != 0 || found_ == kBatch) {
			return;
		}
		for (size_t i = 0; i < count_; ++i) {
			const char* own = reinterpret_cast<const char*>(keys_) + i * stride_;
			if (std::strcmp(own, key) == 0) {
				return;
			}
		}
		std::snprintf(orphans_[found_++], sizeof(orphans_[0]), "%s", key);
	}

	size_t found() const { return found_; }
	const char* orphan(size_t i) const { return orphans_[i]; }

private:
	const char (*keys_)[Scheduler::kRecordKeySize];
	size_t stride_;
	size_t count_;
	char orphans_[kBatch][Settings::kMaxKeyLength + 1] = {};
	size_t found_ = 0;
};

} // namespace

void Scheduler::delete_orphan_records() {
	const char(*keys)[kRecordKeySize] = count_ > 0 ? &entries_[0].key : nullptr;
	for (;;) {
		OrphanCollector collector(keys, sizeof(Entry), count_);
		settings_.visit(collector);
		size_t erased = 0;
		for (size_t i = 0; i < collector.found(); ++i) {
			if (settings_.erase(collector.orphan(i)) == SettingsStatus::ok) {
				++erased;
				Log::info(kTag, "deleted record %s of a schedule no longer in the image", collector.orphan(i));
			} else {
				Log::error(kTag, "cannot delete record %s of a schedule no longer in the image", collector.orphan(i));
			}
		}
		if (collector.found() < OrphanCollector::kBatch || erased == 0) {
			return;
		}
	}
}

void Scheduler::bind_outputs() {
	for (size_t i = 0; i < count_; ++i) {
		Entry& entry = entries_[i];
		if (entry.has_output && model_->bind(entry.output, *this) != DatapointStatus::ok) {
			entry.has_output = false; // the model logged why
		}
	}
}

void Scheduler::update_outputs(int64_t now) {
	if (model_ == nullptr) {
		return;
	}
	Date today;
	if (!time_.local_date(now, today)) {
		return;
	}
	for (size_t i = 0; i < count_; ++i) {
		Entry& entry = entries_[i];
		if (!entry.has_output) {
			continue;
		}
		Date fired_on;
		bool value = entry.last_fired != kNone && time_.local_date(entry.last_fired, fired_on) && fired_on == today;
		const Datapoint* dp = model_->get(entry.output);
		if (dp != nullptr && dp->quality() == Quality::good && dp->value() == Value::of_bool(value)) {
			continue;
		}
		model_->report(*this, entry.output, Value::of_bool(value));
	}
}

std::optional<size_t> Scheduler::find(const char* id) const {
	if (id == nullptr) {
		return std::nullopt;
	}
	for (size_t i = 0; i < count_; ++i) {
		if (std::strcmp(entries_[i].spec.id, id) == 0) {
			return i;
		}
	}
	return std::nullopt;
}

const char* Scheduler::id(size_t index) const {
	return index < count_ ? entries_[index].spec.id : "";
}

const char* Scheduler::label(size_t index) const {
	return index < count_ ? entries_[index].spec.label : "";
}

ScheduleKind Scheduler::kind(size_t index) const {
	return index < count_ ? entries_[index].spec.kind : ScheduleKind::weekly;
}

bool Scheduler::enabled(size_t index) const {
	return index < count_ && entries_[index].enabled;
}

std::optional<int64_t> Scheduler::next_firing(size_t index) const {
	if (index >= count_ || entries_[index].next == kNone) {
		return std::nullopt;
	}
	return entries_[index].next;
}

std::optional<int64_t> Scheduler::next_after(size_t index, int64_t utc) const {
	if (index >= count_) {
		return std::nullopt;
	}
	return occurrence_after(entries_[index], utc);
}

std::optional<int64_t> Scheduler::last_fired(size_t index) const {
	if (index >= count_ || entries_[index].last_fired == kNone) {
		return std::nullopt;
	}
	return entries_[index].last_fired;
}

bool Scheduler::set_enabled(const char* id, bool value, const char** error) {
	std::optional<size_t> index = find(id);
	const char* reason = nullptr;
	if (!index) {
		reason = "no such schedule";
	} else if (!entries_[*index].spec.enabled_adjustable) {
		reason = "the image fixes this schedule's enable switch";
	}
	if (reason != nullptr) {
		Log::error(kTag, "refusing to switch schedule %s %s: %s", id == nullptr ? "" : id, value ? "on" : "off",
			reason);
		if (error != nullptr) {
			*error = reason;
		}
		return false;
	}
	Entry& entry = entries_[*index];
	if (entry.enabled == value) {
		return true;
	}
	entry.enabled = value;
	if (value) {
		// What passed while the entry was off is not missed.
		WallTime now = time_.now_utc();
		entry.accounted = now.synchronized ? now.seconds : kNone;
	}
	persist(entry);
	Log::info(kTag, "schedule %s switched %s", entry.spec.id, value ? "on" : "off");
	evaluate();
	return true;
}

bool Scheduler::add_listener(ScheduleListener& listener) {
	if (notifying_) {
		Log::error(kTag, "refusing to add a schedule listener from within a firing");
		return false;
	}
	for (size_t i = 0; i < listener_count_; ++i) {
		if (listeners_[i] == &listener) {
			Log::error(kTag, "refusing to add a schedule listener twice");
			return false;
		}
	}
	if (listener_count_ == kListenerCapacity) {
		Log::error(kTag, "refusing schedule listener: capacity of %u exhausted",
			static_cast<unsigned>(kListenerCapacity));
		return false;
	}
	listeners_[listener_count_++] = &listener;
	return true;
}

bool Scheduler::remove_listener(ScheduleListener& listener) {
	if (notifying_) {
		Log::error(kTag, "refusing to remove a schedule listener from within a firing");
		return false;
	}
	for (size_t i = 0; i < listener_count_; ++i) {
		if (listeners_[i] == &listener) {
			for (size_t j = i + 1; j < listener_count_; ++j) {
				listeners_[j - 1] = listeners_[j];
			}
			--listener_count_;
			return true;
		}
	}
	return false;
}

bool Scheduler::command(const Datapoint& datapoint, const Value&) {
	Log::error(kTag, "refusing command to '%s': a schedule output is set by the scheduler only", datapoint.name());
	return false;
}

// --- The calendar ---

namespace {

/// The clamp of an astro entry on one date: the sun's moment plus the offset, moved up to
/// `notBefore` and down to `notAfter`. Non-decreasing in the moment, and for a fixed moment in the
/// date: the bound of the astro search relies on both (Scheduler::occurrence_after()).
class AstroClamp {
public:
	AstroClamp(const TimeSource& time, const ScheduleSpec& spec, const Date& date) {
		if (spec.has_not_before) {
			has_low_ = true;
			low_ = time.local_to_utc(date, spec.not_before.hour, spec.not_before.minute, spec.not_before.second);
		}
		if (spec.has_not_after) {
			has_high_ = true;
			high_ = time.local_to_utc(date, spec.not_after.hour, spec.not_after.minute, spec.not_after.second);
		}
	}

	int64_t apply(int64_t at) const {
		if (has_low_ && at < low_) {
			at = low_;
		}
		if (has_high_ && at > high_) {
			at = high_;
		}
		return at;
	}

private:
	bool has_low_ = false;
	bool has_high_ = false;
	int64_t low_ = 0;
	int64_t high_ = 0;
};

} // namespace

std::optional<int64_t> Scheduler::occurrence_on(const Entry& entry, const Date& date) const {
	const ScheduleSpec& spec = entry.spec;
	switch (spec.kind) {
	case ScheduleKind::weekly:
	case ScheduleKind::yearly:
	case ScheduleKind::dates:
		return time_.local_to_utc(date, spec.time.hour, spec.time.minute, spec.time.second);
	case ScheduleKind::once:
		return time_.local_to_utc(date, spec.at_time.hour, spec.at_time.minute, spec.at_time.second);
	case ScheduleKind::astro: {
		SunTimes sun;
		if (!time_.sun_times(date, sun) || !sun.occurs(spec.sun)) {
			return std::nullopt;
		}
		return AstroClamp(time_, spec, date).apply(sun.at(spec.sun) + spec.offset_s);
	}
	case ScheduleKind::interval:
		break;
	}
	return std::nullopt;
}

std::optional<int64_t> Scheduler::interval_after(const Entry& entry, const Date& date, int64_t utc) const {
	const ScheduleSpec& spec = entry.spec;
	int64_t step = spec.every_minutes * 60;
	int64_t from = seconds_of(spec.from);
	int64_t until = seconds_of(spec.until);
	int64_t last_slot = (until - from) / step;
	// Start two hours before the local time of `utc` (more than any UTC offset change), so that
	// only a few slots are converted.
	int64_t local_of_utc = utc + time_.utc_offset(utc) - days_of(date) * 86400;
	int64_t first_slot = 0;
	int64_t margin = local_of_utc - 7200 - from;
	if (margin > 0) {
		first_slot = margin / step;
	}
	for (int64_t k = first_slot; k <= last_slot; ++k) {
		int64_t local = from + k * step;
		int64_t at = time_.local_to_utc(date, static_cast<unsigned>(local / 3600), static_cast<unsigned>(local % 3600 / 60),
			static_cast<unsigned>(local % 60));
		if (at > utc) {
			return at;
		}
	}
	return std::nullopt;
}

std::optional<int64_t> Scheduler::occurrence_after(const Entry& entry, int64_t utc) const {
	const ScheduleSpec& spec = entry.spec;
	Date local;
	if (!time_.local_date(utc, local)) {
		return std::nullopt;
	}
	int64_t today = days_of(local);
	switch (spec.kind) {
	case ScheduleKind::weekly:
	case ScheduleKind::interval:
		for (int d = -1; d <= kSearchDays; ++d) {
			int64_t days = today + d;
			if ((spec.days & Weekdays::bit(weekday_from_days(days))) == 0) {
				continue;
			}
			Date date = date_from_days(days);
			std::optional<int64_t> at = spec.kind == ScheduleKind::interval ? interval_after(entry, date, utc)
				: occurrence_on(entry, date);
			if (at && *at > utc) {
				return at;
			}
		}
		return std::nullopt;
	case ScheduleKind::astro: {
		// The earliest over the window: an offset or a clamp may shift a day's firing across a
		// neighbour's. The sun is computed only for the dates that can hold it (#57): a date's
		// firing lies within its sun window moved by the offset and clamped, which costs no
		// trigonometry. The window's earliest moment grows with the date (the window moves by a
		// day, the clamp's local times by a day give or take a DST hour, and the clamp is
		// non-decreasing in both), so once it is no earlier than the best firing found no later
		// date can beat it. A date whose latest possible firing is not after `utc` is skipped.
		// Polar days and nights only make dates without a firing; the result is the same as the
		// search over every date of the window.
		std::optional<int64_t> best;
		for (int d = -1; d <= kSearchDays; ++d) {
			int64_t days = today + d;
			if ((spec.days & Weekdays::bit(weekday_from_days(days))) == 0) {
				continue;
			}
			Date date = date_from_days(days);
			SunWindow window;
			if (time_.sun_window(date, window)) {
				AstroClamp clamp(time_, spec, date);
				if (best && clamp.apply(window.earliest + spec.offset_s) >= *best) {
					break;
				}
				if (clamp.apply(window.latest + spec.offset_s) <= utc) {
					continue;
				}
			}
			std::optional<int64_t> at = occurrence_on(entry, date);
			if (at && *at > utc && (!best || *at < *best)) {
				best = at;
			}
		}
		return best;
	}
	case ScheduleKind::yearly:
		for (int32_t year = local.year - 1; year <= local.year + 8; ++year) {
			if (spec.day > days_in_month(year, spec.month)) {
				continue; // 29 February outside a leap year
			}
			Date date;
			date.year = static_cast<uint16_t>(year);
			date.month = spec.month;
			date.day = spec.day;
			std::optional<int64_t> at = occurrence_on(entry, date);
			if (at && *at > utc) {
				return at;
			}
		}
		return std::nullopt;
	case ScheduleKind::dates:
		for (size_t i = 0; i < spec.date_count; ++i) {
			int64_t days = days_of(spec.dates[i]);
			if (days < today - 1) {
				continue;
			}
			std::optional<int64_t> at = occurrence_on(entry, spec.dates[i]);
			if (at && *at > utc) {
				return at;
			}
		}
		return std::nullopt;
	case ScheduleKind::once: {
		std::optional<int64_t> at = occurrence_on(entry, spec.at_date);
		if (at && *at > utc) {
			return at;
		}
		return std::nullopt;
	}
	}
	return std::nullopt;
}

std::optional<int64_t> Scheduler::latest_up_to(const Entry& entry, int64_t first, int64_t now) const {
	int64_t cursor = first;
	// Dense kinds: start from a recent occurrence instead of walking the whole gap.
	static const int64_t kProbes[] = {3600, 86400, 8 * 86400};
	for (int64_t back : kProbes) {
		if (now - back <= cursor) {
			break;
		}
		std::optional<int64_t> probe = occurrence_after(entry, now - back);
		if (probe && *probe <= now) {
			cursor = *probe;
			break;
		}
	}
	for (int step = 0; step < kMaxWalk; ++step) {
		std::optional<int64_t> next = occurrence_after(entry, cursor);
		if (!next || *next > now) {
			break;
		}
		cursor = *next;
	}
	return cursor;
}

// --- Evaluation ---

void Scheduler::time_changed(uint8_t) {
	evaluate();
}

bool Scheduler::is_idle(const Entry& entry) {
	return !entry.enabled || entry.astro_refusal_logged;
}

void Scheduler::idle(Entry& entry, int64_t now) {
	// An entry that does not run (switched off, astronomy unavailable) misses nothing: its
	// accounted moment follows the clock. Its record says "idle" (written when it became idle), so
	// that after a reboot or an image swap the time until then is not taken for a gap either.
	if (entry.accounted == kNone || entry.accounted < now - 1) {
		entry.accounted = now - 1;
	}
	if (!entry.stored_idle) {
		persist(entry);
	}
}

void Scheduler::evaluate() {
	if (firing_) {
		return; // fire_due() evaluates when it is done
	}
	WallTime now = time_.now_utc();
	if (!now.synchronized) {
		if (timer_.valid()) {
			loop_.cancel(timer_);
			timer_ = TimerHandle();
		}
		for (size_t i = 0; i < count_; ++i) {
			entries_[i].next = kNone;
		}
		if (count_ > 0 && !waiting_logged_) {
			waiting_logged_ = true;
			Log::info(kTag, "schedules wait for a synchronized wall clock");
		}
		return;
	}
	waiting_logged_ = false;
	update_outputs(now.seconds);
	bool due = false;
	for (size_t i = 0; i < count_; ++i) {
		Entry& entry = entries_[i];
		entry.next = kNone;
		if (!entry.enabled) {
			idle(entry, now.seconds);
			continue;
		}
		if (entry.spec.kind == ScheduleKind::astro) {
			SunWindow window; // only whether astronomy is available: no sun computed
			Date today;
			if (!time_.local_date(now.seconds, today) || !time_.sun_window(today, window)) {
				if (!entry.astro_refusal_logged) {
					entry.astro_refusal_logged = true;
					Log::error(kTag, "schedule %s (%s %s) refused: astronomy needs a location (settings %s, %s)",
						entry.spec.id, schedule_kind_name(entry.spec.kind), sun_event_name(entry.spec.sun),
						setting::kLatitude, setting::kLongitude);
				}
				idle(entry, now.seconds);
				continue;
			}
			if (entry.astro_refusal_logged) {
				entry.astro_refusal_logged = false;
				persist(entry); // running again: the record must no longer say idle
			}
		}
		if (entry.accounted == kNone) {
			entry.accounted = now.seconds - 1; // a new entry: from now on
			persist(entry);
		}
		std::optional<int64_t> first = occurrence_after(entry, entry.accounted);
		if (first && *first <= now.seconds) {
			entry.next = *first;
			due = true;
			continue;
		}
		int64_t base = entry.accounted > now.seconds ? entry.accounted : now.seconds;
		std::optional<int64_t> next = occurrence_after(entry, base);
		entry.next = next ? *next : kNone;
	}
	if (due) {
		if (timer_.valid()) {
			loop_.cancel(timer_);
		}
		timer_ = loop_.schedule_at(time_.monotonic_ms(), Callback::member<Scheduler, &Scheduler::on_timer>(this));
		return;
	}
	arm(now.seconds);
}

void Scheduler::arm(int64_t now) {
	if (timer_.valid()) {
		loop_.cancel(timer_);
		timer_ = TimerHandle();
	}
	int64_t earliest = kNone;
	for (size_t i = 0; i < count_; ++i) {
		int64_t next = entries_[i].next;
		if (next != kNone && (earliest == kNone || next < earliest)) {
			earliest = next;
		}
	}
	if (earliest == kNone) {
		return;
	}
	uint64_t delay_ms = 0;
	if (earliest > now) {
		uint64_t seconds = static_cast<uint64_t>(earliest - now);
		delay_ms = seconds > kMaxTimerMs / 1000 ? kMaxTimerMs : seconds * 1000;
	}
	timer_ = loop_.schedule_at(time_.monotonic_ms() + delay_ms, Callback::member<Scheduler, &Scheduler::on_timer>(this));
}

void Scheduler::format_local(int64_t utc, char* buffer, size_t size) const {
	LocalTime local;
	if (time_.to_local(utc, local)) {
		format_local_time(local, utc, buffer, size);
	} else {
		std::snprintf(buffer, size, "%" PRId64 " s since the epoch", utc);
	}
}

void Scheduler::fire(Entry& entry, int64_t now, bool caught_up) {
	entry.last_fired = now;
	persist(entry);
	if (caught_up) {
		char when[kTimeText];
		format_local(entry.accounted, when, sizeof(when));
		Log::info(kTag, "schedule %s fired (catching up the firing of %s)", entry.spec.id, when);
	} else {
		Log::info(kTag, "schedule %s fired", entry.spec.id);
	}
	if (entry.has_output && model_ != nullptr) {
		model_->report(*this, entry.output, Value::of_bool(true));
	}
	notifying_ = true;
	for (size_t i = 0; i < listener_count_; ++i) {
		listeners_[i]->schedule_fired(entry.spec.id);
	}
	notifying_ = false;
}

void Scheduler::on_timer() {
	timer_ = TimerHandle();
	fire_due();
}

void Scheduler::fire_due() {
	WallTime wall = time_.now_utc();
	if (!wall.synchronized) {
		evaluate();
		return;
	}
	int64_t now = wall.seconds;
	// Decide every due entry first, then fire in the order of the occurrences (image order on ties).
	for (size_t i = 0; i < count_; ++i) {
		Entry& entry = entries_[i];
		entry.next = kNone;
		if (!entry.enabled || entry.accounted == kNone) {
			continue;
		}
		std::optional<int64_t> first = occurrence_after(entry, entry.accounted);
		if (!first || *first > now) {
			continue;
		}
		std::optional<int64_t> latest = latest_up_to(entry, *first, now);
		int64_t last = latest ? *latest : *first;
		bool on_time = now - last <= kGraceS;
		if (*first < last || !on_time) {
			char from[kTimeText];
			char until[kTimeText];
			format_local(*first, from, sizeof(from));
			int64_t last_missed = last;
			if (on_time) {
				// The latest is on time; the missed ones end before it.
				std::optional<int64_t> before = *first;
				for (int step = 0; step < kMaxWalk; ++step) {
					std::optional<int64_t> next = occurrence_after(entry, *before);
					if (!next || *next >= last) {
						break;
					}
					before = next;
				}
				last_missed = *before;
			}
			format_local(last_missed, until, sizeof(until));
			const char* action = on_time ? "firing the current one"
				: entry.spec.missed == MissedPolicy::catch_up ? "catching up once" : "skipped";
			if (*first == last_missed) {
				Log::warn(kTag, "schedule %s missed its firing of %s; %s", entry.spec.id, from, action);
			} else {
				Log::warn(kTag, "schedule %s missed its firings from %s to %s; %s", entry.spec.id, from, until, action);
			}
		}
		if (on_time || entry.spec.missed == MissedPolicy::catch_up) {
			entry.next = last; // fire below
		} else {
			entry.accounted = last;
			persist(entry);
		}
	}
	// A listener may switch entries (set_enabled); the evaluation waits until all have fired.
	firing_ = true;
	for (;;) {
		Entry* pick = nullptr;
		for (size_t i = 0; i < count_; ++i) {
			Entry& entry = entries_[i];
			if (entry.next != kNone && entry.enabled && (pick == nullptr || entry.next < pick->next)) {
				pick = &entry;
			}
		}
		if (pick == nullptr) {
			break;
		}
		int64_t occurrence = pick->next;
		pick->next = kNone;
		bool caught_up = now - occurrence > kGraceS;
		pick->accounted = occurrence;
		fire(*pick, now, caught_up);
	}
	firing_ = false;
	evaluate();
}

} // namespace hc
