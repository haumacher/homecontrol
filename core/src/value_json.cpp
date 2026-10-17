/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/value_json.h"

#include <cmath>

#include "hc/sha.h"

namespace hc {

void write_value_json(JsonWriter& json, const Value& value) {
	switch (value.type()) {
	case ValueType::none:
		json.null();
		return;
	case ValueType::boolean:
		json.boolean(value.as_bool());
		return;
	case ValueType::unsigned_int:
		json.number(value.as_unsigned());
		return;
	case ValueType::signed_int:
		json.number(value.as_signed());
		return;
	case ValueType::real:
		json.number(value.as_real());
		return;
	case ValueType::enumeration:
		json.number(value.as_enum());
		return;
	case ValueType::string:
		json.string(value.as_string(), value.length());
		return;
	case ValueType::raw: {
		char hex[Value::kMaxRaw * 2 + 1];
		to_hex(value.raw(), value.length(), hex);
		json.string(hex);
		return;
	}
	case ValueType::time_of_day: {
		TimeOfDay t = value.as_time();
		json.begin_object()
			.member("day", uint32_t(t.day_of_week))
			.member("hour", uint32_t(t.hour))
			.member("minute", uint32_t(t.minute))
			.member("second", uint32_t(t.second))
			.end_object();
		return;
	}
	case ValueType::date: {
		Date d = value.as_date();
		json.begin_object()
			.member("year", uint32_t(d.year))
			.member("month", uint32_t(d.month))
			.member("day", uint32_t(d.day))
			.end_object();
		return;
	}
	case ValueType::date_time: {
		DateTime d = value.as_date_time();
		json.begin_object()
			.member("year", uint32_t(d.year))
			.member("month", uint32_t(d.month))
			.member("day", uint32_t(d.day))
			.member("day_of_week", uint32_t(d.day_of_week))
			.member("hour", uint32_t(d.hour))
			.member("minute", uint32_t(d.minute))
			.member("second", uint32_t(d.second))
			.member("flags", uint32_t(d.flags))
			.end_object();
		return;
	}
	case ValueType::controlled: {
		Controlled c = value.as_controlled();
		json.begin_object().member("control", c.control).member("value", uint32_t(c.value)).end_object();
		return;
	}
	case ValueType::color: {
		Color c = value.as_color();
		json.begin_object()
			.member("red", uint32_t(c.red))
			.member("green", uint32_t(c.green))
			.member("blue", uint32_t(c.blue))
			.member("white", uint32_t(c.white))
			.member("valid", uint32_t(c.valid))
			.end_object();
		return;
	}
	}
	json.null();
}

namespace {

/// Reads the integer member `name` of `object` in [min, max].
bool field(const JsonSpan& object, const char* name, uint32_t min, uint32_t max, uint32_t& out, const char** error) {
	JsonSpan span;
	double number = 0;
	if (!json_member(object, name, span)) {
		*error = "a field of the value is missing";
		return false;
	}
	if (!json_number(span, number) || number != std::floor(number) || number < min || number > max) {
		*error = "a field of the value is not an integer in its range";
		return false;
	}
	out = static_cast<uint32_t>(number);
	return true;
}

} // namespace

std::optional<Value> read_value_json(const JsonSpan& span, ValueType type, const char** error) {
	const char* dummy = nullptr;
	if (error == nullptr) {
		error = &dummy;
	}
	switch (type) {
	case ValueType::none:
		*error = "the datapoint has no type";
		return std::nullopt;
	case ValueType::boolean: {
		bool b = false;
		if (json_bool(span, b)) {
			return Value::of_bool(b);
		}
		*error = "a boolean datapoint takes true or false";
		return std::nullopt;
	}
	case ValueType::unsigned_int:
	case ValueType::signed_int:
	case ValueType::real:
	case ValueType::enumeration: {
		double number = 0;
		if (!json_number(span, number)) {
			*error = "a numeric datapoint takes a number";
			return std::nullopt;
		}
		std::optional<Value> converted = Value::of_real(number).convert(type);
		if (!converted) {
			*error = "the number does not fit the datapoint's type (range, or a fraction for an integer)";
		}
		return converted;
	}
	case ValueType::string: {
		char text[Value::kMaxString + 1];
		size_t length = 0;
		if (span.kind != JsonSpan::Kind::string) {
			*error = "a string datapoint takes a string";
			return std::nullopt;
		}
		if (!json_string(span, text, sizeof(text), &length)) {
			*error = "the string is longer than 28 bytes or contains a NUL";
			return std::nullopt;
		}
		return Value::of_string(text, length);
	}
	case ValueType::raw: {
		char hex[Value::kMaxRaw * 2 + 1];
		size_t length = 0;
		uint8_t bytes[Value::kMaxRaw];
		if (!json_string(span, hex, sizeof(hex), &length) || length % 2 != 0 ||
			!from_hex(hex, bytes, length / 2)) {
			*error = "a raw datapoint takes a string of at most 28 hex digits";
			return std::nullopt;
		}
		return Value::of_raw(bytes, length / 2);
	}
	case ValueType::time_of_day: {
		uint32_t day, hour, minute, second;
		if (span.kind != JsonSpan::Kind::object) {
			*error = "a time of day takes {\"day\",\"hour\",\"minute\",\"second\"}";
			return std::nullopt;
		}
		if (!field(span, "day", 0, 7, day, error) || !field(span, "hour", 0, 23, hour, error) ||
			!field(span, "minute", 0, 59, minute, error) || !field(span, "second", 0, 59, second, error)) {
			return std::nullopt;
		}
		TimeOfDay t;
		t.day_of_week = uint8_t(day);
		t.hour = uint8_t(hour);
		t.minute = uint8_t(minute);
		t.second = uint8_t(second);
		return Value::of_time(t);
	}
	case ValueType::date: {
		uint32_t year, month, day;
		if (span.kind != JsonSpan::Kind::object) {
			*error = "a date takes {\"year\",\"month\",\"day\"}";
			return std::nullopt;
		}
		if (!field(span, "year", 0, 65535, year, error) || !field(span, "month", 1, 12, month, error) ||
			!field(span, "day", 1, 31, day, error)) {
			return std::nullopt;
		}
		Date d;
		d.year = uint16_t(year);
		d.month = uint8_t(month);
		d.day = uint8_t(day);
		return Value::of_date(d);
	}
	case ValueType::date_time: {
		uint32_t year, month, day, dow, hour, minute, second, flags;
		if (span.kind != JsonSpan::Kind::object) {
			*error = "a date-time takes an object of its fields";
			return std::nullopt;
		}
		if (!field(span, "year", 1900, 2155, year, error) || !field(span, "month", 1, 12, month, error) ||
			!field(span, "day", 1, 31, day, error) || !field(span, "day_of_week", 0, 7, dow, error) ||
			!field(span, "hour", 0, 24, hour, error) || !field(span, "minute", 0, 59, minute, error) ||
			!field(span, "second", 0, 59, second, error) || !field(span, "flags", 0, 0xFFFF, flags, error)) {
			return std::nullopt;
		}
		if ((flags & ~uint32_t(DateTime::kAllFlags)) != 0) {
			*error = "unknown date-time flag bits";
			return std::nullopt;
		}
		DateTime d;
		d.year = uint16_t(year);
		d.month = uint8_t(month);
		d.day = uint8_t(day);
		d.day_of_week = uint8_t(dow);
		d.hour = uint8_t(hour);
		d.minute = uint8_t(minute);
		d.second = uint8_t(second);
		d.flags = uint16_t(flags);
		return Value::of_date_time(d);
	}
	case ValueType::controlled: {
		JsonSpan control;
		bool c = false;
		uint32_t value = 0;
		if (span.kind != JsonSpan::Kind::object || !json_member(span, "control", control) || !json_bool(control, c)) {
			*error = "a controlled value takes {\"control\": true|false, \"value\": n}";
			return std::nullopt;
		}
		if (!field(span, "value", 0, 255, value, error)) {
			return std::nullopt;
		}
		return Value::of_controlled(c, uint8_t(value));
	}
	case ValueType::color: {
		uint32_t r, g, b, w = 0, valid = Color::kRgb;
		if (span.kind != JsonSpan::Kind::object) {
			*error = "a color takes {\"red\",\"green\",\"blue\"[,\"white\",\"valid\"]}";
			return std::nullopt;
		}
		JsonSpan ignored;
		if (!field(span, "red", 0, 255, r, error) || !field(span, "green", 0, 255, g, error) ||
			!field(span, "blue", 0, 255, b, error)) {
			return std::nullopt;
		}
		if (json_member(span, "white", ignored) && !field(span, "white", 0, 255, w, error)) {
			return std::nullopt;
		}
		if (json_member(span, "valid", ignored) && !field(span, "valid", 0, Color::kAll, valid, error)) {
			return std::nullopt;
		}
		Color c;
		c.red = uint8_t(r);
		c.green = uint8_t(g);
		c.blue = uint8_t(b);
		c.white = uint8_t(w);
		c.valid = uint8_t(valid);
		return Value::of_color(c);
	}
	}
	*error = "unsupported type";
	return std::nullopt;
}

} // namespace hc
