/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/datapoint.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "hc/log.h"
#include "hc/technology.h"

namespace hc {

namespace {

constexpr const char* kTag = "hc.dp";

/// Whether a double holds an integer (32-bit integers convert to double exactly).
bool integral(double value) { return std::isfinite(value) && std::floor(value) == value; }

} // namespace

const char* value_type_name(ValueType type) {
	switch (type) {
		case ValueType::none: return "none";
		case ValueType::boolean: return "boolean";
		case ValueType::unsigned_int: return "unsigned";
		case ValueType::signed_int: return "signed";
		case ValueType::real: return "real";
		case ValueType::string: return "string";
		case ValueType::time_of_day: return "time of day";
		case ValueType::date: return "date";
		case ValueType::date_time: return "date-time";
		case ValueType::enumeration: return "enumeration";
		case ValueType::raw: return "raw";
		case ValueType::controlled: return "controlled";
		case ValueType::color: return "color";
	}
	return "?";
}

const char* quality_name(Quality quality) {
	switch (quality) {
		case Quality::unknown: return "unknown";
		case Quality::good: return "good";
		case Quality::stale: return "stale";
	}
	return "?";
}

const char* update_origin_name(UpdateOrigin origin) {
	switch (origin) {
		case UpdateOrigin::report: return "report";
		case UpdateOrigin::command: return "command";
		case UpdateOrigin::stale: return "stale";
		case UpdateOrigin::publish: return "publish";
	}
	return "?";
}

const char* datapoint_status_name(DatapointStatus status) {
	switch (status) {
		case DatapointStatus::ok: return "ok";
		case DatapointStatus::unknown_id: return "unknown id";
		case DatapointStatus::wrong_type: return "wrong type";
		case DatapointStatus::not_writable: return "not writable";
		case DatapointStatus::not_owner: return "not owner";
		case DatapointStatus::refused: return "refused by the technology";
		case DatapointStatus::busy: return "busy (called from a listener or a technology command)";
		case DatapointStatus::invalid_model: return "invalid model";
		case DatapointStatus::out_of_memory: return "out of memory";
		case DatapointStatus::capacity: return "capacity exhausted";
	}
	return "?";
}

// ----------------------------------------------------------------------------------------------
// Value

template <class T>
Value Value::make(ValueType type, const T& value) {
	static_assert(sizeof(T) <= kStorage, "value fits the storage");
	Value result;
	result.type_ = type;
	std::memcpy(result.storage_, &value, sizeof(T));
	return result;
}

template <class T>
T Value::get(ValueType type) const {
	T result{};
	if (type_ == type) {
		std::memcpy(&result, storage_, sizeof(T));
	}
	return result;
}

Value Value::of_bool(bool value) { return make(ValueType::boolean, value); }
Value Value::of_unsigned(uint32_t value) { return make(ValueType::unsigned_int, value); }
Value Value::of_signed(int32_t value) { return make(ValueType::signed_int, value); }
Value Value::of_real(double value) { return std::isfinite(value) ? make(ValueType::real, value) : Value(); }
Value Value::of_enum(uint32_t element) { return make(ValueType::enumeration, element); }
Value Value::of_time(const TimeOfDay& value) { return make(ValueType::time_of_day, value); }
Value Value::of_date(const Date& value) { return make(ValueType::date, value); }
Value Value::of_date_time(const DateTime& value) { return make(ValueType::date_time, value); }
Value Value::of_controlled(bool control, uint8_t value) {
	return make(ValueType::controlled, Controlled{control, value});
}
Value Value::of_color(const Color& value) { return make(ValueType::color, value); }

std::optional<Value> Value::of_string(const char* text, size_t length) {
	if (length > kMaxString || (length > 0 && std::memchr(text, '\0', length) != nullptr)) {
		return std::nullopt;
	}
	Value result;
	result.type_ = ValueType::string;
	result.length_ = static_cast<uint8_t>(length);
	if (length > 0) {
		std::memcpy(result.storage_, text, length);
	}
	result.storage_[length] = '\0';
	return result;
}

std::optional<Value> Value::of_string(const char* text) {
	size_t length = 0;
	while (length <= kMaxString && text[length] != '\0') {
		++length;
	}
	return of_string(text, length);
}

std::optional<Value> Value::of_raw(const uint8_t* bytes, size_t length) {
	if (length > kMaxRaw) {
		return std::nullopt;
	}
	Value result;
	result.type_ = ValueType::raw;
	result.length_ = static_cast<uint8_t>(length);
	if (length > 0) {
		std::memcpy(result.storage_, bytes, length);
	}
	return result;
}

bool Value::as_bool() const { return get<bool>(ValueType::boolean); }
uint32_t Value::as_unsigned() const { return get<uint32_t>(ValueType::unsigned_int); }
int32_t Value::as_signed() const { return get<int32_t>(ValueType::signed_int); }
double Value::as_real() const { return get<double>(ValueType::real); }
uint32_t Value::as_enum() const { return get<uint32_t>(ValueType::enumeration); }
TimeOfDay Value::as_time() const { return get<TimeOfDay>(ValueType::time_of_day); }
Date Value::as_date() const { return get<Date>(ValueType::date); }
DateTime Value::as_date_time() const { return get<DateTime>(ValueType::date_time); }
Controlled Value::as_controlled() const { return get<Controlled>(ValueType::controlled); }
Color Value::as_color() const { return get<Color>(ValueType::color); }

const char* Value::as_string() const {
	return type_ == ValueType::string ? reinterpret_cast<const char*>(storage_) : "";
}

const uint8_t* Value::raw() const { return type_ == ValueType::raw ? storage_ : nullptr; }

std::optional<Value> Value::convert(ValueType type) const {
	if (type == type_) {
		return *this;
	}
	// The numeric family: an exact conversion through a double (32-bit integers are exact).
	double number = 0;
	switch (type_) {
		case ValueType::unsigned_int: number = as_unsigned(); break;
		case ValueType::signed_int: number = as_signed(); break;
		case ValueType::real: number = as_real(); break;
		case ValueType::enumeration: number = as_enum(); break;
		default: return std::nullopt;
	}
	switch (type) {
		case ValueType::real: return of_real(number);
		case ValueType::unsigned_int:
			if (integral(number) && number >= 0 && number <= 4294967295.0) {
				return of_unsigned(static_cast<uint32_t>(number));
			}
			return std::nullopt;
		case ValueType::enumeration:
			if (integral(number) && number >= 0 && number <= 4294967295.0) {
				return of_enum(static_cast<uint32_t>(number));
			}
			return std::nullopt;
		case ValueType::signed_int:
			if (integral(number) && number >= -2147483648.0 && number <= 2147483647.0) {
				return of_signed(static_cast<int32_t>(number));
			}
			return std::nullopt;
		default: return std::nullopt;
	}
}

const char* Value::format(char* buffer, size_t size) const {
	if (size == 0) {
		return buffer;
	}
	static const char* const kDays[] = {"", " Mon", " Tue", " Wed", " Thu", " Fri", " Sat", " Sun"};
	switch (type_) {
		case ValueType::none: std::snprintf(buffer, size, "(none)"); break;
		case ValueType::boolean: std::snprintf(buffer, size, "%s", as_bool() ? "on" : "off"); break;
		case ValueType::unsigned_int: std::snprintf(buffer, size, "%" PRIu32, as_unsigned()); break;
		case ValueType::signed_int: std::snprintf(buffer, size, "%" PRId32, as_signed()); break;
		case ValueType::real: std::snprintf(buffer, size, "%g", as_real()); break;
		case ValueType::enumeration: std::snprintf(buffer, size, "#%" PRIu32, as_enum()); break;
		case ValueType::string: std::snprintf(buffer, size, "\"%s\"", as_string()); break;
		case ValueType::time_of_day: {
			TimeOfDay t = as_time();
			std::snprintf(buffer, size, "%02u:%02u:%02u%s", t.hour, t.minute, t.second,
				t.day_of_week <= 7 ? kDays[t.day_of_week] : " ?");
			break;
		}
		case ValueType::date: {
			Date d = as_date();
			std::snprintf(buffer, size, "%04u-%02u-%02u", d.year, d.month, d.day);
			break;
		}
		case ValueType::date_time: {
			DateTime d = as_date_time();
			std::snprintf(buffer, size, "%04u-%02u-%02u %02u:%02u:%02u flags 0x%04x", d.year, d.month, d.day,
				d.hour, d.minute, d.second, d.flags);
			break;
		}
		case ValueType::raw: {
			size_t used = 0;
			buffer[0] = '\0';
			for (size_t i = 0; i < length_ && used + 3 < size; ++i) {
				used += static_cast<size_t>(std::snprintf(buffer + used, size - used, i == 0 ? "%02X" : " %02X",
					storage_[i]));
			}
			break;
		}
		case ValueType::controlled: {
			Controlled c = as_controlled();
			std::snprintf(buffer, size, "control %u, %u", c.control ? 1u : 0u, c.value);
			break;
		}
		case ValueType::color: {
			Color c = as_color();
			std::snprintf(buffer, size, "rgbw %u %u %u %u valid 0x%x", c.red, c.green, c.blue, c.white, c.valid);
			break;
		}
	}
	return buffer;
}

bool operator==(const Value& a, const Value& b) {
	if (a.type_ != b.type_) {
		return false;
	}
	switch (a.type_) {
		case ValueType::none: return true;
		case ValueType::boolean: return a.as_bool() == b.as_bool();
		case ValueType::unsigned_int: return a.as_unsigned() == b.as_unsigned();
		case ValueType::signed_int: return a.as_signed() == b.as_signed();
		case ValueType::real: return a.as_real() == b.as_real();
		case ValueType::enumeration: return a.as_enum() == b.as_enum();
		case ValueType::time_of_day: return a.as_time() == b.as_time();
		case ValueType::date: return a.as_date() == b.as_date();
		case ValueType::date_time: return a.as_date_time() == b.as_date_time();
		case ValueType::controlled: return a.as_controlled() == b.as_controlled();
		case ValueType::color: return a.as_color() == b.as_color();
		case ValueType::string:
		case ValueType::raw:
			return a.length_ == b.length_ && std::memcmp(a.storage_, b.storage_, a.length_) == 0;
	}
	return false;
}

// ----------------------------------------------------------------------------------------------
// DatapointModel

DatapointModel::DatapointModel(const Clock& clock) : clock_(clock) {}

DatapointModel::~DatapointModel() { free_tables(); }

void DatapointModel::free_tables() {
	delete[] datapoints_;
	delete[] names_;
	datapoints_ = nullptr;
	names_ = nullptr;
	by_name_ = nullptr;
	count_ = 0;
}

void DatapointModel::clear() {
	if (refuse_busy("clear")) {
		return;
	}
	free_tables();
}

namespace {

/// Checks one spec; logs and returns false for a refusal.
bool valid_spec(const DatapointSpec& spec, size_t index, size_t& name_length) {
	if (spec.name == nullptr) {
		Log::error(kTag, "model refused: datapoint %u has no name", static_cast<unsigned>(index));
		return false;
	}
	size_t length = 0;
	while (length <= Datapoint::kMaxName && spec.name[length] != '\0') {
		unsigned char c = static_cast<unsigned char>(spec.name[length]);
		if (c < 0x20 || c == 0x7F) {
			Log::error(kTag, "model refused: name of datapoint %u has a control character",
				static_cast<unsigned>(index));
			return false;
		}
		++length;
	}
	if (length == 0 || length > Datapoint::kMaxName) {
		Log::error(kTag, "model refused: name of datapoint %u is %s (1..%u bytes)", static_cast<unsigned>(index),
			length == 0 ? "empty" : "too long", static_cast<unsigned>(Datapoint::kMaxName));
		return false;
	}
	if (spec.type == ValueType::none || static_cast<uint8_t>(spec.type) > static_cast<uint8_t>(ValueType::color)) {
		Log::error(kTag, "model refused: datapoint %u '%s' has no valid value type", static_cast<unsigned>(index),
			spec.name);
		return false;
	}
	if ((spec.flags & ~DatapointFlags::kAll) != 0) {
		Log::error(kTag, "model refused: datapoint %u '%s' has unknown flags 0x%02x", static_cast<unsigned>(index),
			spec.name, spec.flags);
		return false;
	}
	name_length = length;
	return true;
}

} // namespace

DatapointStatus DatapointModel::load(const DatapointSpec* specs, size_t count, bool keep_values) {
	if (refuse_busy("load")) {
		return DatapointStatus::busy;
	}
	if (count > kMaxDatapoints) {
		Log::error(kTag, "model refused: %u datapoints exceed the capacity of %u", static_cast<unsigned>(count),
			static_cast<unsigned>(kMaxDatapoints));
		return DatapointStatus::capacity;
	}
	if (specs == nullptr && count > 0) {
		Log::error(kTag, "model refused: %u datapoints without specs", static_cast<unsigned>(count));
		return DatapointStatus::invalid_model;
	}
	size_t names_bytes = 0;
	for (size_t i = 0; i < count; ++i) {
		size_t length = 0;
		if (!valid_spec(specs[i], i, length)) {
			return DatapointStatus::invalid_model;
		}
		names_bytes += length + 1;
	}

	// One block for the names followed by the sorted index (aligned for uint16_t).
	size_t index_offset = (names_bytes + 1) & ~static_cast<size_t>(1);
	size_t block = index_offset + count * sizeof(uint16_t);
	Datapoint* datapoints = count > 0 ? new (std::nothrow) Datapoint[count] : nullptr;
	char* names = block > 0 ? new (std::nothrow) char[block] : nullptr;
	if ((count > 0 && datapoints == nullptr) || (block > 0 && names == nullptr)) {
		delete[] datapoints;
		delete[] names;
		Log::error(kTag, "model refused: no memory for %u datapoints (%u bytes)", static_cast<unsigned>(count),
			static_cast<unsigned>(count * sizeof(Datapoint) + block));
		return DatapointStatus::out_of_memory;
	}
	uint16_t* by_name = reinterpret_cast<uint16_t*>(names + index_offset);
	char* cursor = names;
	for (size_t i = 0; i < count; ++i) {
		size_t length = std::strlen(specs[i].name);
		std::memcpy(cursor, specs[i].name, length + 1);
		Datapoint& dp = datapoints[i];
		dp.name_ = cursor;
		dp.id_ = static_cast<uint16_t>(i);
		dp.type_ = specs[i].type;
		dp.flags_ = specs[i].flags;
		cursor += length + 1;
		by_name[i] = static_cast<uint16_t>(i);
	}
	std::sort(by_name, by_name + count, [datapoints](uint16_t a, uint16_t b) {
		return std::strcmp(datapoints[a].name_, datapoints[b].name_) < 0;
	});
	for (size_t i = 1; i < count; ++i) {
		if (std::strcmp(datapoints[by_name[i - 1]].name_, datapoints[by_name[i]].name_) == 0) {
			Log::error(kTag, "model refused: datapoints %u and %u are both named '%s'",
				static_cast<unsigned>(std::min(by_name[i - 1], by_name[i])),
				static_cast<unsigned>(std::max(by_name[i - 1], by_name[i])), datapoints[by_name[i]].name_);
			delete[] datapoints;
			delete[] names;
			return DatapointStatus::invalid_model;
		}
	}

	size_t kept = 0;
	if (keep_values) {
		for (size_t i = 0; i < count; ++i) {
			const Datapoint* previous = find(datapoints[i].name_);
			if (previous != nullptr && previous->type_ == datapoints[i].type_) {
				datapoints[i].value_ = previous->value_;
				datapoints[i].quality_ = previous->quality_;
				datapoints[i].updated_ms_ = previous->updated_ms_;
				++kept;
			}
		}
	}
	free_tables();
	datapoints_ = datapoints;
	names_ = names;
	by_name_ = by_name;
	count_ = count;
	if (keep_values) {
		Log::info(kTag, "model loaded: %u datapoints, %u bytes, %u values kept", static_cast<unsigned>(count),
			static_cast<unsigned>(count * sizeof(Datapoint) + block), static_cast<unsigned>(kept));
	} else {
		Log::info(kTag, "model loaded: %u datapoints, %u bytes", static_cast<unsigned>(count),
			static_cast<unsigned>(count * sizeof(Datapoint) + block));
	}
	return DatapointStatus::ok;
}

const Datapoint* DatapointModel::get(uint16_t id) const { return id < count_ ? &datapoints_[id] : nullptr; }

const Datapoint* DatapointModel::find(const char* name) const {
	if (name == nullptr) {
		return nullptr;
	}
	const uint16_t* begin = by_name_;
	const uint16_t* end = by_name_ + count_;
	const uint16_t* it = std::lower_bound(begin, end, name, [this](uint16_t id, const char* key) {
		return std::strcmp(datapoints_[id].name_, key) < 0;
	});
	if (it != end && std::strcmp(datapoints_[*it].name_, name) == 0) {
		return &datapoints_[*it];
	}
	return nullptr;
}

bool DatapointModel::refuse_busy(const char* what) {
	if (notifying_) {
		Log::error(kTag, "refusing %s from a datapoint listener (post it to the event loop)", what);
		return true;
	}
	if (commanding_) {
		Log::error(kTag, "refusing %s from within a technology's command (post it to the event loop)", what);
		return true;
	}
	return false;
}

Datapoint* DatapointModel::lookup(uint16_t id, const char* what) {
	if (id >= count_) {
		Log::error(kTag, "refusing %s: no datapoint with id %u (model has %u)", what, id,
			static_cast<unsigned>(count_));
		return nullptr;
	}
	return &datapoints_[id];
}

DatapointStatus DatapointModel::bind(uint16_t id, Technology& technology) {
	if (refuse_busy("bind")) {
		return DatapointStatus::busy;
	}
	Datapoint* dp = lookup(id, "bind");
	if (dp == nullptr) {
		return DatapointStatus::unknown_id;
	}
	if (dp->technology_ != nullptr && dp->technology_ != &technology) {
		Log::error(kTag, "refusing to bind '%s' to %s: owned by %s", dp->name_, technology.name(),
			dp->technology_->name());
		return DatapointStatus::not_owner;
	}
	dp->technology_ = &technology;
	return DatapointStatus::ok;
}

DatapointStatus DatapointModel::set_transmitter(uint16_t id, Technology& transmitter, const char** error) {
	if (refuse_busy("set transmitter")) {
		if (error != nullptr) {
			*error = "busy (called from a listener or a technology command)";
		}
		return DatapointStatus::busy;
	}
	Datapoint* dp = lookup(id, "set transmitter");
	if (dp == nullptr) {
		if (error != nullptr) {
			*error = "no datapoint with this id";
		}
		return DatapointStatus::unknown_id;
	}
	if (dp->technology_ == nullptr) {
		Log::error(kTag, "refusing %s as transmitter of '%s': the datapoint has no owner yet", transmitter.name(),
			dp->name_);
		if (error != nullptr) {
			*error = "the datapoint has no owner yet";
		}
		return DatapointStatus::not_owner;
	}
	if (dp->technology_ == &transmitter) {
		return DatapointStatus::ok;
	}
	if (dp->transmitter_ != nullptr && dp->transmitter_ != &transmitter) {
		Log::error(kTag, "refusing %s as transmitter of '%s': transmitted by %s", transmitter.name(), dp->name_,
			dp->transmitter_->name());
		if (error != nullptr) {
			*error = "the datapoint has another transmitter";
		}
		return DatapointStatus::not_owner;
	}
	dp->transmitter_ = &transmitter;
	return DatapointStatus::ok;
}

DatapointStatus DatapointModel::publish(Technology& owner, uint16_t id, const Value& value) {
	if (refuse_busy("publish")) {
		return DatapointStatus::busy;
	}
	Datapoint* dp = lookup(id, "publish");
	if (dp == nullptr) {
		return DatapointStatus::unknown_id;
	}
	if (dp->technology_ != &owner) {
		Log::error(kTag, "refusing publish of '%s' from %s: not its owner", dp->name_, owner.name());
		return DatapointStatus::not_owner;
	}
	std::optional<Value> converted = value.convert(dp->type_);
	if (!converted) {
		Log::error(kTag, "refusing publish of '%s' from %s: a %s value for a %s datapoint", dp->name_, owner.name(),
			value_type_name(value.type()), value_type_name(dp->type_));
		return DatapointStatus::wrong_type;
	}
	if (dp->transmitter_ != nullptr && dp->transmits()) {
		commanding_ = true;
		bool accepted = dp->transmitter_->command(*dp, *converted);
		commanding_ = false;
		if (!accepted) {
			char text[48];
			Log::warn(kTag, "publish %s of '%s' refused by %s", converted->format(text, sizeof(text)), dp->name_,
				dp->transmitter_->name());
			return DatapointStatus::refused;
		}
	}
	store(*dp, *converted, UpdateOrigin::publish);
	return DatapointStatus::ok;
}

DatapointStatus DatapointModel::report(Technology& technology, uint16_t id, const Value& value) {
	if (refuse_busy("report")) {
		return DatapointStatus::busy;
	}
	Datapoint* dp = lookup(id, "report");
	if (dp == nullptr) {
		return DatapointStatus::unknown_id;
	}
	if (dp->technology_ != &technology) {
		Log::error(kTag, "refusing report of '%s' from %s: not its owner", dp->name_, technology.name());
		return DatapointStatus::not_owner;
	}
	std::optional<Value> converted = value.convert(dp->type_);
	if (!converted) {
		Log::error(kTag, "refusing report of '%s' from %s: a %s value for a %s datapoint", dp->name_,
			technology.name(), value_type_name(value.type()), value_type_name(dp->type_));
		return DatapointStatus::wrong_type;
	}
	store(*dp, *converted, UpdateOrigin::report);
	return DatapointStatus::ok;
}

DatapointStatus DatapointModel::report_stale(Technology& technology, uint16_t id) {
	if (refuse_busy("stale report")) {
		return DatapointStatus::busy;
	}
	Datapoint* dp = lookup(id, "stale report");
	if (dp == nullptr) {
		return DatapointStatus::unknown_id;
	}
	if (dp->technology_ != &technology) {
		Log::error(kTag, "refusing stale report of '%s' from %s: not its owner", dp->name_, technology.name());
		return DatapointStatus::not_owner;
	}
	if (dp->quality_ != Quality::good) {
		return DatapointStatus::ok; // Unknown stays unknown; stale stays stale.
	}
	Value old_value = dp->value_;
	Quality old_quality = dp->quality_;
	dp->quality_ = Quality::stale;
	notify(*dp, old_value, old_quality, UpdateOrigin::stale);
	return DatapointStatus::ok;
}

DatapointStatus DatapointModel::restore(uint16_t id, const Value& value) {
	if (refuse_busy("restore")) {
		return DatapointStatus::busy;
	}
	Datapoint* dp = lookup(id, "restore");
	if (dp == nullptr) {
		return DatapointStatus::unknown_id;
	}
	if (dp->quality_ != Quality::unknown) {
		Log::error(kTag, "refusing restore of '%s': it has a %s value already", dp->name_, quality_name(dp->quality_));
		return DatapointStatus::busy;
	}
	std::optional<Value> converted = value.convert(dp->type_);
	if (!converted) {
		Log::error(kTag, "refusing restore of '%s': a %s value for a %s datapoint", dp->name_,
			value_type_name(value.type()), value_type_name(dp->type_));
		return DatapointStatus::wrong_type;
	}
	dp->value_ = *converted;
	dp->quality_ = Quality::stale;
	return DatapointStatus::ok;
}

DatapointStatus DatapointModel::command(uint16_t id, const Value& value) {
	if (refuse_busy("command")) {
		return DatapointStatus::busy;
	}
	Datapoint* dp = lookup(id, "command");
	if (dp == nullptr) {
		return DatapointStatus::unknown_id;
	}
	if (!dp->writable()) {
		Log::error(kTag, "refusing command to '%s': not writable", dp->name_);
		return DatapointStatus::not_writable;
	}
	std::optional<Value> converted = value.convert(dp->type_);
	if (!converted) {
		Log::error(kTag, "refusing command to '%s': a %s value for a %s datapoint", dp->name_,
			value_type_name(value.type()), value_type_name(dp->type_));
		return DatapointStatus::wrong_type;
	}
	if (dp->technology_ != nullptr && dp->transmits()) {
		commanding_ = true;
		bool accepted = dp->technology_->command(*dp, *converted);
		commanding_ = false;
		if (!accepted) {
			char text[48];
			Log::error(kTag, "command %s to '%s' refused by %s", converted->format(text, sizeof(text)), dp->name_,
				dp->technology_->name());
			return DatapointStatus::refused;
		}
	}
	store(*dp, *converted, UpdateOrigin::command);
	return DatapointStatus::ok;
}

void DatapointModel::store(Datapoint& datapoint, const Value& value, UpdateOrigin origin) {
	Value old_value = datapoint.value_;
	Quality old_quality = datapoint.quality_;
	datapoint.value_ = value;
	datapoint.quality_ = Quality::good;
	datapoint.updated_ms_ = clock_.monotonic_ms();
	notify(datapoint, old_value, old_quality, origin);
}

void DatapointModel::notify(const Datapoint& datapoint, const Value& old_value, Quality old_quality,
	UpdateOrigin origin) {
	notifying_ = true;
	DatapointUpdate update{datapoint, old_value, old_quality, origin};
	for (size_t i = 0; i < listener_count_; ++i) {
		listeners_[i]->datapoint_updated(update);
	}
	notifying_ = false;
}

DatapointStatus DatapointModel::add_listener(DatapointListener& listener) {
	if (refuse_busy("adding a listener")) {
		return DatapointStatus::busy;
	}
	for (size_t i = 0; i < listener_count_; ++i) {
		if (listeners_[i] == &listener) {
			Log::error(kTag, "refusing datapoint listener: already registered");
			return DatapointStatus::capacity;
		}
	}
	if (listener_count_ == kListenerCapacity) {
		Log::error(kTag, "datapoint listeners exhausted (capacity %u), refusing listener",
			static_cast<unsigned>(kListenerCapacity));
		return DatapointStatus::capacity;
	}
	listeners_[listener_count_++] = &listener;
	return DatapointStatus::ok;
}

bool DatapointModel::remove_listener(DatapointListener& listener) {
	if (refuse_busy("removing a listener")) {
		return false;
	}
	for (size_t i = 0; i < listener_count_; ++i) {
		if (listeners_[i] == &listener) {
			for (size_t j = i + 1; j < listener_count_; ++j) {
				listeners_[j - 1] = listeners_[j];
			}
			listeners_[--listener_count_] = nullptr;
			return true;
		}
	}
	return false;
}

} // namespace hc
