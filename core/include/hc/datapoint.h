/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_DATAPOINT_H
#define HC_DATAPOINT_H

#include <cstddef>
#include <cstdint>
#include <optional>

#include "hc/platform/clock.h"

namespace hc {

class Technology;

/// The kind of value a datapoint holds. Technology neutral: a KNX datapoint type, a Zigbee cluster
/// attribute or a rule parameter all map onto one of these (doc/datapoints.md).
enum class ValueType : uint8_t {
	none,         ///< No value (an empty Value; never the type of a datapoint).
	boolean,
	unsigned_int, ///< uint32_t
	signed_int,   ///< int32_t
	real,         ///< double
	string,       ///< UTF-8, at most Value::kMaxString bytes.
	time_of_day,  ///< TimeOfDay
	date,         ///< Date
	date_time,    ///< DateTime
	enumeration,  ///< One element of an enumeration, by its number (uint32_t).
	raw,          ///< Bytes, at most Value::kMaxRaw.
	controlled,   ///< Controlled: a control bit with a small number (KNX DPT 2, 3, 18).
	color,        ///< Color: red, green, blue, white levels.
};

/// "none", "boolean", "unsigned", "signed", "real", "string", "time of day", "date", "date-time",
/// "enumeration", "raw", "controlled", "color".
const char* value_type_name(ValueType type);

/// A time of day with an optional day of the week.
struct TimeOfDay {
	uint8_t day_of_week = 0; ///< 0: no day, 1: Monday ... 7: Sunday.
	uint8_t hour = 0;        ///< 0..23
	uint8_t minute = 0;      ///< 0..59
	uint8_t second = 0;      ///< 0..59

	friend bool operator==(const TimeOfDay& a, const TimeOfDay& b) {
		return a.day_of_week == b.day_of_week && a.hour == b.hour && a.minute == b.minute && a.second == b.second;
	}
};

/// A calendar date.
struct Date {
	uint16_t year = 0;
	uint8_t month = 0; ///< 1..12
	uint8_t day = 0;   ///< 1..31

	friend bool operator==(const Date& a, const Date& b) {
		return a.year == b.year && a.month == b.month && a.day == b.day;
	}
};

/// A date with a time of day and the status flags of the clock that sent it (KNX DPT 19). The flag
/// bits are those of the KNX encoding: the low byte is octet 2 of the DPT, the high byte octet 1.
struct DateTime {
	static constexpr uint16_t kSummerTime = 0x0001;    ///< SUTI: summer time in effect.
	static constexpr uint16_t kNoTime = 0x0002;        ///< NT: hour, minute, second are not valid.
	static constexpr uint16_t kNoDayOfWeek = 0x0004;   ///< NDOW: day_of_week is not valid.
	static constexpr uint16_t kNoDate = 0x0008;        ///< ND: month and day are not valid.
	static constexpr uint16_t kNoYear = 0x0010;        ///< NY: year is not valid.
	static constexpr uint16_t kNoWorkingDay = 0x0020;  ///< NWD: kWorkingDay is not valid.
	static constexpr uint16_t kWorkingDay = 0x0040;    ///< WD: a working day.
	static constexpr uint16_t kFault = 0x0080;         ///< F: the clock is faulty.
	static constexpr uint16_t kSyncReliable = 0x4000;  ///< SRC: the synchronisation source is reliable.
	static constexpr uint16_t kClockQuality = 0x8000;  ///< CLQ: clock with an external sync signal.
	static constexpr uint16_t kAllFlags = 0xC0FF;

	uint16_t year = 1900;    ///< 1900..2155
	uint8_t month = 1;       ///< 1..12
	uint8_t day = 1;         ///< 1..31
	uint8_t day_of_week = 0; ///< 0: any day, 1: Monday ... 7: Sunday.
	uint8_t hour = 0;        ///< 0..24 (24 only as 24:00:00).
	uint8_t minute = 0;      ///< 0..59
	uint8_t second = 0;      ///< 0..59
	uint16_t flags = 0;

	friend bool operator==(const DateTime& a, const DateTime& b) {
		return a.year == b.year && a.month == b.month && a.day == b.day && a.day_of_week == b.day_of_week &&
			a.hour == b.hour && a.minute == b.minute && a.second == b.second && a.flags == b.flags;
	}
};

/// A control bit with a small number: KNX DPT 2 (control, value 0/1), DPT 3 (direction, step code
/// 0..7, 0 is a stop) and DPT 18 (learn, scene number 0..63).
struct Controlled {
	bool control = false;
	uint8_t value = 0;

