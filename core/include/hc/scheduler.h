/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_SCHEDULER_H
#define HC_SCHEDULER_H

#include <cstddef>
#include <cstdint>
#include <optional>

#include "hc/astro.h"
#include "hc/datapoint.h"
#include "hc/event_loop.h"
#include "hc/technology.h"
#include "hc/time_service.h"
#include "hc/platform/settings.h"

namespace hc {

/// The kinds of schedule entries (spec/schedules.md).
enum class ScheduleKind : uint8_t {
	weekly,   ///< On the given days of the week at a local time.
	yearly,   ///< Every year on a month and day at a local time (holidays, anniversaries).
	dates,    ///< On each date of an explicit list at a local time.
	astro,    ///< On the given days at a sun event plus an offset, optionally clamped to a local window.
	interval, ///< On the given days every N minutes from a local start time up to an end time.
	once,     ///< Once, at a local date and time.
};

/// "weekly", "yearly", "dates", "astro", "interval", "once".
const char* schedule_kind_name(ScheduleKind kind);

/// What happens to a firing that fell into a gap (the controller was off, the clock jumped over it).
enum class MissedPolicy : uint8_t {
	skip,     ///< Logged and dropped.
	catch_up, ///< Logged and fired once as soon as the gap is noticed.
};

/// "skip", "catchUp".
const char* missed_policy_name(MissedPolicy policy);

/// A local time of day, second resolution.
struct LocalClockTime {
	uint8_t hour = 0;   ///< 0..23
	uint8_t minute = 0; ///< 0..59
	uint8_t second = 0; ///< 0..59
};

/// Days of the week as bits: bit 0 Monday .. bit 6 Sunday.
struct Weekdays {
	static constexpr uint8_t kMonday = 0x01;
	static constexpr uint8_t kSunday = 0x40;
	static constexpr uint8_t kAll = 0x7F;

	/// The bit of a day 1 (Monday) .. 7 (Sunday).
	static constexpr uint8_t bit(unsigned weekday) { return static_cast<uint8_t>(1u << (weekday - 1)); }
};

/// The members of an entry that the image may declare adjustable (spec/schedules.md 2.1, issue
/// #58), as bits of ScheduleSpec::adjustable.
struct ScheduleMember {
	static constexpr uint16_t kTime = 0x001;      ///< weekly, yearly, dates: `time`.
	static constexpr uint16_t kDays = 0x002;      ///< weekly, astro, interval: `days`.
	static constexpr uint16_t kOffset = 0x004;    ///< astro: `offset`.
	static constexpr uint16_t kNotBefore = 0x008; ///< astro: `notBefore`.
	static constexpr uint16_t kNotAfter = 0x010;  ///< astro: `notAfter`.
	static constexpr uint16_t kEvery = 0x020;     ///< interval: `every`.
	static constexpr uint16_t kFrom = 0x040;      ///< interval: `from`.
	static constexpr uint16_t kUntil = 0x080;     ///< interval: `until`.
	static constexpr uint16_t kAt = 0x100;        ///< once: `at`.
	static constexpr size_t kCount = 9;

	/// The bit of member number `i` (0..kCount-1), in the order above.
	static constexpr uint16_t bit(size_t i) { return static_cast<uint16_t>(1u << i); }

	/// The image's name of a single member bit ("time", "days", "offset", "notBefore", "notAfter",
	/// "every", "from", "until", "at"); "?" for anything else.
	static const char* name(uint16_t member);

	/// The bit of a member name; 0 for an unknown name.
	static uint16_t of_name(const char* name, size_t length);

	/// The members an entry of `kind` may declare adjustable.
	static uint16_t allowed(ScheduleKind kind);

