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

/// Version of the record with member overrides (issue #58); a record without overrides is still
/// written in kRecordVersion, so that an older firmware keeps reading it.
constexpr char kRecordVersionOverrides = '2';

constexpr const char* const kMemberNames[ScheduleMember::kCount] = {"time", "days", "offset", "notBefore",
	"notAfter", "every", "from", "until", "at"};

constexpr uint16_t kTimeMembers = ScheduleMember::kTime | ScheduleMember::kNotBefore | ScheduleMember::kNotAfter |
	ScheduleMember::kFrom | ScheduleMember::kUntil;

/// Whether `value` is a value of `member` at all (its range, before any bounds).
bool member_in_range(uint16_t member, int64_t value) {
	if ((member & kTimeMembers) != 0) {
		return value >= 0 && value < 86400;
	}
	switch (member) {
	case ScheduleMember::kDays:
		return value >= 1 && value <= Weekdays::kAll;
	case ScheduleMember::kOffset:
		return value >= -kMaxOffsetS && value <= kMaxOffsetS;
	case ScheduleMember::kEvery:
		return value >= 60 && value <= 720 * 60 && value % 60 == 0;
	case ScheduleMember::kAt: {
		if (value < 0) {
			return false;
		}
		Date date = date_from_days(value / 86400);
		return date.year >= 1970 && date.year <= 2155;
	}
	default:
		return false;
	}
}

/// What a value outside the member's range is told.
const char* range_reason(uint16_t member) {
	switch (member) {
	case ScheduleMember::kDays: return "days must name 1..7 days of the week";
	case ScheduleMember::kOffset: return "offset beyond 12 h";
	case ScheduleMember::kEvery: return "every must be a whole number of minutes, 1 to 720";
	case ScheduleMember::kAt: return "at is not a date and time in 1970..2155";
	default: return "not a time of day";
	}
}

/// The member's value for people: 06:30:00, 1,2,3, -1800 s, 15 min, 2026-12-31 23:59:30.
void format_member(uint16_t member, int64_t value, char* buffer, size_t size) {
	if ((member & kTimeMembers) != 0) {
		std::snprintf(buffer, size, "%02d:%02d:%02d", static_cast<int>(value / 3600), static_cast<int>(value % 3600 / 60),
			static_cast<int>(value % 60));
		return;
	}
	switch (member) {
	case ScheduleMember::kDays: {
		size_t used = 0;
		buffer[0] = '\0';
		for (unsigned day = 1; day <= 7 && used + 3 < size; ++day) {
			if ((value & Weekdays::bit(day)) != 0) {
				used += static_cast<size_t>(std::snprintf(buffer + used, size - used, used > 0 ? ",%u" : "%u", day));
			}
		}
		return;
	}
	case ScheduleMember::kOffset:
		std::snprintf(buffer, size, "%+" PRId64 " s", value);
		return;
	case ScheduleMember::kEvery:
		std::snprintf(buffer, size, "%" PRId64 " min", value / 60);
		return;
	case ScheduleMember::kAt: {
		Date date = date_from_days(value / 86400);
		int64_t rest = value % 86400;
		std::snprintf(buffer, size, "%04u-%02u-%02u %02d:%02d:%02d", date.year, date.month, date.day,
			static_cast<int>(rest / 3600), static_cast<int>(rest % 3600 / 60), static_cast<int>(rest % 60));
		return;
	}
	default:
		std::snprintf(buffer, size, "%" PRId64, value);
	}
}

LocalClockTime clock_of(int64_t seconds) {
	LocalClockTime t;
	t.hour = static_cast<uint8_t>(seconds / 3600);
	t.minute = static_cast<uint8_t>(seconds % 3600 / 60);
	t.second = static_cast<uint8_t>(seconds % 60);
	return t;
}

/// The members of `spec` contradict each other (an interval's `from` after its `until`, an astro
/// entry's `notBefore` after its `notAfter`): the pair's bits, else 0.
uint16_t contradiction(const ScheduleSpec& spec) {
	if (spec.kind == ScheduleKind::interval && seconds_of(spec.from) > seconds_of(spec.until)) {
		return ScheduleMember::kFrom | ScheduleMember::kUntil;
	}
	if (spec.kind == ScheduleKind::astro && spec.has_not_before && spec.has_not_after &&
		seconds_of(spec.not_before) > seconds_of(spec.not_after)) {
		return ScheduleMember::kNotBefore | ScheduleMember::kNotAfter;
	}
	return 0;
}

