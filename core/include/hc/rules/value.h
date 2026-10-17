/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_RULES_VALUE_H
#define HC_RULES_VALUE_H

#include <cstddef>
#include <cstdint>
#include <optional>

#include "hc/datapoint.h"
#include "hc/json_value.h"

/// The rule interpreter (spec/rules.md, doc/rules.md).
namespace hc::rules {

/// The expression types of spec/rules.md 3.1.
enum class Type : uint8_t {
	boolean,
	integer,     ///< int64_t
	real,        ///< double, finite
	string,      ///< UTF-8, at most kMaxString bytes
	duration,    ///< int64_t milliseconds
	time_of_day, ///< hc::TimeOfDay
	date,        ///< hc::Date
	date_time,   ///< hc::DateTime
	enumeration, ///< uint32_t element number
	controlled,  ///< hc::Controlled
	color,       ///< hc::Color
	raw,         ///< at most kMaxRaw bytes
	invalid = 0xFF,
};

constexpr size_t kMaxString = Value::kMaxString;
constexpr size_t kMaxRaw = Value::kMaxRaw;

/// The spec's (camelCase) name of a type: "boolean", "integer", ..., "timeOfDay", "dateTime".
const char* type_name(Type type);
/// The type named `name` (spec names); Type::invalid if there is none.
Type type_from_name(const char* name);

/// The expression type a datapoint of `type` reads as (unsigned and signed read as integer).
Type type_of_datapoint(ValueType type);
/// The spec's name of a datapoint value type ("unsigned", "timeOfDay", ...); "none" for none.
const char* datapoint_type_name(ValueType type);
/// The datapoint value type named `name` (spec names); ValueType::none if there is none.
ValueType datapoint_type_from_name(const char* name);

/// An expression value: a type and, unless it has *no value* (spec 3.3), the value. 40 bytes, no
/// heap; strings and raw bytes inline.
struct Val {
	Type type = Type::invalid;
	bool present = false; ///< false: no value.
	uint8_t length = 0;   ///< Bytes of a string or raw value.
	union Data {
		Data() : i(0) {}
		bool b;
		int64_t i; ///< integer, duration
		double r;
		uint32_t e;
		TimeOfDay tod;
		Date date;
		DateTime dt;
		Controlled ctl;
		Color color;
		char bytes[kMaxString + 1]; ///< string (NUL-terminated) or raw
	} u;

	bool has() const { return present; }

	static Val none(Type type = Type::invalid) {
		Val v;
		v.type = type;
		return v;
	}
	static Val of_bool(bool value);
	static Val of_int(int64_t value);
	static Val of_real(double value); ///< No value if not finite.
	static Val of_duration(int64_t ms);
	static Val of_enum(uint32_t value);
	static Val of_time(const TimeOfDay& value);
	static Val of_date(const Date& value);
	static Val of_date_time(const DateTime& value);
	static Val of_controlled(bool control, uint8_t value);
	static Val of_color(const Color& value);
	/// No value if longer than kMaxString or containing a NUL.
	static Val of_string(const char* text, size_t length);
	static Val of_raw(const uint8_t* bytes, size_t length);

	const char* str() const { return u.bytes; }
};

/// Equality of two present values of the same type, as the language's `==` (spec 4.5): reals
/// exactly, times of day without the day of week, dates and date-times by their fields without
/// flags and day of week, strings and raw bytes bytewise, the rest by all fields.
bool val_equal(const Val& a, const Val& b);

/// Exact identity (all fields, also the day of week and the flags): what a stored value is.
bool val_identical(const Val& a, const Val& b);

/// The value of a datapoint as an expression value (unsigned and signed become integer).
Val val_from_datapoint(const Value& value);

/// Whether an expression of `type` may be written to a datapoint of `target` (spec 3.2).
bool write_allowed(Type type, ValueType target);
/// The value converted exactly to the datapoint type, or empty (spec 3.2: not exact at run time).
std::optional<Value> convert_for_datapoint(const Val& value, ValueType target);

/// Whether an expression of `type` may be assigned to a parameter of `target` (spec 3.2).
bool assign_allowed(Type type, Type target);
/// The value converted exactly to the parameter type; false if not exact.
bool convert_for_parameter(const Val& value, Type target, Val& out);

/// Outcome of reading a JSON literal value (spec 3.1).
enum class LiteralStatus : uint8_t {
	ok,
	malformed, ///< Wrong JSON kind or members.
	invalid,   ///< Out of its range (25:00, a level of 256, a string longer than 28 bytes, ...).
};

/// Reads the JSON literal value of `type` (spec 3.1). `reason` (if given) names a problem.
LiteralStatus val_from_json(Type type, const JsonValue& json, Val& out, const char** reason = nullptr);

/// Formats a value as log messages and toString() do (spec 4.9; "none" for no value). Writes at
/// most `size - 1` bytes plus a NUL and returns the full length (snprintf convention).
size_t format_val(const Val& value, char* out, size_t size);

/// The canonical literal text of a value (spec 4.2): `21.5`, `1h30min`, `"text"`, `#ff8000`,
/// `x"0AFF"`, `2026-10-17T20:30`. Booleans, integers and enumerations as decimals; a controlled
/// value as `controlled(true, 3)`. Same return convention as format_val.
size_t literal_text(const Val& value, char* out, size_t size);

/// A real rounded to 6 decimals, ties away from zero, trailing zeros removed (spec 4.9).
size_t format_real(double value, char* out, size_t size);
/// The shortest decimal reading back as the same double (spec 4.2).
size_t real_literal_text(double value, char* out, size_t size);
/// A duration normalized into d h min s ms (spec 4.2); "0s" for zero.
size_t duration_text(int64_t ms, char* out, size_t size);

/// Calendar helpers on proleptic Gregorian dates.
int64_t days_of(int32_t year, unsigned month, unsigned day);
void civil_of(int64_t days, int32_t& year, unsigned& month, unsigned& day);
/// ISO day of the week 1 (Monday) .. 7 (Sunday).
uint8_t weekday_of(int32_t year, unsigned month, unsigned day);
bool valid_date(int32_t year, unsigned month, unsigned day);

} // namespace hc::rules

#endif // HC_RULES_VALUE_H