	/// Whether the member is a time of day (time, notBefore, notAfter, from, until).
	static bool is_time(uint16_t member) {
		return (member & (kTime | kNotBefore | kNotAfter | kFrom | kUntil)) != 0;
	}
};

/// The bounds of an adjustable member (spec/schedules.md 2.1), in the member's *scalar*: a time of
/// day as seconds since midnight (0..86399), `offset` and `every` in seconds, `at` as local seconds
/// since 1970-01-01 00:00 (days_from_civil() * 86400 + seconds of the day). `days` has no bounds.
struct ScheduleBound {
	uint16_t member = 0; ///< One ScheduleMember bit.
	int64_t min = 0;     ///< Inclusive.
	int64_t max = 0;     ///< Inclusive, >= min.
	/// The value must be a multiple of it (times: seconds since midnight; durations: seconds); 0
	/// for none. Not for `at`.
	int64_t step = 0;
};

/// One entry as the runtime image declares it (spec/schedules.md); the image loader (issue #8)
/// fills it from JSON. Only the members of the entry's kind are read. Scheduler::load() copies
/// everything; the spec need not outlive the call.
struct ScheduleSpec {
	const char* id = nullptr;    ///< 1..32 characters of A-Z a-z 0-9 _ -, unique in the image.
	const char* label = nullptr; ///< 1..63 bytes, printable.
	ScheduleKind kind = ScheduleKind::weekly;
	bool enabled = true;            ///< Default of the enable switch.
	bool enabled_adjustable = true; ///< Whether the inhabitant may switch the entry on and off.
	MissedPolicy missed = MissedPolicy::skip;
	/// Boolean datapoint the scheduler owns and sets true at each firing and false at local
	/// midnight ("fired today"); nullptr for none.
	const char* output = nullptr;

	uint8_t days = Weekdays::kAll; ///< weekly, astro, interval: Weekdays bits, not empty.
	LocalClockTime time;           ///< weekly, yearly, dates: the local time of the firing.
	uint8_t month = 0;             ///< yearly: 1..12.
	uint8_t day = 0;               ///< yearly: 1..days of the month (29 February fires in leap years).
	const Date* dates = nullptr;   ///< dates: valid dates, any order, no duplicates.
	size_t date_count = 0;         ///< dates: 1..kMaxDates.

	SunEvent sun = SunEvent::sunrise; ///< astro: the anchor.
	int32_t offset_s = 0;             ///< astro: signed offset from the anchor, |offset| <= 12 h.
	bool has_not_before = false;      ///< astro: clamp the firing to not earlier than not_before.
	LocalClockTime not_before;
	bool has_not_after = false;       ///< astro: clamp the firing to not later than not_after.
	LocalClockTime not_after;

	uint16_t every_minutes = 0; ///< interval: 1..720.
	LocalClockTime from;        ///< interval: the first slot of a day.
	LocalClockTime until;       ///< interval: the last slot is the latest from + k * every <= until (until >= from).

	Date at_date;           ///< once: the local date.
	LocalClockTime at_time; ///< once: the local time.