size_t bit_count(uint16_t bits) {
	size_t n = 0;
	for (; bits != 0; bits = static_cast<uint16_t>(bits & (bits - 1))) {
		++n;
	}
	return n;
}

} // namespace

const char* ScheduleMember::name(uint16_t member) {
	for (size_t i = 0; i < kCount; ++i) {
		if (member == bit(i)) {
			return kMemberNames[i];
		}
	}
	return "?";
}

uint16_t ScheduleMember::of_name(const char* name, size_t length) {
	for (size_t i = 0; i < kCount; ++i) {
		if (std::strlen(kMemberNames[i]) == length && std::strncmp(kMemberNames[i], name, length) == 0) {
			return bit(i);
		}
	}
	return 0;
}

uint16_t ScheduleMember::allowed(ScheduleKind kind) {
	switch (kind) {
	case ScheduleKind::weekly: return kTime | kDays;
	case ScheduleKind::yearly: return kTime;
	case ScheduleKind::dates: return kTime;
	case ScheduleKind::astro: return kOffset | kNotBefore | kNotAfter | kDays;
	case ScheduleKind::interval: return kEvery | kFrom | kUntil | kDays;
	case ScheduleKind::once: return kAt;
	}
	return 0;
}

int64_t schedule_member_value(const ScheduleSpec& spec, uint16_t member) {
	switch (member) {
	case ScheduleMember::kTime: return seconds_of(spec.time);
	case ScheduleMember::kDays: return spec.days;
	case ScheduleMember::kOffset: return spec.offset_s;
	case ScheduleMember::kNotBefore: return seconds_of(spec.not_before);
	case ScheduleMember::kNotAfter: return seconds_of(spec.not_after);
	case ScheduleMember::kEvery: return static_cast<int64_t>(spec.every_minutes) * 60;
	case ScheduleMember::kFrom: return seconds_of(spec.from);
	case ScheduleMember::kUntil: return seconds_of(spec.until);
	case ScheduleMember::kAt: return days_of(spec.at_date) * 86400 + seconds_of(spec.at_time);
	default: return 0;
	}
}

void set_schedule_member_value(ScheduleSpec& spec, uint16_t member, int64_t value) {
	switch (member) {
	case ScheduleMember::kTime: spec.time = clock_of(value); break;
	case ScheduleMember::kDays: spec.days = static_cast<uint8_t>(value); break;
	case ScheduleMember::kOffset: spec.offset_s = static_cast<int32_t>(value); break;
	case ScheduleMember::kNotBefore: spec.not_before = clock_of(value); break;
	case ScheduleMember::kNotAfter: spec.not_after = clock_of(value); break;
	case ScheduleMember::kEvery: spec.every_minutes = static_cast<uint16_t>(value / 60); break;
	case ScheduleMember::kFrom: spec.from = clock_of(value); break;
	case ScheduleMember::kUntil: spec.until = clock_of(value); break;
	case ScheduleMember::kAt:
		spec.at_date = date_from_days(value / 86400);
		spec.at_time = clock_of(value % 86400);
		break;
	default: break;
	}
}