	friend bool operator==(const Controlled& a, const Controlled& b) {
		return a.control == b.control && a.value == b.value;
	}
};

/// A color as channel levels 0..255 (255 is full intensity, 100 %), with the channels that carry a
/// valid level (KNX DPT 251 transmits a validity bit per channel; DPT 232 has no white).
struct Color {
	static constexpr uint8_t kWhite = 0x01;
	static constexpr uint8_t kBlue = 0x02;
	static constexpr uint8_t kGreen = 0x04;
	static constexpr uint8_t kRed = 0x08;
	static constexpr uint8_t kRgb = kRed | kGreen | kBlue;
	static constexpr uint8_t kAll = kRgb | kWhite;

	uint8_t red = 0;
	uint8_t green = 0;
	uint8_t blue = 0;
	uint8_t white = 0;
	uint8_t valid = kRgb;

	friend bool operator==(const Color& a, const Color& b) {
		return a.red == b.red && a.green == b.green && a.blue == b.blue && a.white == b.white &&
			a.valid == b.valid;
	}
};

/// A typed value: the current value of a datapoint, a value to command, a decoded telegram. A
/// fixed-size object of 32 bytes (no heap): strings and raw bytes are stored inline. A
/// default-constructed Value has the type none.
class Value {
public:
	/// Longest string in bytes (UTF-8, without the terminating NUL): 14 Latin-1 characters of KNX
	/// DPT 16 take up to 28 bytes in UTF-8.
	static constexpr size_t kMaxString = 28;

	/// Longest raw value in bytes.
	static constexpr size_t kMaxRaw = 14;

	Value() = default;

	static Value of_bool(bool value);
	static Value of_unsigned(uint32_t value);
	static Value of_signed(int32_t value);
	/// A real number. NaN and infinities are values of no type (none): no datapoint holds them.
	static Value of_real(double value);
	static Value of_enum(uint32_t element);
	static Value of_time(const TimeOfDay& value);
	static Value of_date(const Date& value);
	static Value of_date_time(const DateTime& value);
	static Value of_controlled(bool control, uint8_t value);
	static Value of_color(const Color& value);

	/// A string of `length` bytes (UTF-8 is not validated here; the codecs do). Empty if it is
	/// longer than kMaxString or contains a NUL byte.
	static std::optional<Value> of_string(const char* text, size_t length);
	static std::optional<Value> of_string(const char* text);

	/// Raw bytes; empty if longer than kMaxRaw.
	static std::optional<Value> of_raw(const uint8_t* bytes, size_t length);

	ValueType type() const { return type_; }
	bool empty() const { return type_ == ValueType::none; }

	/// The accessors return the value if the type matches and a zero value otherwise.
	bool as_bool() const;
	uint32_t as_unsigned() const;
	int32_t as_signed() const;
	double as_real() const;
	uint32_t as_enum() const;
	TimeOfDay as_time() const;
	Date as_date() const;
	DateTime as_date_time() const;
	Controlled as_controlled() const;
	Color as_color() const;
	/// The NUL-terminated string ("" for another type).
	const char* as_string() const;
	/// Length of a string or raw value in bytes (0 for another type).
	size_t length() const { return length_; }
	/// The raw bytes (nullptr for another type).
	const uint8_t* raw() const;

	/// This value as `type`, where the conversion is exact: between unsigned, signed, real and
	/// enumeration when the number is representable (a real only when it is integral), and to the
	/// same type. Empty otherwise.
	std::optional<Value> convert(ValueType type) const;

	/// Formats the value for the log ("21.5", "on", "13:45:30 Tue", "\"text\""); truncates to
	/// `size`. Returns `buffer`.
	const char* format(char* buffer, size_t size) const;

	/// Equal type and equal content (reals compare numerically).
	friend bool operator==(const Value& a, const Value& b);
	friend bool operator!=(const Value& a, const Value& b) { return !(a == b); }

private:
	static constexpr size_t kStorage = 30;

	template <class T>
	static Value make(ValueType type, const T& value);
	template <class T>
	T get(ValueType type) const;

