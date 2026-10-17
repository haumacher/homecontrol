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
};

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
/// Memory: load() allocates two blocks, the entry table and one for the entries' strings and dates;
/// nothing is allocated afterwards.
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

	bool add_listener(ScheduleListener& listener);
	bool remove_listener(ScheduleListener& listener);

	/// Re-evaluates all entries now (what every time event does).
	void evaluate();

	// TimeListener
	void time_changed(uint8_t events) override;

	// Technology: the scheduler owns the output datapoints; commands to them are refused.
	const char* name() const override { return "scheduler"; }
	bool command(const Datapoint& datapoint, const Value& value) override;

	/// The settings key of an entry id ("sch." + 8 lower-case hex digits), into `buffer` of at least
	/// kRecordKeySize bytes.
	static constexpr size_t kRecordKeySize = 13;
	static void record_key(const char* id, char* buffer);

private:
	struct Entry;

	ScheduleStatus validate(const ScheduleSpec& spec, size_t index, size_t& dates) const;
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
};

} // namespace hc

#endif // HC_SCHEDULER_H