	/// ScheduleMember bits the inhabitant may change (issue #58); a subset of
	/// ScheduleMember::allowed(kind).
	uint16_t adjustable = 0;
	/// Bounds of adjustable members, at most one per member; nullptr for none.
	const ScheduleBound* bounds = nullptr;
	size_t bound_count = 0; ///< 0..ScheduleMember::kCount.
};

/// The scalar of a member of `spec` (ScheduleBound): seconds of the day for a time, seconds for
/// `offset` and `every`, the Weekdays bits for `days`, local seconds since 1970 for `at`.
int64_t schedule_member_value(const ScheduleSpec& spec, uint16_t member);

/// Sets a member of `spec` from its scalar (no validation).
void set_schedule_member_value(ScheduleSpec& spec, uint16_t member, int64_t value);

/// Checks the adjustable members and bounds of a spec whose other members are valid (the image
/// loader and Scheduler::load() share it): every adjustable member allowed for the kind, an
/// adjustable `notBefore`/`notAfter` set in the image, bounds only for adjustable members other
/// than `days`, at most one per member, `min <= max` within the member's range, a positive step
/// (whole minutes for `every`, none for `at`), and the image's own value within its bounds and on
/// the step. nullptr when valid, else the reason (a static text, without the reason code
/// `malformed`).
const char* schedule_adjustable_error(const ScheduleSpec& spec);

/// The name of the datapoint that mirrors the adjustable `time` of an entry (spec/schedules.md
/// 2.2): `schedule/<id>/time`. False if it does not fit `size` (Datapoint::kMaxName + 1 always fits
/// a valid id).
bool schedule_time_datapoint_name(const char* id, char* out, size_t size);

/// The prefix of the datapoints the schedules imply; a declared datapoint may not start with it.
constexpr const char* kScheduleDatapointPrefix = "schedule/";

/// Whether an entry of `kind` with these adjustable members has a time datapoint.
inline bool schedule_has_time_datapoint(ScheduleKind kind, uint16_t adjustable) {
	return (adjustable & ScheduleMember::kTime) != 0 &&
		(kind == ScheduleKind::weekly || kind == ScheduleKind::yearly || kind == ScheduleKind::dates);
}

/// Outcome of a change of adjustable members (Scheduler::set_members()). The names are those of
/// the API's refusals (spec/api.md, `schedule.<name>`).
enum class ScheduleChange : uint8_t {
	ok,
	unknown,        ///< No entry with this id.
	not_adjustable, ///< The image does not let the inhabitant change the member.
	out_of_bounds,  ///< Outside the member's bounds, or not on its step.
	malformed,      ///< Not a value of the member (out of its range, an empty days set), or the
	                ///< members contradict each other (`from` after `until`, `notBefore` after `notAfter`).
};

/// "ok", "unknown", "not_adjustable", "out_of_bounds", "malformed".
const char* schedule_change_name(ScheduleChange change);

/// Outcome of Scheduler::load(). Everything except `ok` refuses the whole load (the previous
/// entries stay) and is logged naming the entry. The names are the reasons of spec/rules.md 10.1.
enum class ScheduleStatus : uint8_t {
	ok,
	malformed,         ///< A member is missing or out of its range (the log says which).
	duplicate_id,      ///< Two entries with the same id, or a date listed twice.
	unknown_reference, ///< The output datapoint does not exist.
	type_error,        ///< The output datapoint is not boolean.
	output_conflict,   ///< The output datapoint is owned by a technology, or named by two entries.
	limit_exceeded,    ///< More than kMaxEntries entries or kMaxDates dates.
	out_of_memory,     ///< The entry table could not be allocated.
	busy,              ///< Called from within a listener's schedule_fired().
};

/// "ok", "malformed", "duplicateId", "unknownReference", "typeError", "outputConflict",
/// "limitExceeded", "outOfMemory", "busy".
const char* schedule_status_name(ScheduleStatus status);

/// Receives the firings. Called on the event loop's thread from the scheduler's timer, once per
/// firing, in the order of the entries' firing times (entries due at the same time in image order).
/// A listener may read the scheduler but must not load, clear or (un)register from within the call
/// (refused and logged).
class ScheduleListener {
public:
	virtual void schedule_fired(const char* id) = 0;

protected:
	~ScheduleListener() = default;
};

/// Runs the schedule entries of the runtime image (spec/schedules.md, doc/time.md): computes each
/// entry's next firing in local time from the TimeSource, keeps one event-loop timer for the
/// earliest, fires the due entries to its listeners, handles missed firings by the entry's policy,
/// keeps the "fired today" output datapoints, and remembers each entry's last firing and enable
/// override in the settings (one key per entry, "sch." + 8 hex digits of the id's FNV-1a hash).
///
/// It does nothing while the wall clock is not synchronized. It re-evaluates every entry on load
/// and on every time event: register it with TimeService::add_listener() (all events).
///
/// The image may declare members of an entry adjustable (`adjustable`, `bounds`; issue #58): the
/// inhabitant's values are overrides in the entry's record, kept by a new image while the entry
/// exists, the member is adjustable and the value fits the new bounds, dropped (logged) otherwise.
/// An adjustable `time` is also the datapoint `schedule/<id>/time`, owned by the scheduler.
///
/// Memory: load() allocates two blocks, the entry table and one for the entries' bounds, dates and
/// strings; nothing is allocated afterwards.
class Scheduler : public TimeListener, public Technology {
public:
	static constexpr size_t kMaxEntries = 128;
	static constexpr size_t kMaxDates = 128;
	static constexpr size_t kMaxId = 32;
	static constexpr size_t kMaxLabel = 63;
	static constexpr size_t kListenerCapacity = 4;