	ValueType type_ = ValueType::none;
	uint8_t length_ = 0;
	uint8_t storage_[kStorage] = {};
};

static_assert(sizeof(Value) == 32, "a Value is 32 bytes");

/// How far a datapoint's current value can be trusted.
enum class Quality : uint8_t {
	unknown, ///< No value received or commanded yet.
	good,    ///< The last update came from the technology or the application.
	stale,   ///< The technology lost contact (bus down, device gone); the value is the last known.
};

/// "unknown", "good", "stale".
const char* quality_name(Quality quality);

/// Flags of a datapoint (bit mask).
struct DatapointFlags {
	/// The technology answers read requests on its side with the current value (KNX: R flag).
	static constexpr uint8_t kReadable = 0x01;
	/// The application may command the datapoint; commands to others are refused.
	static constexpr uint8_t kWritable = 0x02;
	/// Commands are passed to the owning technology to transmit (KNX: T flag); without it a command
	/// only changes the value in the model.
	static constexpr uint8_t kTransmit = 0x04;
	/// The technology reads the value when it starts (KNX: I flag).
	static constexpr uint8_t kReadOnStart = 0x08;
	/// The last good value and its wall-clock time survive a reboot: hc::ValueStore saves it (write
	/// coalescing, a flash budget) and restores it after the load with quality stale
	/// (doc/persistence.md).
	static constexpr uint8_t kPersist = 0x10;
	/// hc::History keeps a ring of recent samples for the UI's sparklines; the retention and the
	/// sample spacing come with the image's history spec (doc/persistence.md). Numeric and boolean
	/// datapoints only.
	static constexpr uint8_t kHistory = 0x20;
	/// A value of the application: the image declares it `virtual` or without a technology member
	/// (spec/runtime-image.md 3.3, schema 1.6). Rules may command it also without kWritable, which is
	/// the permission of the user (the UI, the API, MQTT); a rule's write goes through
	/// DatapointModel::command_by_rule().
	static constexpr uint8_t kApplication = 0x40;
	static constexpr uint8_t kAll = 0x7F;
};

/// One typed value of the house: a KNX group object, a Zigbee attribute, a rule parameter
/// (ROADMAP: everything is a datapoint). Owned by a DatapointModel; read it there, change it with
/// DatapointModel::report() (the technology) or DatapointModel::command() (the application).
class Datapoint {
public:
	/// Longest name in bytes, without the terminating NUL.
	static constexpr size_t kMaxName = 63;

	/// The stable id: the datapoint's index in the model it was loaded into.
	uint16_t id() const { return id_; }
	const char* name() const { return name_; }
	ValueType type() const { return type_; }
	uint8_t flags() const { return flags_; }
	bool readable() const { return (flags_ & DatapointFlags::kReadable) != 0; }
	bool writable() const { return (flags_ & DatapointFlags::kWritable) != 0; }
	bool transmits() const { return (flags_ & DatapointFlags::kTransmit) != 0; }
	bool read_on_start() const { return (flags_ & DatapointFlags::kReadOnStart) != 0; }
	bool persists() const { return (flags_ & DatapointFlags::kPersist) != 0; }
	bool has_history() const { return (flags_ & DatapointFlags::kHistory) != 0; }
	bool application() const { return (flags_ & DatapointFlags::kApplication) != 0; }

	/// The current value; of type none while the quality is unknown.
	const Value& value() const { return value_; }
	Quality quality() const { return quality_; }
	/// Monotonic time of the last value update (report or command); 0 before the first (also for a
	/// value restored from before the reboot: hc::ValueStore knows its wall-clock time).
	uint64_t updated_ms() const { return updated_ms_; }
	/// The technology that owns the datapoint; nullptr for a datapoint of the application alone (a
	/// rule parameter, a virtual value).
	Technology* technology() const { return technology_; }
	/// The technology that transmits what the owner publishes (DatapointModel::set_transmitter());
	/// nullptr when the owner transmits itself or nothing is transmitted.
	Technology* transmitter() const { return transmitter_; }
	/// Whether the owning technology holds a commanded value it has not sent yet, because it is not
	/// connected (homecontrol #72: the KNX tunnel is down, the Zigbee network is not up). The value is
	/// value(); the technology sends it once it connects. Always false without an owner.
	bool pending() const { return pending_ && technology_ != nullptr; }

private:
	friend class DatapointModel;