const char* schedule_adjustable_error(const ScheduleSpec& spec) {
	uint16_t allowed = ScheduleMember::allowed(spec.kind);
	if ((spec.adjustable & ~allowed) != 0) {
		return "adjustable names a member this kind cannot adjust";
	}
	if ((spec.adjustable & ScheduleMember::kNotBefore) != 0 && !spec.has_not_before) {
		return "notBefore is adjustable, but the entry sets none";
	}
	if ((spec.adjustable & ScheduleMember::kNotAfter) != 0 && !spec.has_not_after) {
		return "notAfter is adjustable, but the entry sets none";
	}
	if (spec.bound_count > ScheduleMember::kCount || (spec.bound_count > 0 && spec.bounds == nullptr)) {
		return "more bounds than members";
	}
	uint16_t seen = 0;
	for (size_t i = 0; i < spec.bound_count; ++i) {
		const ScheduleBound& b = spec.bounds[i];
		if (bit_count(b.member) != 1 || (b.member & allowed) == 0) {
			return "bounds name a member this kind does not have";
		}
		if ((b.member & seen) != 0) {
			return "bounds name a member twice";
		}
		seen = static_cast<uint16_t>(seen | b.member);
		if ((b.member & spec.adjustable) == 0) {
			return "bounds name a member that is not adjustable";
		}
		if (b.member == ScheduleMember::kDays) {
			return "days take no bounds";
		}
		if (!member_in_range(b.member, b.min) || !member_in_range(b.member, b.max)) {
			return "a bound is not a value of its member";
		}
		if (b.min > b.max) {
			return "a bound's min is greater than its max";
		}
		if (b.step < 0 || (b.step > 0 && b.member == ScheduleMember::kAt)) {
			return b.step < 0 ? "a step must be positive" : "at takes no step";
		}
		if (b.step > 0 && b.member == ScheduleMember::kEvery && b.step % 60 != 0) {
			return "the step of every must be a whole number of minutes";
		}
		if (b.step > 0 && (b.member & kTimeMembers) != 0 && b.step > 86400) {
			return "the step of a time is at most a day";
		}
		int64_t value = schedule_member_value(spec, b.member);
		if (value < b.min || value > b.max) {
			return "the image's value lies outside its bounds";
		}
		if (b.step > 0 && value % b.step != 0) {
			return "the image's value is not on its step";
		}
	}
	return nullptr;
}

bool schedule_time_datapoint_name(const char* id, char* out, size_t size) {
	int n = std::snprintf(out, size, "%s%s/time", kScheduleDatapointPrefix, id);
	return n > 0 && static_cast<size_t>(n) < size;
}