	/// A firing up to this late is on time; an older due firing was missed.
	static constexpr int64_t kGraceS = 60;

	/// Longest timer the scheduler sets: it re-reads the wall clock at least this often, so that a
	/// slow drift of the wall clock against the monotonic clock cannot delay a firing.
	static constexpr uint32_t kMaxTimerMs = 600000;

	/// How far ahead the search for a next firing looks for the kinds that recur within a week
	/// (weekly, astro, interval); an astro entry in a polar night may have none, it is searched
	/// again at the next day change.
	static constexpr int kSearchDays = 8;

	/// `model` (optional) holds the output datapoints; `settings` the persisted records.
	Scheduler(EventLoop& loop, const TimeSource& time, Settings& settings, DatapointModel* model);
	~Scheduler();

	Scheduler(const Scheduler&) = delete;
	Scheduler& operator=(const Scheduler&) = delete;

	/// Replaces the entries by `count` new ones. Validates everything first; a refusal keeps the
	/// previous entries. Restores each entry's persisted record, deletes the records of ids no
	/// longer present, binds the outputs and evaluates (missed firings are fired or logged from the
	/// timer, never from within load()).
	ScheduleStatus load(const ScheduleSpec* specs, size_t count);

	/// Removes all entries (their records stay, for a following load).
	void clear();

	/// With true, load() binds the time datapoints (`schedule/<id>/time`) but leaves their values to
	/// publish_times(), so that the caller can make them no events for the rules (the image service:
	/// taking the values at load is no event, as for the rule parameters' datapoints). Default false:
	/// load() publishes them.
	void defer_time_reports(bool defer) { defer_times_ = defer; }

	/// Reports the effective time of every entry with a bound time datapoint where the datapoint
	/// differs.
	void publish_times();

	size_t size() const { return count_; }

	/// The index of the entry with `id`; empty if there is none.
	std::optional<size_t> find(const char* id) const;

	const char* id(size_t index) const;
	const char* label(size_t index) const;
	ScheduleKind kind(size_t index) const;
	bool enabled(size_t index) const;

	/// The next firing of an entry (UTC seconds), as currently scheduled; empty when it has none
	/// (disabled, a past one-shot, astronomy unavailable, clock not synchronized).
	std::optional<int64_t> next_firing(size_t index) const;

	/// The first firing of an entry strictly after `utc` (UTC seconds) by its calendar alone
	/// (whether enabled or not); empty when there is none within the search horizon. For the UI's
	/// preview.
	std::optional<int64_t> next_after(size_t index, int64_t utc) const;

	/// The UTC time of the entry's last firing, also from before a reboot; empty if unknown.
	std::optional<int64_t> last_fired(size_t index) const;

	/// Switches an entry on or off like the UI does. Refused (false, logged, `error` set) for an
	/// unknown id or an entry whose switch is not adjustable. The override is persisted; switching
	/// back to the image's default removes it. Firings that passed while the entry was off are not
	/// missed firings.
	bool set_enabled(const char* id, bool enabled, const char** error = nullptr);

	/// Whether the inhabitant may switch the entry on and off (the image's `enabledAdjustable`).
	bool enabled_adjustable(size_t index) const;
	/// Whether the enable switch differs from the image's `enabled` (an override).
	bool enabled_overridden(size_t index) const;

	/// The entry with its effective values (the image's, with the inhabitant's overrides); the
	/// pointers stay valid until the next load().
	const ScheduleSpec& spec(size_t index) const;
	/// The entry as the image declares it (no overrides).
	ScheduleSpec image_spec(size_t index) const;
	/// The ScheduleMember bits the image declares adjustable.
	uint16_t adjustable(size_t index) const;
	/// The ScheduleMember bits whose effective value is an override.
	uint16_t overridden(size_t index) const;
	/// The bounds of an adjustable member; nullptr when it has none.
	const ScheduleBound* bound(size_t index, uint16_t member) const;