	Value value_;
	uint64_t updated_ms_ = 0;
	const char* name_ = "";
	Technology* technology_ = nullptr;
	Technology* transmitter_ = nullptr;
	uint16_t id_ = 0;
	ValueType type_ = ValueType::none;
	uint8_t flags_ = 0;
	Quality quality_ = Quality::unknown;
	bool pending_ = false;
};

/// What a model load declares about one datapoint.
struct DatapointSpec {
	const char* name;  ///< 1..Datapoint::kMaxName bytes, printable, unique in the model.
	ValueType type;    ///< Not none.
	uint8_t flags;     ///< DatapointFlags.
};

/// A datapoint as a compiler of rules and blocks sees it: its id, name, type and flags.
struct DatapointRef {
	uint16_t id_ = 0;
	const char* name_ = "";
	ValueType type_ = ValueType::none;
	uint8_t flags_ = 0;
	uint16_t id() const { return id_; }
	const char* name() const { return name_; }
	ValueType type() const { return type_; }
	uint8_t flags() const { return flags_; }
	bool writable() const { return (flags_ & DatapointFlags::kWritable) != 0; }
	/// Whether a rule may write it: writable, or a value of the application (kApplication, 1.6).
	bool rule_writable() const { return (flags_ & (DatapointFlags::kWritable | DatapointFlags::kApplication)) != 0; }
};

/// Looks datapoints up by name and id for the compilers of rules and blocks (homecontrol #70): the
/// device's model (ModelDirectory), or the image loader's tables before any model is built, so the
/// loader needs no scratch model of the image.
class DatapointDirectory {
public:
	virtual bool lookup(const char* name, DatapointRef& out) const = 0;
	virtual bool lookup(uint16_t id, DatapointRef& out) const = 0;

protected:
	~DatapointDirectory() = default;
};

/// Where an update came from.
enum class UpdateOrigin : uint8_t {
	report,  ///< The technology reported a value.
	command, ///< The application commanded a value.
	stale,   ///< The technology reported the value stale (value unchanged).
	publish, ///< The owner published a value of its own (sent through the transmitter, if any).
};

/// "report", "command", "stale", "publish".
const char* update_origin_name(UpdateOrigin origin);

/// One update a DatapointListener is told about.
struct DatapointUpdate {
	const Datapoint& datapoint; ///< Carries the new value, quality and time.
	const Value& old_value;
	Quality old_quality;
	UpdateOrigin origin;

	/// Whether value or quality differ from before (a repeated report of the same value does not).
	bool changed() const { return old_value != datapoint.value() || old_quality != datapoint.quality(); }
};

/// Observes the datapoints of a model: rules, the UI, logging. Called on the event loop's thread
/// right after every update, in registration order. A listener may read the model but must not
/// change it (report, command, load and listener changes are refused and logged while listeners
/// run); post() to the event loop to react with a change.
class DatapointListener {
public:
	virtual void datapoint_updated(const DatapointUpdate& update) = 0;

	/// Datapoint::pending() changed without an update (the technology sent the held value, or a
	/// configuration dropped it). A change that comes with an update (a command the technology holds,
	/// a report) is told through datapoint_updated() alone. The same rules as datapoint_updated().
	virtual void datapoint_pending(const Datapoint& datapoint) { (void) datapoint; }

protected:
	~DatapointListener() = default;
};

/// Outcome of a model call. Everything except `ok` is a refusal, which is also logged (tag "hc.dp");
/// datapoint_status_name() gives the text.
enum class DatapointStatus : uint8_t {
	ok,
	unknown_id,      ///< No datapoint with this id in the model.
	wrong_type,      ///< The value does not convert to the datapoint's type.
	not_writable,    ///< Command to a datapoint without kWritable.
	not_owner,       ///< Report or publish from a technology that does not own the datapoint, bound twice,
	                 ///< or a transmitter refused (another one is set, no owner yet).
	refused,         ///< The owning technology refused the command (it logged why).
	busy,            ///< Called from a listener while listeners run.
	invalid_model,   ///< Load refused: a spec is invalid (the log names it).
	out_of_memory,   ///< Load refused: the tables could not be allocated.
	capacity,        ///< Listener capacity exhausted or model larger than kMaxDatapoints.
};

/// "ok", "unknown id", "wrong type", ...
const char* datapoint_status_name(DatapointStatus status);

/// The table of all datapoints of the loaded runtime image, and the two ways a value changes:
///
/// - report(): the technology says what the value is (a telegram arrived, a device reported).
/// - command(): the application (a rule, the UI) wants a value. The owning technology receives it
///   through Technology::command() to transmit (if the datapoint has kTransmit); when it accepts,
///   the model takes the value at once.
///
/// Both store the value with quality good and the current monotonic time, then tell every listener.
/// Memory: load() allocates the table once (one block for the datapoints, one for the names and
/// the name index); nothing is allocated afterwards. doc/datapoints.md has the budget.
///
/// Threading: every method is called on the event loop's thread.
class DatapointModel {
public:
	/// Most datapoints in one model (ids are uint16_t).
	static constexpr size_t kMaxDatapoints = 65535;