const char* schedule_change_name(ScheduleChange change) {
	switch (change) {
	case ScheduleChange::ok: return "ok";
	case ScheduleChange::unknown: return "unknown";
	case ScheduleChange::not_adjustable: return "not_adjustable";
	case ScheduleChange::out_of_bounds: return "out_of_bounds";
	case ScheduleChange::malformed: return "malformed";
	}
	return "?";
}

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
	/// The image's scalars of the adjustable members, in member order (issue #58), in the block.
	const int64_t* image_values = nullptr;
	uint16_t overridden = 0; ///< ScheduleMember bits whose effective value is an override.
	uint16_t time_dp = 0;    ///< The datapoint `schedule/<id>/time`, when has_time_dp.
	bool has_time_dp = false;

	/// The image's scalar of an adjustable member.
	int64_t image_value(uint16_t member) const {
		size_t at = 0;
		for (size_t i = 0; i < ScheduleMember::kCount; ++i) {
			uint16_t bit = ScheduleMember::bit(i);
			if (bit == member) {
				return image_values[at];
			}
			if ((spec.adjustable & bit) != 0) {
				++at;
			}
		}
		return 0;
	}
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
	if (const char* error = schedule_adjustable_error(spec)) {
		return refuse(error);
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
	size_t total_bounds = 0;
	size_t total_values = 0;
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
		total_bounds += spec.bound_count;
		total_values += bit_count(spec.adjustable);
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

	// Allocate: the entry table, and one block with the bounds and the image's values of the
	// adjustable members first (8-byte aligned), then the dates, then the strings.
	Entry* entries = count > 0 ? new (std::nothrow) Entry[count] : nullptr;
	size_t head_size = total_bounds * sizeof(ScheduleBound) + total_values * sizeof(int64_t);
	size_t block_size = head_size + total_dates * sizeof(Date) + string_bytes;
	char* block = block_size > 0 ? new (std::nothrow) char[block_size] : nullptr;
	if ((count > 0 && entries == nullptr) || (block_size > 0 && block == nullptr)) {
		delete[] entries;
		delete[] block;
		Log::error(kTag, "refusing schedules: out of memory for %u entries", static_cast<unsigned>(count));
		return ScheduleStatus::out_of_memory;
	}
	ScheduleBound* bound_cursor = reinterpret_cast<ScheduleBound*>(block);
	int64_t* value_cursor = reinterpret_cast<int64_t*>(block + total_bounds * sizeof(ScheduleBound));
	Date* date_cursor = reinterpret_cast<Date*>(block + head_size);
	char* string_cursor = block + head_size + total_dates * sizeof(Date);
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
		for (size_t b = 0; b < spec.bound_count; ++b) {
			bound_cursor[b] = spec.bounds[b];
		}
		entry.spec.bounds = spec.bound_count > 0 ? bound_cursor : nullptr;
		bound_cursor += spec.bound_count;
		entry.image_values = value_cursor;
		for (size_t m = 0; m < ScheduleMember::kCount; ++m) {
			if ((spec.adjustable & ScheduleMember::bit(m)) != 0) {
				*value_cursor++ = schedule_member_value(spec, ScheduleMember::bit(m));
			}
		}
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
	bool with_overrides = status == SettingsStatus::ok && p[0] == kRecordVersionOverrides;
	bool parsed = status == SettingsStatus::ok && (p[0] == kRecordVersion || with_overrides) && p[1] == ';';
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
	// Version 2: the member overrides, `<member>=<scalar>` separated by commas, up to a ';'.
	const char* overrides = nullptr;
	if (parsed && with_overrides) {
		overrides = p;
		const char* end = std::strchr(p, ';');
		parsed = end != nullptr;
		if (parsed) {
			p = end + 1;
		}
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
	bool rewrite = false;
	if (overrides != nullptr) {
		size_t length = static_cast<size_t>(std::strchr(overrides, ';') - overrides);
		char text[Settings::kMaxStringLength + 1];
		std::memcpy(text, overrides, length);
		text[length] = '\0';
		rewrite = restore_overrides(entry, text);
	}
	if (override_flag != 0) {
		bool value_flag = override_flag == '1';
		if (!entry.spec.enabled_adjustable) {
			Log::warn(kTag, "schedule %s: enable override removed: the image fixes the switch", entry.spec.id);
			rewrite = true;
		} else {
			entry.enabled = value_flag;
		}
	}
	if (rewrite) {
		persist(entry);
	}
}

bool Scheduler::restore_overrides(Entry& entry, const char* text) {
	// The override of a member is kept while the image declares the member adjustable and the value
	// fits the member's bounds; otherwise it is dropped with a log line (spec/schedules.md 4.2). An
	// override equal to the new image's value is no override any more.
	bool rewrite = false;
	const char* p = text;
	while (*p != '\0') {
		const char* equals = std::strchr(p, '=');
		const char* comma = std::strchr(p, ',');
		const char* end = comma != nullptr ? comma : p + std::strlen(p);
		if (equals == nullptr || equals > end) {
			Log::warn(kTag, "schedule %s: unreadable member override '%.*s' dropped", entry.spec.id,
				static_cast<int>(end - p), p);
			rewrite = true;
		} else {
			uint16_t member = ScheduleMember::of_name(p, static_cast<size_t>(equals - p));
			char digits[24];
			size_t length = static_cast<size_t>(end - equals - 1);
			int64_t value = 0;
			bool number = length > 0 && length + 2 <= sizeof(digits);
			if (number) {
				std::memcpy(digits, equals + 1, length);
				digits[length] = ';'; // parse_field reads up to a ';'
				digits[length + 1] = '\0';
				const char* q = digits;
				number = parse_field(q, value) && value != kNone;
			}
			const ScheduleBound* b = nullptr;
			for (size_t i = 0; member != 0 && i < entry.spec.bound_count; ++i) {
				if (entry.spec.bounds[i].member == member) {
					b = &entry.spec.bounds[i];
				}
			}
			const char* why = nullptr;
			if (member == 0 || !number || !member_in_range(member, value)) {
				why = "unreadable";
			} else if ((entry.spec.adjustable & member) == 0) {
				why = "the image no longer declares it adjustable";
			} else if (b != nullptr && (value < b->min || value > b->max)) {
				why = "it no longer fits the image's bounds";
			} else if (b != nullptr && b->step > 0 && value % b->step != 0) {
				why = "it is not on the image's step";
			}
			if (why != nullptr) {
				char shown[40];
				if (member != 0 && number) {
					format_member(member, value, shown, sizeof(shown));
				} else {
					std::snprintf(shown, sizeof(shown), "%.*s", static_cast<int>(end - equals - 1), equals + 1);
				}
				Log::warn(kTag, "schedule %s: override of %.*s (%s) dropped: %s", entry.spec.id,
					static_cast<int>(equals - p), p, shown, why);
				rewrite = true;
			} else if (value == entry.image_value(member)) {
				rewrite = true; // the image's value now: no override
			} else {
				set_schedule_member_value(entry.spec, member, value);
				entry.overridden = static_cast<uint16_t>(entry.overridden | member);
			}
		}
		p = comma != nullptr ? comma + 1 : end;
	}
	uint16_t pair = contradiction(entry.spec);
	if (pair != 0) {
		uint16_t dropped = static_cast<uint16_t>(pair & entry.overridden);
		for (size_t i = 0; i < ScheduleMember::kCount; ++i) {
			uint16_t member = ScheduleMember::bit(i);
			if ((dropped & member) != 0) {
				drop_override(entry, member, "it contradicts the entry's other values");
			}
		}
		rewrite = true;
	}
	return rewrite;
}

void Scheduler::drop_override(Entry& entry, uint16_t member, const char* why) {
	char shown[40];
	format_member(member, schedule_member_value(entry.spec, member), shown, sizeof(shown));
	Log::warn(kTag, "schedule %s: override of %s (%s) dropped: %s", entry.spec.id, ScheduleMember::name(member), shown,
		why);
	set_schedule_member_value(entry.spec, member, entry.image_value(member));
	entry.overridden = static_cast<uint16_t>(entry.overridden & ~member);
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
	// The member overrides (version 2): `<member>=<scalar>,...`, at most 9 * 21 bytes in theory, in
	// practice the members of one kind (an astro entry: 4 members, 52 bytes).
	char overrides[72] = "";
	size_t used = 0;
	for (size_t i = 0; i < ScheduleMember::kCount; ++i) {
		uint16_t member = ScheduleMember::bit(i);
		if ((entry.overridden & member) == 0) {
			continue;
		}
		int n = std::snprintf(overrides + used, sizeof(overrides) - used, "%s%s=%" PRId64, used > 0 ? "," : "",
			kMemberNames[i], schedule_member_value(entry.spec, member));
		if (n < 0 || used + static_cast<size_t>(n) >= sizeof(overrides)) {
			overrides[used] = '\0';
			Log::error(kTag, "schedule %s: the overrides do not fit the record; %s is not persisted", entry.spec.id,
				kMemberNames[i]);
			continue;
		}
		used += static_cast<size_t>(n);
	}
	int written;
	if (used == 0) {
		written = std::snprintf(value, sizeof(value), "%c;%s;%s;%s%s;%s", kRecordVersion, last, accounted, override_flag,
			entry.stored_idle ? "i" : "", entry.spec.id);
	} else {
		written = std::snprintf(value, sizeof(value), "%c;%s;%s;%s%s;%s;%s", kRecordVersionOverrides, last, accounted,
			override_flag, entry.stored_idle ? "i" : "", overrides, entry.spec.id);
	}
	if (written < 0 || static_cast<size_t>(written) >= sizeof(value)) {
		Log::error(kTag, "schedule %s: record %s too long; not stored", entry.spec.id, entry.key);
		return;
	}
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
		entry.has_time_dp = false;
		if (model_ == nullptr || !schedule_has_time_datapoint(entry.spec.kind, entry.spec.adjustable)) {
			continue;
		}
		// The time datapoint the image loader implies (spec/schedules.md 2.2); a model without it (a
		// test, a scheduler without the image) simply has none.
		char name[Datapoint::kMaxName + 1];
		const Datapoint* dp = schedule_time_datapoint_name(entry.spec.id, name, sizeof(name)) ? model_->find(name) : nullptr;
		if (dp == nullptr) {
			continue;
		}
		if (dp->type() != ValueType::time_of_day) {
			Log::error(kTag, "schedule %s: datapoint '%s' is %s, not a time of day; not bound", entry.spec.id, name,
				value_type_name(dp->type()));
			continue;
		}
		if (model_->bind(dp->id(), *this) != DatapointStatus::ok) {
			continue; // the model logged why
		}
		entry.time_dp = dp->id();
		entry.has_time_dp = true;
		if (!defer_times_) {
			report_time(entry);
		}
	}
}