	/// Changes adjustable members of an entry like the UI does (spec/schedules.md 4.2): `members`
	/// (ScheduleMember bits) are taken from `values`, all or none. A value equal to the image's
	/// removes the member's override; any other value becomes the override, persisted by schedule id
	/// and member. The entry is evaluated at once with the new values; a change is not a gap, and a
	/// day's firing that was handled stays handled (4.3). On a refusal (logged) `culprit` names the
	/// member and `reason` the why.
	ScheduleChange set_members(const char* id, uint16_t members, const ScheduleSpec& values,
		uint16_t* culprit = nullptr, const char** reason = nullptr);

	/// Returns the entry to the image's values: every member override and the enable override are
	/// removed (with the change semantics of set_members()). Refused (false, logged) for an unknown
	/// id.
	bool reset_overrides(const char* id, const char** reason = nullptr);

	bool add_listener(ScheduleListener& listener);
	bool remove_listener(ScheduleListener& listener);

	/// Re-evaluates all entries now (what every time event does).
	void evaluate();

	// TimeListener
	void time_changed(uint8_t events) override;

	// Technology: the scheduler owns the output datapoints, whose commands it refuses, and the
	// time datapoints `schedule/<id>/time`, whose commands set the entry's time (set_members()).
	const char* name() const override { return "scheduler"; }
	bool command(const Datapoint& datapoint, const Value& value) override;

	/// The settings key of an entry id ("sch." + 8 lower-case hex digits), into `buffer` of at least
	/// kRecordKeySize bytes.
	static constexpr size_t kRecordKeySize = 13;
	static void record_key(const char* id, char* buffer);

private:
	struct Entry;

	ScheduleStatus validate(const ScheduleSpec& spec, size_t index, size_t& dates) const;
	bool restore_overrides(Entry& entry, const char* text);
	void drop_override(Entry& entry, uint16_t member, const char* why);
	ScheduleChange check_member(const Entry& entry, uint16_t member, const ScheduleSpec& values,
		const char** reason) const;
	void apply_change(Entry& entry, const ScheduleSpec& before, bool from_command);
	void account_change(Entry& entry, const ScheduleSpec& before);
	std::optional<int64_t> firing_on(const Entry& entry, const Date& date) const;
	void report_time(Entry& entry);
	void evaluate_soon();
	std::optional<int64_t> occurrence_after(const Entry& entry, int64_t utc) const;
	std::optional<int64_t> occurrence_on(const Entry& entry, const Date& date) const;
	std::optional<int64_t> interval_after(const Entry& entry, const Date& date, int64_t utc) const;
	std::optional<int64_t> latest_up_to(const Entry& entry, int64_t first, int64_t now) const;
	void restore(Entry& entry);
	void persist(Entry& entry);
	void delete_orphan_records();
	void bind_outputs();
	void update_outputs(int64_t now);
	void fire_due();
	void idle(Entry& entry, int64_t now);
	static bool is_idle(const Entry& entry);
	void arm(int64_t now);
	void on_timer();
	void fire(Entry& entry, int64_t now, bool caught_up);
	void format_local(int64_t utc, char* buffer, size_t size) const;

	EventLoop& loop_;
	const TimeSource& time_;
	Settings& settings_;
	DatapointModel* model_;
	Entry* entries_ = nullptr;
	char* block_ = nullptr;
	size_t count_ = 0;
	ScheduleListener* listeners_[kListenerCapacity] = {};
	size_t listener_count_ = 0;
	TimerHandle timer_;
	bool notifying_ = false;
	bool firing_ = false;
	bool waiting_logged_ = false;
	bool commanding_ = false; ///< Within command(): the model stores the value, nothing may report.
	bool defer_times_ = false;
};

} // namespace hc

#endif // HC_SCHEDULER_H