	/// Most listeners registered at the same time.
	static constexpr size_t kListenerCapacity = 8;

	explicit DatapointModel(const Clock& clock);
	~DatapointModel();

	DatapointModel(const DatapointModel&) = delete;
	DatapointModel& operator=(const DatapointModel&) = delete;

	/// Replaces the model by `count` datapoints with the ids 0..count-1 in the order of `specs`
	/// (values unknown, no technology bound). Validates every spec before anything changes: on a
	/// refusal the previous model stays. Datapoint pointers and bindings of the previous model are
	/// invalid afterwards; listeners stay registered.
	DatapointStatus load(const DatapointSpec* specs, size_t count) { return load(specs, count, false); }

	/// As load(); with `keep_values` a datapoint whose name and value type are those of a datapoint
	/// of the previous model keeps its value, quality and update time (the runtime image swap,
	/// spec/rules.md 7.6), and its Datapoint::pending() mark (a held command stays held when the new
	/// owner holds commands, Technology::holds_commands()). Nothing is announced to the listeners:
	/// the values did not change.
	DatapointStatus load(const DatapointSpec* specs, size_t count, bool keep_values);

	/// The checks load() makes of each spec (name, type, flags; logged), without the duplicate names
	/// and without allocating: ok, capacity or invalid_model.
	static DatapointStatus validate(const DatapointSpec* specs, size_t count);

	/// Removes all datapoints and frees the tables.
	void clear();

	size_t size() const { return count_; }

	/// The datapoint with `id`, nullptr if there is none.
	const Datapoint* get(uint16_t id) const;

	/// The datapoint named `name`, nullptr if there is none (binary search).
	const Datapoint* find(const char* name) const;

	/// Makes `technology` the owner of the datapoint: it receives its commands and may report it.
	/// Refused (not_owner) if another technology owns it already. Drops a pending mark kept by the
	/// load unless the technology holds commands.
	DatapointStatus bind(uint16_t id, Technology& technology);

	/// Makes `transmitter` the technology that transmits what the owner publishes (publish()): a
	/// datapoint owned by an application component (the KNX time master, later weather or virtual
	/// values) and sent to a bus by a technology (the KNX adapter). At most one per datapoint.
	/// Refused (not_owner, the reason in `error` and in the log) while the datapoint has no owner or
	/// when another transmitter is set. The owner itself is accepted and changes nothing (an owner
	/// that transmits does so in its own code). A load() or clear() drops it like the binding.
	DatapointStatus set_transmitter(uint16_t id, Technology& transmitter, const char** error = nullptr);

	/// The technology reports the value of a datapoint it owns. The value is converted to the
	/// datapoint's type where exact (Value::convert).
	DatapointStatus report(Technology& technology, uint16_t id, const Value& value);

	/// The owner publishes a value of its own (what the time master broadcasts). Only the owner may
	/// (not_owner); the value is converted like a report (wrong_type). If the datapoint transmits
	/// (kTransmit) and has a transmitter, the transmitter's Technology::command() receives the
	/// value first; when it refuses, publish() fails with `refused` and nothing is stored (the rule
	/// of command()). Then the value is stored with quality good and announced with
	/// UpdateOrigin::publish, so listeners can tell "we sent this" from "the bus said this". Does not
	/// need kWritable: that flag is about the application's commands, which still go to the owner.
	DatapointStatus publish(Technology& owner, uint16_t id, const Value& value);

	/// The technology lost contact: the value stays, the quality becomes stale (listeners are told
	/// if the quality was good).
	DatapointStatus report_stale(Technology& technology, uint16_t id);