void Scheduler::publish_times() {
	for (size_t i = 0; i < count_; ++i) {
		report_time(entries_[i]);
	}
}

void Scheduler::report_time(Entry& entry) {
	if (!entry.has_time_dp || model_ == nullptr || commanding_) {
		return;
	}
	TimeOfDay t;
	t.hour = entry.spec.time.hour;
	t.minute = entry.spec.time.minute;
	t.second = entry.spec.time.second;
	Value value = Value::of_time(t);
	const Datapoint* dp = model_->get(entry.time_dp);
	if (dp != nullptr && dp->quality() == Quality::good && dp->value() == value) {
		return;
	}
	model_->report(*this, entry.time_dp, value);
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

bool Scheduler::enabled_adjustable(size_t index) const {
	return index < count_ && entries_[index].spec.enabled_adjustable;
}

bool Scheduler::enabled_overridden(size_t index) const {
	return index < count_ && entries_[index].enabled != entries_[index].spec.enabled;
}

const ScheduleSpec& Scheduler::spec(size_t index) const {
	static const ScheduleSpec kNone;
	return index < count_ ? entries_[index].spec : kNone;
}

ScheduleSpec Scheduler::image_spec(size_t index) const {
	if (index >= count_) {
		return ScheduleSpec();
	}
	const Entry& entry = entries_[index];
	ScheduleSpec image = entry.spec;
	for (size_t i = 0; i < ScheduleMember::kCount; ++i) {
		uint16_t member = ScheduleMember::bit(i);
		if ((entry.overridden & member) != 0) {
			set_schedule_member_value(image, member, entry.image_value(member));
		}
	}
	return image;
}

uint16_t Scheduler::adjustable(size_t index) const {
	return index < count_ ? entries_[index].spec.adjustable : 0;
}

uint16_t Scheduler::overridden(size_t index) const {
	return index < count_ ? entries_[index].overridden : 0;
}

const ScheduleBound* Scheduler::bound(size_t index, uint16_t member) const {
	if (index >= count_) {
		return nullptr;
	}
	const ScheduleSpec& spec = entries_[index].spec;
	for (size_t i = 0; i < spec.bound_count; ++i) {
		if (spec.bounds[i].member == member) {
			return &spec.bounds[i];
		}
	}
	return nullptr;
}

ScheduleChange Scheduler::check_member(const Entry& entry, uint16_t member, const ScheduleSpec& values,
	const char** reason) const {
	if ((entry.spec.adjustable & member) == 0) {
		*reason = (ScheduleMember::allowed(entry.spec.kind) & member) == 0 ? "the entry's kind has no such member"
			: "the image does not let the inhabitant change it";
		return ScheduleChange::not_adjustable;
	}
	bool valid;
	switch (member) {
	case ScheduleMember::kTime: valid = time_valid(values.time); break;
	case ScheduleMember::kNotBefore: valid = time_valid(values.not_before); break;
	case ScheduleMember::kNotAfter: valid = time_valid(values.not_after); break;
	case ScheduleMember::kFrom: valid = time_valid(values.from); break;
	case ScheduleMember::kUntil: valid = time_valid(values.until); break;
	case ScheduleMember::kAt: valid = date_valid(values.at_date) && time_valid(values.at_time); break;
	case ScheduleMember::kOffset: valid = member_in_range(member, values.offset_s); break;
	case ScheduleMember::kEvery: valid = member_in_range(member, static_cast<int64_t>(values.every_minutes) * 60); break;
	default: valid = values.days != 0 && (values.days & ~Weekdays::kAll) == 0; break;
	}
	int64_t value = schedule_member_value(values, member);
	if (!valid) {
		*reason = range_reason(member);
		return ScheduleChange::malformed;
	}
	for (size_t i = 0; i < entry.spec.bound_count; ++i) {
		const ScheduleBound& b = entry.spec.bounds[i];
		if (b.member != member) {
			continue;
		}
		if (value < b.min || value > b.max) {
			*reason = "outside the member's bounds";
			return ScheduleChange::out_of_bounds;
		}
		if (b.step > 0 && value % b.step != 0) {
			*reason = "not on the member's step";
			return ScheduleChange::out_of_bounds;
		}
	}
	return ScheduleChange::ok;
}

ScheduleChange Scheduler::set_members(const char* id, uint16_t members, const ScheduleSpec& values, uint16_t* culprit,
	const char** reason) {
	std::optional<size_t> index = find(id);
	const char* why = nullptr;
	uint16_t bad = 0;
	ScheduleChange result = ScheduleChange::ok;
	auto refuse = [&]() {
		Log::error(kTag, "refusing to change schedule %s: %s%s%s (%s)", id == nullptr ? "" : id,
			bad != 0 ? ScheduleMember::name(bad) : "", bad != 0 ? ": " : "", why, schedule_change_name(result));
		if (culprit != nullptr) {
			*culprit = bad;
		}
		if (reason != nullptr) {
			*reason = why;
		}
		return result;
	};
	if (!index) {
		why = "no such schedule";
		result = ScheduleChange::unknown;
		return refuse();
	}
	Entry& entry = entries_[*index];
	ScheduleSpec candidate = entry.spec;
	for (size_t i = 0; i < ScheduleMember::kCount; ++i) {
		uint16_t member = ScheduleMember::bit(i);
		if ((members & member) == 0) {
			continue;
		}
		result = check_member(entry, member, values, &why);
		if (result != ScheduleChange::ok) {
			bad = member;
			return refuse();
		}
		set_schedule_member_value(candidate, member, schedule_member_value(values, member));
	}
	uint16_t pair = contradiction(candidate);
	if (pair != 0) {
		uint16_t named = static_cast<uint16_t>((pair & members) != 0 ? pair & members : pair);
		bad = static_cast<uint16_t>(named & ~(named - 1)); // the first of the pair
		why = pair == (ScheduleMember::kFrom | ScheduleMember::kUntil) ? "from would be later than until"
			: "notBefore would be later than notAfter";
		result = ScheduleChange::malformed;
		return refuse();
	}
	ScheduleSpec before = entry.spec;
	bool changed = false;
	for (size_t i = 0; i < ScheduleMember::kCount; ++i) {
		uint16_t member = ScheduleMember::bit(i);
		if ((members & member) == 0) {
			continue;
		}
		int64_t value = schedule_member_value(candidate, member);
		if (value == schedule_member_value(before, member)) {
			continue;
		}
		changed = true;
		set_schedule_member_value(entry.spec, member, value);
		bool image = value == entry.image_value(member);
		entry.overridden = static_cast<uint16_t>(image ? entry.overridden & ~member : entry.overridden | member);
		char shown[40];
		format_member(member, value, shown, sizeof(shown));
		Log::info(kTag, "schedule %s: %s set to %s%s", entry.spec.id, ScheduleMember::name(member), shown,
			image ? " (the image's value)" : "");
	}
	if (changed) {
		apply_change(entry, before, commanding_);
	}
	return ScheduleChange::ok;
}

bool Scheduler::reset_overrides(const char* id, const char** reason) {
	std::optional<size_t> index = find(id);
	if (!index) {
		Log::error(kTag, "refusing to reset schedule %s: no such schedule", id == nullptr ? "" : id);
		if (reason != nullptr) {
			*reason = "no such schedule";
		}
		return false;
	}
	Entry& entry = entries_[*index];
	if (entry.overridden == 0 && entry.enabled == entry.spec.enabled) {
		return true;
	}
	ScheduleSpec before = entry.spec;
	for (size_t i = 0; i < ScheduleMember::kCount; ++i) {
		uint16_t member = ScheduleMember::bit(i);
		if ((entry.overridden & member) != 0) {
			set_schedule_member_value(entry.spec, member, entry.image_value(member));
		}
	}
	entry.overridden = 0;
	entry.enabled = entry.spec.enabled;
	Log::info(kTag, "schedule %s: back to the image's values", entry.spec.id);
	apply_change(entry, before, false);
	return true;
}

void Scheduler::apply_change(Entry& entry, const ScheduleSpec& before, bool from_command) {
	account_change(entry, before);
	persist(entry);
	if (!from_command) {
		report_time(entry);
		evaluate();
	} else {
		// Within the model's command: nothing may report now; the evaluation follows from the loop.
		evaluate_soon();
	}
}

void Scheduler::evaluate_soon() {
	if (timer_.valid()) {
		loop_.cancel(timer_);
	}
	timer_ = loop_.schedule_at(time_.monotonic_ms(), Callback::member<Scheduler, &Scheduler::on_timer>(this));
}

std::optional<int64_t> Scheduler::firing_on(const Entry& entry, const Date& date) const {
	const ScheduleSpec& spec = entry.spec;
	switch (spec.kind) {
	case ScheduleKind::weekly:
	case ScheduleKind::astro:
		if ((spec.days & Weekdays::bit(weekday_from_days(days_of(date)))) == 0) {
			return std::nullopt;
		}
		break;
	case ScheduleKind::yearly:
		if (date.month != spec.month || date.day != spec.day) {
			return std::nullopt;
		}
		break;
	case ScheduleKind::dates: {
		bool listed = false;
		for (size_t i = 0; i < spec.date_count && !listed; ++i) {
			listed = spec.dates[i] == date;
		}
		if (!listed) {
			return std::nullopt;
		}
		break;
	}
	case ScheduleKind::once:
		if (!(date == spec.at_date)) {
			return std::nullopt;
		}
		break;
	case ScheduleKind::interval:
		return std::nullopt;
	}
	return occurrence_on(entry, date);
}

void Scheduler::account_change(Entry& entry, const ScheduleSpec& before) {
	// A change is not a gap (spec/schedules.md 4.3): the moments up to now count as handled. And a
	// day whose firing was handled under the old values keeps it handled under the new ones: moving
	// a fired 06:30 to 07:00 does not fire a second time that day. An interval's slots are handled
	// up to now only; a once entry that fired never fires again.
	WallTime now = time_.now_utc();
	if (!now.synchronized || entry.accounted == kNone) {
		return; // counts from its first evaluation anyway
	}
	int64_t handled = entry.accounted > now.seconds - 1 ? entry.accounted : now.seconds - 1;
	Entry old = entry;
	old.spec = before;
	if (entry.spec.kind == ScheduleKind::once) {
		std::optional<int64_t> was = occurrence_on(old, before.at_date);
		std::optional<int64_t> moved = occurrence_on(entry, entry.spec.at_date);
		if (was && *was <= entry.accounted && moved && *moved > handled) {
			handled = *moved;
		}
	} else if (entry.spec.kind != ScheduleKind::interval) {
		Date local;
		if (time_.local_date(entry.accounted, local)) {
			int64_t day = days_of(local);
			for (int64_t d = day - 1; d <= day; ++d) {
				Date date = date_from_days(d);
				std::optional<int64_t> was = firing_on(old, date);
				if (!was || *was > entry.accounted) {
					continue;
				}
				std::optional<int64_t> moved = firing_on(entry, date);
				if (moved && *moved > handled) {
					handled = *moved;
				}
			}
		}
	}
	entry.accounted = handled;
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

bool Scheduler::command(const Datapoint& datapoint, const Value& value) {
	for (size_t i = 0; i < count_; ++i) {
		Entry& entry = entries_[i];
		if (!entry.has_time_dp || entry.time_dp != datapoint.id()) {
			continue;
		}
		// The time datapoint: a command is a change of the entry's time, like the API's PUT.
		if (value.type() != ValueType::time_of_day) {
			Log::error(kTag, "refusing command to '%s': not a time of day", datapoint.name());
			return false;
		}
		TimeOfDay t = value.as_time();
		if (t.day_of_week != 0) {
			Log::error(kTag, "refusing command to '%s': a schedule's time has no day of the week", datapoint.name());
			return false;
		}
		ScheduleSpec values = entry.spec;
		values.time.hour = t.hour;
		values.time.minute = t.minute;
		values.time.second = t.second;
		commanding_ = true;
		ScheduleChange result = set_members(entry.spec.id, ScheduleMember::kTime, values);
		commanding_ = false;
		return result == ScheduleChange::ok;
	}
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