	/// The owning technology marks the datapoint's value as held for sending (`pending` true: it is
	/// not connected; homecontrol #72) or as sent or dropped (false); Datapoint::pending(). Only the
	/// owner may (not_owner). Allowed inside the technology's command() (the mark of the value the
	/// model is about to store); listeners are told through DatapointListener::datapoint_pending()
	/// when the mark changes outside a command. A report() clears the mark: the value on the bus
	/// is newer than the held one.
	DatapointStatus set_pending(Technology& technology, uint16_t id, bool pending);

	/// Sets the last known value of a datapoint from before the reboot (hc::ValueStore, right after
	/// load()): the value (converted where exact) with quality stale, updated_ms() stays 0, and no
	/// listener is told (it is part of the load: rules start afterwards and see it through
	/// lastKnown()). Refused with wrong_type if it does not convert, and with busy unless the
	/// quality is still unknown (a value reported or commanded since the load always wins).
	DatapointStatus restore(uint16_t id, const Value& value);

	/// The application wants `value`. Refused if the datapoint is not writable or the value does not
	/// convert; passed to the owning technology if the datapoint transmits (refused if the technology
	/// refuses); then stored and announced like a report.
	DatapointStatus command(uint16_t id, const Value& value);

	/// A rule's write (spec/rules.md 7.2): as command(), but a value of the application
	/// (DatapointFlags::kApplication) that no technology owns takes it also when it is not writable;
	/// `writable` is the user's permission (spec/runtime-image.md 3.3, 1.6).
	DatapointStatus command_by_rule(uint16_t id, const Value& value);

	/// The application asks the owning technology to fetch the datapoint's current value
	/// (Technology::read(); a rule's `read` action). `ok` when the request went out (or was queued);
	/// the value arrives later as a report. Refused (logged) with `refused` when no technology owns
	/// the datapoint or the owner refuses (it logs why), `busy` while listeners run. Changes nothing
	/// in the model.
	DatapointStatus read(uint16_t id);

	/// Registers a listener (refused when full, for a duplicate and while listeners run).
	DatapointStatus add_listener(DatapointListener& listener);

	/// Unregisters a listener; false if it was not registered (refused while listeners run).
	bool remove_listener(DatapointListener& listener);

	/// Whether listeners are running now.
	bool notifying() const { return notifying_; }

private:
	DatapointStatus take_command(uint16_t id, const Value& value, bool by_rule);
	Datapoint* lookup(uint16_t id, const char* what);
	bool refuse_busy(const char* what);
	void store(Datapoint& datapoint, const Value& value, UpdateOrigin origin);
	void notify(const Datapoint& datapoint, const Value& old_value, Quality old_quality, UpdateOrigin origin);
	void free_tables();

	/// The datapoints in chunks of kChunk (homecontrol #70: no block of 64 bytes per datapoint in one
	/// piece, which a fragmented heap refuses during an image swap): datapoint `id` is
	/// chunks_[id / kChunk][id % kChunk].
	static constexpr size_t kChunkShift = 5;
	static constexpr size_t kChunk = size_t{1} << kChunkShift;
	Datapoint& at(size_t id) const { return chunks_[id >> kChunkShift][id & (kChunk - 1)]; }
	static void free_chunks(Datapoint** chunks, size_t count);

	const Clock& clock_;
	Datapoint** chunks_ = nullptr;
	char* names_ = nullptr;       ///< All names, NUL-separated (one allocation).
	uint16_t* by_name_ = nullptr; ///< Ids sorted by name (one allocation with names_).
	size_t count_ = 0;
	DatapointListener* listeners_[kListenerCapacity] = {};
	size_t listener_count_ = 0;
	bool notifying_ = false;
	bool commanding_ = false; ///< A Technology::command() call is running.
};

/// The device's model as a DatapointDirectory.
class ModelDirectory : public DatapointDirectory {
public:
	explicit ModelDirectory(const DatapointModel& model) : model_(model) {}
	bool lookup(const char* name, DatapointRef& out) const override { return fill(model_.find(name), out); }
	bool lookup(uint16_t id, DatapointRef& out) const override { return fill(model_.get(id), out); }

private:
	static bool fill(const Datapoint* dp, DatapointRef& out) {
		if (dp == nullptr) {
			return false;
		}
		out.id_ = dp->id();
		out.name_ = dp->name();
		out.type_ = dp->type();
		out.flags_ = dp->flags();
		return true;
	}
	const DatapointModel& model_;
};

} // namespace hc

#endif // HC_DATAPOINT_H
