/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/rules/value.h"

#include <cinttypes>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hc/log.h"
#include "hc/tz.h"

namespace hc::rules {

namespace {

constexpr double kMaxSafeInteger = 9007199254740991.0; // 2^53 - 1

struct TypeName {
	Type type;
	const char* name;
};

constexpr TypeName kTypeNames[] = {
	{Type::boolean, "boolean"},
	{Type::integer, "integer"},
	{Type::real, "real"},
	{Type::string, "string"},
	{Type::duration, "duration"},
	{Type::time_of_day, "timeOfDay"},
	{Type::date, "date"},
	{Type::date_time, "dateTime"},
	{Type::enumeration, "enumeration"},
	{Type::controlled, "controlled"},
	{Type::color, "color"},
	{Type::raw, "raw"},
};

struct DatapointTypeName {
	ValueType type;
	const char* name;
};

constexpr DatapointTypeName kDatapointTypeNames[] = {
	{ValueType::boolean, "boolean"},
	{ValueType::unsigned_int, "unsigned"},
	{ValueType::signed_int, "signed"},
	{ValueType::real, "real"},
	{ValueType::string, "string"},
	{ValueType::time_of_day, "timeOfDay"},
	{ValueType::date, "date"},
	{ValueType::date_time, "dateTime"},
	{ValueType::enumeration, "enumeration"},
	{ValueType::controlled, "controlled"},
	{ValueType::color, "color"},
	{ValueType::raw, "raw"},
};

bool integral(double value) { return std::isfinite(value) && std::floor(value) == value; }

/// snprintf-style appender that keeps the full length.
struct Out {
	char* buffer;
	size_t size;
	size_t length = 0;

	Out(char* b, size_t s) : buffer(b), size(s) {
		if (size > 0) {
			buffer[0] = '\0';
		}
	}

	void put(char c) {
		if (length + 1 < size) {
			buffer[length] = c;
			buffer[length + 1] = '\0';
		}
		++length;
	}

	void put(const char* text) {
		while (*text) {
			put(*text++);
		}
	}

	void put(const char* text, size_t n) {
		for (size_t i = 0; i < n; ++i) {
			put(text[i]);
		}
	}

	void printf(const char* fmt, ...) HC_PRINTF_FORMAT(2, 3) {
		char temp[64];
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(temp, sizeof(temp), fmt, args);
		va_end(args);
		put(temp);
	}
};

// ------------------------------------------------------------------------------------------
// Exact decimal conversion of doubles (format_real): a small unsigned big integer.

class BigInt {
public:
	explicit BigInt(uint64_t value) {
		limbs_[0] = static_cast<uint32_t>(value);
		limbs_[1] = static_cast<uint32_t>(value >> 32);
		count_ = limbs_[1] ? 2 : (limbs_[0] ? 1 : 0);
	}

	void mul_small(uint32_t factor) {
		uint64_t carry = 0;
		for (size_t i = 0; i < count_; ++i) {
			uint64_t product = static_cast<uint64_t>(limbs_[i]) * factor + carry;
			limbs_[i] = static_cast<uint32_t>(product);
			carry = product >> 32;
		}
		if (carry && count_ < kLimbs) {
			limbs_[count_++] = static_cast<uint32_t>(carry);
		}
	}

	void add_one() {
		for (size_t i = 0; i < kLimbs; ++i) {
			if (++limbs_[i] != 0) {
				if (i >= count_) {
					count_ = i + 1;
				}
				return;
			}
		}
	}

	bool bit(size_t index) const {
		size_t limb = index / 32;
		return limb < count_ && ((limbs_[limb] >> (index % 32)) & 1u) != 0;
	}

	void shift_left(size_t bits) {
		size_t limb_shift = bits / 32;
		unsigned bit_shift = static_cast<unsigned>(bits % 32);
		if (count_ == 0) {
			return;
		}
		size_t new_count = count_ + limb_shift + 1;
		if (new_count > kLimbs) {
			new_count = kLimbs;
		}
		for (size_t i = new_count; i-- > 0;) {
			uint64_t value = 0;
			if (i >= limb_shift) {
				size_t source = i - limb_shift;
				uint64_t high = source < count_ ? limbs_[source] : 0;
				uint64_t low = (source >= 1 && source - 1 < count_) ? limbs_[source - 1] : 0;
				value = bit_shift ? ((high << bit_shift) | (low >> (32 - bit_shift))) : high;
			}
			limbs_[i] = static_cast<uint32_t>(value);
		}
		count_ = new_count;
		trim();
	}

	void shift_right(size_t bits) {
		size_t limb_shift = bits / 32;
		unsigned bit_shift = static_cast<unsigned>(bits % 32);
		if (limb_shift >= count_) {
			count_ = 0;
			return;
		}
		size_t new_count = count_ - limb_shift;
		for (size_t i = 0; i < new_count; ++i) {
			uint64_t low = limbs_[i + limb_shift];
			uint64_t high = i + limb_shift + 1 < count_ ? limbs_[i + limb_shift + 1] : 0;
			limbs_[i] = static_cast<uint32_t>(bit_shift ? ((low >> bit_shift) | (high << (32 - bit_shift))) : low);
		}
		count_ = new_count;
		trim();
	}

	/// Divides by `divisor` and returns the remainder.
	uint32_t div_small(uint32_t divisor) {
		uint64_t remainder = 0;
		for (size_t i = count_; i-- > 0;) {
			uint64_t current = (remainder << 32) | limbs_[i];
			limbs_[i] = static_cast<uint32_t>(current / divisor);
			remainder = current % divisor;
		}
		trim();
		return static_cast<uint32_t>(remainder);
	}

	bool zero() const { return count_ == 0; }

private:
	static constexpr size_t kLimbs = 36; // 1152 bits: a double's mantissa times 10^6 times 2^971

	void trim() {
		while (count_ > 0 && limbs_[count_ - 1] == 0) {
			--count_;
		}
	}

	uint32_t limbs_[kLimbs] = {};
	size_t count_ = 0;
};

void put_two(Out& out, unsigned value) { out.printf("%02u", value); }

void put_time(Out& out, unsigned hour, unsigned minute, unsigned second) {
	put_two(out, hour);
	out.put(':');
	put_two(out, minute);
	if (second != 0) {
		out.put(':');
		put_two(out, second);
	}
}

void put_date(Out& out, unsigned year, unsigned month, unsigned day) { out.printf("%04u-%02u-%02u", year, month, day); }

void put_color(Out& out, const Color& c) {
	out.printf("#%02x%02x%02x", c.red, c.green, c.blue);
	if ((c.valid & Color::kWhite) != 0) {
		out.printf("%02x", c.white);
	}
}

void put_raw(Out& out, const Val& v) {
	out.put("x\"");
	for (size_t i = 0; i < v.length; ++i) {
		out.printf("%02X", static_cast<uint8_t>(v.u.bytes[i]));
	}
	out.put('"');
}

void put_quoted(Out& out, const char* text, size_t length) {
	out.put('"');
	for (size_t i = 0; i < length; ++i) {
		char c = text[i];
		switch (c) {
			case '"': out.put("\\\""); break;
			case '\\': out.put("\\\\"); break;
			case '\n': out.put("\\n"); break;
			case '\t': out.put("\\t"); break;
			default: out.put(c); break;
		}
	}
	out.put('"');
}

/// Common to format_val and literal_text: the types whose text is the canonical literal.
bool put_literal_common(Out& out, const Val& v) {
	switch (v.type) {
		case Type::boolean: out.put(v.u.b ? "true" : "false"); return true;
		case Type::integer: out.printf("%" PRId64, v.u.i); return true;
		case Type::enumeration: out.printf("%" PRIu32, v.u.e); return true;
		case Type::duration: {
			char text[48];
			duration_text(v.u.i, text, sizeof(text));
			out.put(text);
			return true;
		}
		case Type::time_of_day: {
			// A day of the week precedes the time (spec 4.2): "mon 07:30".
			static const char* const kDays[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
			if (v.u.tod.day_of_week >= 1 && v.u.tod.day_of_week <= 7) {
				out.put(kDays[v.u.tod.day_of_week - 1]);
				out.put(' ');
			}
			put_time(out, v.u.tod.hour, v.u.tod.minute, v.u.tod.second);
			return true;
		}
		case Type::date: put_date(out, v.u.date.year, v.u.date.month, v.u.date.day); return true;
		case Type::date_time:
			put_date(out, v.u.dt.year, v.u.dt.month, v.u.dt.day);
			out.put('T');
			put_time(out, v.u.dt.hour, v.u.dt.minute, v.u.dt.second);
			return true;
		case Type::color: put_color(out, v.u.color); return true;
		case Type::raw: put_raw(out, v); return true;
		case Type::controlled:
			out.printf("controlled(%s, %u)", v.u.ctl.control ? "true" : "false", v.u.ctl.value);
			return true;
		default: return false;
	}
}

// JSON reading helpers.

bool allowed_member(const char* name, const char* const* allowed) {
	if (std::strcmp(name, "description") == 0 || std::strcmp(name, "comment") == 0) {
		return true;
	}
	for (; *allowed; ++allowed) {
		if (std::strcmp(name, *allowed) == 0) {
			return true;
		}
	}
	return false;
}

bool only_members(const JsonValue& object, const char* const* allowed) {
	size_t position = 0;
	char name[32];
	size_t length = 0;
	JsonValue value;
	while (object.next_member(position, name, sizeof(name), length, value)) {
		if (length >= sizeof(name) || !allowed_member(name, allowed)) {
			return false;
		}
	}
	return true;
}

/// An integral member within [low, high]. Absent: `present` false (status ok).
LiteralStatus int_member(const JsonValue& object, const char* name, int64_t low, int64_t high, bool required,
	int64_t& out, bool* present = nullptr) {
	JsonValue member;
	if (!object.member(name, member)) {
		if (present) {
			*present = false;
		}
		return required ? LiteralStatus::malformed : LiteralStatus::ok;
	}
	if (present) {
		*present = true;
	}
	double number = 0;
	if (!member.get_number(number)) {
		return LiteralStatus::malformed;
	}
	if (!integral(number) || number < static_cast<double>(low) || number > static_cast<double>(high)) {
		return LiteralStatus::invalid;
	}
	out = static_cast<int64_t>(number);
	return LiteralStatus::ok;
}

int hex_digit(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

} // namespace

const char* type_name(Type type) {
	for (const TypeName& entry : kTypeNames) {
		if (entry.type == type) {
			return entry.name;
		}
	}
	return "invalid";
}

Type type_from_name(const char* name) {
	for (const TypeName& entry : kTypeNames) {
		if (std::strcmp(entry.name, name) == 0) {
			return entry.type;
		}
	}
	return Type::invalid;
}

Type type_of_datapoint(ValueType type) {
	switch (type) {
		case ValueType::boolean: return Type::boolean;
		case ValueType::unsigned_int:
		case ValueType::signed_int: return Type::integer;
		case ValueType::real: return Type::real;
		case ValueType::string: return Type::string;
		case ValueType::time_of_day: return Type::time_of_day;
		case ValueType::date: return Type::date;
		case ValueType::date_time: return Type::date_time;
		case ValueType::enumeration: return Type::enumeration;
		case ValueType::raw: return Type::raw;
		case ValueType::controlled: return Type::controlled;
		case ValueType::color: return Type::color;
		case ValueType::none: break;
	}
	return Type::invalid;
}

const char* datapoint_type_name(ValueType type) {
	for (const DatapointTypeName& entry : kDatapointTypeNames) {
		if (entry.type == type) {
			return entry.name;
		}
	}
	return "none";
}

ValueType datapoint_type_from_name(const char* name) {
	for (const DatapointTypeName& entry : kDatapointTypeNames) {
		if (std::strcmp(entry.name, name) == 0) {
			return entry.type;
		}
	}
	return ValueType::none;
}

// ------------------------------------------------------------------------------------------
// Val

Val Val::of_bool(bool value) {
	Val v = none(Type::boolean);
	v.present = true;
	v.u.b = value;
	return v;
}

Val Val::of_int(int64_t value) {
	Val v = none(Type::integer);
	v.present = true;
	v.u.i = value;
	return v;
}

Val Val::of_real(double value) {
	Val v = none(Type::real);
	if (std::isfinite(value)) {
		v.present = true;
		v.u.r = value;
	}
	return v;
}

Val Val::of_duration(int64_t ms) {
	Val v = none(Type::duration);
	v.present = true;
	v.u.i = ms;
	return v;
}

Val Val::of_enum(uint32_t value) {
	Val v = none(Type::enumeration);
	v.present = true;
	v.u.e = value;
	return v;
}

Val Val::of_time(const TimeOfDay& value) {
	Val v = none(Type::time_of_day);
	v.present = true;
	v.u.tod = value;
	return v;
}

Val Val::of_date(const Date& value) {
	Val v = none(Type::date);
	v.present = true;
	v.u.date = value;
	return v;
}

Val Val::of_date_time(const DateTime& value) {
	Val v = none(Type::date_time);
	v.present = true;
	v.u.dt = value;
	return v;
}

Val Val::of_controlled(bool control, uint8_t value) {
	Val v = none(Type::controlled);
	v.present = true;
	v.u.ctl.control = control;
	v.u.ctl.value = value;
	return v;
}

Val Val::of_color(const Color& value) {
	Val v = none(Type::color);
	v.present = true;
	v.u.color = value;
	return v;
}

Val Val::of_string(const char* text, size_t length) {
	Val v = none(Type::string);
	if (length > kMaxString || std::memchr(text, '\0', length) != nullptr) {
		return v;
	}
	v.present = true;
	v.length = static_cast<uint8_t>(length);
	std::memcpy(v.u.bytes, text, length);
	v.u.bytes[length] = '\0';
	return v;
}

Val Val::of_raw(const uint8_t* bytes, size_t length) {
	Val v = none(Type::raw);
	if (length > kMaxRaw) {
		return v;
	}
	v.present = true;
	v.length = static_cast<uint8_t>(length);
	if (length > 0) {
		std::memcpy(v.u.bytes, bytes, length);
	}
	return v;
}

bool val_equal(const Val& a, const Val& b) {
	if (a.type != b.type || !a.present || !b.present) {
		return false;
	}
	switch (a.type) {
		case Type::boolean: return a.u.b == b.u.b;
		case Type::integer:
		case Type::duration: return a.u.i == b.u.i;
		case Type::real: return a.u.r == b.u.r;
		case Type::enumeration: return a.u.e == b.u.e;
		case Type::time_of_day:
			return a.u.tod.hour == b.u.tod.hour && a.u.tod.minute == b.u.tod.minute && a.u.tod.second == b.u.tod.second;
		case Type::date: return a.u.date == b.u.date;
		case Type::date_time:
			return a.u.dt.year == b.u.dt.year && a.u.dt.month == b.u.dt.month && a.u.dt.day == b.u.dt.day &&
				a.u.dt.hour == b.u.dt.hour && a.u.dt.minute == b.u.dt.minute && a.u.dt.second == b.u.dt.second;
		case Type::controlled: return a.u.ctl == b.u.ctl;
		case Type::color: return a.u.color == b.u.color;
		case Type::string:
		case Type::raw: return a.length == b.length && std::memcmp(a.u.bytes, b.u.bytes, a.length) == 0;
		case Type::invalid: break;
	}
	return false;
}

bool val_identical(const Val& a, const Val& b) {
	if (a.type != b.type || a.present != b.present) {
		return false;
	}
	if (!a.present) {
		return true;
	}
	switch (a.type) {
		case Type::time_of_day: return a.u.tod == b.u.tod;
		case Type::date_time: return a.u.dt == b.u.dt;
		default: return val_equal(a, b);
	}
}

Val val_from_datapoint(const Value& value) {
	switch (value.type()) {
		case ValueType::boolean: return Val::of_bool(value.as_bool());
		case ValueType::unsigned_int: return Val::of_int(value.as_unsigned());
		case ValueType::signed_int: return Val::of_int(value.as_signed());
		case ValueType::real: return Val::of_real(value.as_real());
		case ValueType::string: return Val::of_string(value.as_string(), value.length());
		case ValueType::time_of_day: return Val::of_time(value.as_time());
		case ValueType::date: return Val::of_date(value.as_date());
		case ValueType::date_time: return Val::of_date_time(value.as_date_time());
		case ValueType::enumeration: return Val::of_enum(value.as_enum());
		case ValueType::raw: return Val::of_raw(value.raw(), value.length());
		case ValueType::controlled: {
			Controlled c = value.as_controlled();
			return Val::of_controlled(c.control, c.value);
		}
		case ValueType::color: return Val::of_color(value.as_color());
		case ValueType::none: break;
	}
	return Val::none();
}

bool write_allowed(Type type, ValueType target) {
	switch (type) {
		case Type::boolean: return target == ValueType::boolean;
		case Type::integer:
		case Type::real:
			return target == ValueType::unsigned_int || target == ValueType::signed_int || target == ValueType::real ||
				target == ValueType::enumeration;
		case Type::enumeration:
			return target == ValueType::enumeration || target == ValueType::unsigned_int ||
				target == ValueType::signed_int;
		case Type::string: return target == ValueType::string;
		case Type::time_of_day: return target == ValueType::time_of_day;
		case Type::date: return target == ValueType::date;
		case Type::date_time: return target == ValueType::date_time;
		case Type::controlled: return target == ValueType::controlled;
		case Type::color: return target == ValueType::color;
		case Type::raw: return target == ValueType::raw;
		case Type::duration:
		case Type::invalid: break;
	}
	return false;
}

namespace {

std::optional<Value> integer_to(int64_t i, ValueType target) {
	switch (target) {
		case ValueType::unsigned_int:
			if (i >= 0 && i <= 4294967295LL) {
				return Value::of_unsigned(static_cast<uint32_t>(i));
			}
			break;
		case ValueType::enumeration:
			if (i >= 0 && i <= 4294967295LL) {
				return Value::of_enum(static_cast<uint32_t>(i));
			}
			break;
		case ValueType::signed_int:
			if (i >= INT32_MIN && i <= INT32_MAX) {
				return Value::of_signed(static_cast<int32_t>(i));
			}
			break;
		case ValueType::real:
			if (i >= -(int64_t(1) << 53) && i <= (int64_t(1) << 53)) {
				return Value::of_real(static_cast<double>(i));
			}
			break;
		default: break;
	}
	return std::nullopt;
}

} // namespace

std::optional<Value> convert_for_datapoint(const Val& v, ValueType target) {
	if (!v.present || !write_allowed(v.type, target)) {
		return std::nullopt;
	}
	switch (v.type) {
		case Type::boolean: return Value::of_bool(v.u.b);
		case Type::integer: return integer_to(v.u.i, target);
		case Type::real:
			if (target == ValueType::real) {
				return Value::of_real(v.u.r);
			}
			if (integral(v.u.r) && std::fabs(v.u.r) < 9.2e18) {
				return integer_to(static_cast<int64_t>(v.u.r), target);
			}
			return std::nullopt;
		case Type::enumeration:
			if (target == ValueType::enumeration) {
				return Value::of_enum(v.u.e);
			}
			return integer_to(v.u.e, target);
		case Type::string: return Value::of_string(v.u.bytes, v.length);
		case Type::time_of_day: return Value::of_time(v.u.tod);
		case Type::date: return Value::of_date(v.u.date);
		case Type::date_time: return Value::of_date_time(v.u.dt);
		case Type::controlled: return Value::of_controlled(v.u.ctl.control, v.u.ctl.value);
		case Type::color: return Value::of_color(v.u.color);
		case Type::raw: return Value::of_raw(reinterpret_cast<const uint8_t*>(v.u.bytes), v.length);
		case Type::duration:
		case Type::invalid: break;
	}
	return std::nullopt;
}

bool assign_allowed(Type type, Type target) {
	switch (target) {
		case Type::integer:
		case Type::real: return type == Type::integer || type == Type::real;
		case Type::boolean:
		case Type::string:
		case Type::duration:
		case Type::time_of_day:
		case Type::enumeration:
		case Type::color: return type == target;
		default: return false;
	}
}

bool convert_for_parameter(const Val& v, Type target, Val& out) {
	if (!v.present || !assign_allowed(v.type, target)) {
		return false;
	}
	if (v.type == target) {
		out = v;
		return true;
	}
	if (v.type == Type::integer && target == Type::real) {
		if (v.u.i < -(int64_t(1) << 53) || v.u.i > (int64_t(1) << 53)) {
			return false;
		}
		out = Val::of_real(static_cast<double>(v.u.i));
		return true;
	}
	if (v.type == Type::real && target == Type::integer) {
		if (!integral(v.u.r) || std::fabs(v.u.r) >= 9.2e18) {
			return false;
		}
		out = Val::of_int(static_cast<int64_t>(v.u.r));
		return true;
	}
	return false;
}

LiteralStatus val_from_json(Type type, const JsonValue& json, Val& out, const char** reason) {
	const char* dummy = nullptr;
	const char*& why = reason ? *reason : dummy;
	why = nullptr;
	out = Val::none(type);
	switch (type) {
		case Type::boolean: {
			bool b = false;
			if (!json.get_bool(b)) {
				why = "a boolean literal must be true or false";
				return LiteralStatus::malformed;
			}
			out = Val::of_bool(b);
			return LiteralStatus::ok;
		}
		case Type::integer:
		case Type::duration: {
			double number = 0;
			if (!json.get_number(number)) {
				why = type == Type::integer ? "an integer literal must be a number" : "a duration must be a number of ms";
				return LiteralStatus::malformed;
			}
			if (!integral(number) || std::fabs(number) > kMaxSafeInteger) {
				why = "not an integer within 2^53-1";
				return LiteralStatus::invalid;
			}
			out = type == Type::integer ? Val::of_int(static_cast<int64_t>(number))
										: Val::of_duration(static_cast<int64_t>(number));
			return LiteralStatus::ok;
		}
		case Type::real: {
			double number = 0;
			if (!json.get_number(number)) {
				why = "a real literal must be a number";
				return LiteralStatus::malformed;
			}
			out = Val::of_real(number);
			if (!out.present) {
				why = "not a finite real";
				return LiteralStatus::invalid;
			}
			return LiteralStatus::ok;
		}
		case Type::enumeration: {
			double number = 0;
			if (!json.get_number(number)) {
				why = "an enumeration literal must be a number";
				return LiteralStatus::malformed;
			}
			if (!integral(number) || number < 0 || number > 4294967295.0) {
				why = "an enumeration element must be 0..4294967295";
				return LiteralStatus::invalid;
			}
			out = Val::of_enum(static_cast<uint32_t>(number));
			return LiteralStatus::ok;
		}
		case Type::string: {
			if (!json.is(JsonKind::string)) {
				why = "a string literal must be a JSON string";
				return LiteralStatus::malformed;
			}
			char text[kMaxString + 1];
			size_t length = 0;
			if (!json.get_string(text, sizeof(text), &length)) {
				why = "a string is longer than 28 bytes or contains a NUL";
				return LiteralStatus::invalid;
			}
			out = Val::of_string(text, length);
			return LiteralStatus::ok;
		}
		case Type::raw: {
			if (!json.is(JsonKind::string)) {
				why = "a raw literal must be a string of hex octets";
				return LiteralStatus::malformed;
			}
			char text[3 * kMaxRaw + 2];
			size_t length = 0;
			if (!json.get_string(text, sizeof(text), &length)) {
				why = "a raw value is longer than 14 bytes";
				return LiteralStatus::invalid;
			}
			uint8_t bytes[kMaxRaw];
			size_t count = 0;
			size_t i = 0;
			while (i < length) {
				if (count == kMaxRaw || i + 1 >= length || hex_digit(text[i]) < 0 || hex_digit(text[i + 1]) < 0) {
					why = "raw octets must be two hex digits separated by single spaces, at most 14";
					return LiteralStatus::invalid;
				}
				bytes[count++] = static_cast<uint8_t>(hex_digit(text[i]) * 16 + hex_digit(text[i + 1]));
				i += 2;
				if (i < length) {
					if (text[i] != ' ' || i + 1 == length) {
						why = "raw octets must be two hex digits separated by single spaces, at most 14";
						return LiteralStatus::invalid;
					}
					++i;
				}
			}
			out = Val::of_raw(bytes, count);
			return LiteralStatus::ok;
		}
		case Type::time_of_day: {
			static const char* const kMembers[] = {"hour", "minute", "second", "dayOfWeek", nullptr};
			if (!json.is(JsonKind::object) || !only_members(json, kMembers)) {
				why = "a time of day is {\"hour\", \"minute\", \"second\"} with an optional \"dayOfWeek\"";
				return LiteralStatus::malformed;
			}
			int64_t h = 0, m = 0, s = 0, d = 0;
			LiteralStatus st;
			if ((st = int_member(json, "hour", 0, 23, true, h)) != LiteralStatus::ok ||
				(st = int_member(json, "minute", 0, 59, true, m)) != LiteralStatus::ok ||
				(st = int_member(json, "second", 0, 59, true, s)) != LiteralStatus::ok ||
				(st = int_member(json, "dayOfWeek", 0, 7, false, d)) != LiteralStatus::ok) {
				why = st == LiteralStatus::invalid ? "a time of day field out of range" : "a time of day field missing";
				return st;
			}
			TimeOfDay t;
			t.hour = static_cast<uint8_t>(h);
			t.minute = static_cast<uint8_t>(m);
			t.second = static_cast<uint8_t>(s);
			t.day_of_week = static_cast<uint8_t>(d);
			out = Val::of_time(t);
			return LiteralStatus::ok;
		}
		case Type::date:
		case Type::date_time: {
			static const char* const kDateMembers[] = {"year", "month", "day", nullptr};
			static const char* const kDateTimeMembers[] = {"year", "month", "day", "hour", "minute", "second", nullptr};
			bool with_time = type == Type::date_time;
			if (!json.is(JsonKind::object) || !only_members(json, with_time ? kDateTimeMembers : kDateMembers)) {
				why = with_time ? "a date-time is {\"year\", \"month\", \"day\", \"hour\", \"minute\", \"second\"}"
								: "a date is {\"year\", \"month\", \"day\"}";
				return LiteralStatus::malformed;
			}
			int64_t y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
			LiteralStatus st;
			if ((st = int_member(json, "year", 1900, 2155, true, y)) != LiteralStatus::ok ||
				(st = int_member(json, "month", 1, 12, true, mo)) != LiteralStatus::ok ||
				(st = int_member(json, "day", 1, 31, true, d)) != LiteralStatus::ok ||
				(with_time && ((st = int_member(json, "hour", 0, 23, true, h)) != LiteralStatus::ok ||
					(st = int_member(json, "minute", 0, 59, true, mi)) != LiteralStatus::ok ||
					(st = int_member(json, "second", 0, 59, true, s)) != LiteralStatus::ok))) {
				why = st == LiteralStatus::invalid ? "a date field out of range" : "a date field missing";
				return st;
			}
			if (!valid_date(static_cast<int32_t>(y), static_cast<unsigned>(mo), static_cast<unsigned>(d))) {
				why = "not a valid date";
				return LiteralStatus::invalid;
			}
			if (!with_time) {
				Date date;
				date.year = static_cast<uint16_t>(y);
				date.month = static_cast<uint8_t>(mo);
				date.day = static_cast<uint8_t>(d);
				out = Val::of_date(date);
			} else {
				DateTime dt;
				dt.year = static_cast<uint16_t>(y);
				dt.month = static_cast<uint8_t>(mo);
				dt.day = static_cast<uint8_t>(d);
				dt.hour = static_cast<uint8_t>(h);
				dt.minute = static_cast<uint8_t>(mi);
				dt.second = static_cast<uint8_t>(s);
				dt.day_of_week = weekday_of(dt.year, dt.month, dt.day);
				dt.flags = 0;
				out = Val::of_date_time(dt);
			}
			return LiteralStatus::ok;
		}
		case Type::controlled: {
			static const char* const kMembers[] = {"control", "value", nullptr};
			JsonValue control;
			bool bit = false;
			if (!json.is(JsonKind::object) || !only_members(json, kMembers) || !json.member("control", control) ||
				!control.get_bool(bit)) {
				why = "a controlled value is {\"control\": bool, \"value\": n}";
				return LiteralStatus::malformed;
			}
			int64_t n = 0;
			LiteralStatus st = int_member(json, "value", 0, 255, true, n);
			if (st != LiteralStatus::ok) {
				why = "the number of a controlled value must be 0..255";
				return st;
			}
			out = Val::of_controlled(bit, static_cast<uint8_t>(n));
			return LiteralStatus::ok;
		}
		case Type::color: {
			static const char* const kMembers[] = {"red", "green", "blue", "white", nullptr};
			if (!json.is(JsonKind::object) || !only_members(json, kMembers)) {
				why = "a color is {\"red\", \"green\", \"blue\"} with an optional \"white\"";
				return LiteralStatus::malformed;
			}
			int64_t r = 0, g = 0, b = 0, w = 0;
			bool has_white = false;
			LiteralStatus st;
			if ((st = int_member(json, "red", 0, 255, true, r)) != LiteralStatus::ok ||
				(st = int_member(json, "green", 0, 255, true, g)) != LiteralStatus::ok ||
				(st = int_member(json, "blue", 0, 255, true, b)) != LiteralStatus::ok ||
				(st = int_member(json, "white", 0, 255, false, w, &has_white)) != LiteralStatus::ok) {
				why = st == LiteralStatus::invalid ? "a color level must be 0..255" : "a color level missing";
				return st;
			}
			Color c;
			c.red = static_cast<uint8_t>(r);
			c.green = static_cast<uint8_t>(g);
			c.blue = static_cast<uint8_t>(b);
			c.white = static_cast<uint8_t>(w);
			c.valid = has_white ? Color::kAll : Color::kRgb;
			out = Val::of_color(c);
			return LiteralStatus::ok;
		}
		case Type::invalid: break;
	}
	why = "unknown type";
	return LiteralStatus::malformed;
}

// ------------------------------------------------------------------------------------------
// Text

namespace {

/// Appends a real rounded to 6 decimals, ties away from zero (spec 4.9), computed exactly from the
/// binary value.
void put_real(Out& out, double value) {
	if (!std::isfinite(value)) {
		out.put("none");
		return;
	}
	bool negative = std::signbit(value);
	double magnitude = std::fabs(value);
	// Decimal digits of round(magnitude * 10^6), least significant first.
	char reversed[336];
	size_t count = 0;
	if (magnitude != 0) {
		int exponent = 0;
		double fraction = std::frexp(magnitude, &exponent); // magnitude = fraction * 2^exponent
		uint64_t mantissa = static_cast<uint64_t>(std::ldexp(fraction, 53));
		int shift = exponent - 53; // magnitude = mantissa * 2^shift exactly
		BigInt n(mantissa);
		n.mul_small(1000000);
		if (shift >= 0) {
			n.shift_left(static_cast<size_t>(shift));
		} else {
			size_t right = static_cast<size_t>(-shift);
			bool round_up = n.bit(right - 1); // the first dropped bit: a half or more rounds away from zero
			n.shift_right(right);
			if (round_up) {
				n.add_one();
			}
		}
		while (!n.zero() && count + 9 <= sizeof(reversed)) {
			uint32_t group = n.div_small(1000000000u);
			for (int i = 0; i < 9; ++i) {
				reversed[count++] = static_cast<char>('0' + group % 10);
				group /= 10;
			}
		}
		while (count > 0 && reversed[count - 1] == '0') {
			--count;
		}
	}
	if (count == 0) {
		out.put('0');
		return;
	}
	size_t total = count < 7 ? 7 : count;
	auto digit = [&](size_t k) { // k-th digit from the most significant
		size_t index = total - 1 - k;
		return index < count ? reversed[index] : '0';
	};
	size_t integer_digits = total - 6;
	size_t fraction_end = total;
	while (fraction_end > integer_digits && digit(fraction_end - 1) == '0') {
		--fraction_end;
	}
	if (negative) {
		out.put('-');
	}
	for (size_t k = 0; k < integer_digits; ++k) {
		out.put(digit(k));
	}
	if (fraction_end > integer_digits) {
		out.put('.');
		for (size_t k = integer_digits; k < fraction_end; ++k) {
			out.put(digit(k));
		}
	}
}

} // namespace

size_t format_real(double value, char* buffer, size_t size) {
	Out out(buffer, size);
	put_real(out, value);
	return out.length;
}

size_t real_literal_text(double value, char* buffer, size_t size) {
	Out out(buffer, size);
	if (!std::isfinite(value)) {
		out.put("none");
		return out.length;
	}
	if (std::signbit(value)) {
		out.put('-');
	}
	double magnitude = std::fabs(value);
	if (magnitude == 0) {
		out.put("0.0");
		return out.length;
	}
	char text[40];
	char digits[24];
	size_t count = 0;
	int exponent = 0;
	for (int precision = 1; precision <= 17; ++precision) {
		std::snprintf(text, sizeof(text), "%.*e", precision - 1, magnitude);
		if (std::strtod(text, nullptr) == magnitude || precision == 17) {
			break;
		}
	}
	// text is "d.ddde[+-]xx"
	const char* p = text;
	for (; *p && *p != 'e'; ++p) {
		if (*p >= '0' && *p <= '9' && count < sizeof(digits)) {
			digits[count++] = *p;
		}
	}
	if (*p == 'e') {
		exponent = std::atoi(p + 1);
	}
	while (count > 1 && digits[count - 1] == '0') {
		--count;
	}
	if (magnitude >= 1e-4 && magnitude < 1e15) {
		if (exponent >= 0) {
			size_t integer_digits = static_cast<size_t>(exponent) + 1;
			for (size_t i = 0; i < integer_digits; ++i) {
				out.put(i < count ? digits[i] : '0');
			}
			out.put('.');
			if (count > integer_digits) {
				out.put(digits + integer_digits, count - integer_digits);
			} else {
				out.put('0');
			}
		} else {
			out.put("0.");
			for (int i = 0; i < -exponent - 1; ++i) {
				out.put('0');
			}
			out.put(digits, count);
		}
	} else {
		out.put(digits[0]);
		out.put('.');
		if (count > 1) {
			out.put(digits + 1, count - 1);
		} else {
			out.put('0');
		}
		out.printf("e%d", exponent);
	}
	return out.length;
}

size_t duration_text(int64_t ms, char* buffer, size_t size) {
	Out out(buffer, size);
	if (ms == 0) {
		out.put("0s");
		return out.length;
	}
	uint64_t rest = ms < 0 ? static_cast<uint64_t>(0) - static_cast<uint64_t>(ms) : static_cast<uint64_t>(ms);
	if (ms < 0) {
		out.put('-');
	}
	static const struct {
		uint64_t ms;
		const char* unit;
	} kUnits[] = {{86400000ull, "d"}, {3600000ull, "h"}, {60000ull, "min"}, {1000ull, "s"}, {1ull, "ms"}};
	for (const auto& unit : kUnits) {
		uint64_t count = rest / unit.ms;
		rest %= unit.ms;
		if (count > 0) {
			out.printf("%" PRIu64 "%s", count, unit.unit);
		}
	}
	return out.length;
}

size_t format_val(const Val& v, char* buffer, size_t size) {
	Out out(buffer, size);
	if (!v.present) {
		out.put("none");
		return out.length;
	}
	switch (v.type) {
		case Type::real: put_real(out, v.u.r); break;
		case Type::string: out.put(v.u.bytes, v.length); break;
		default: put_literal_common(out, v); break;
	}
	return out.length;
}

size_t literal_text(const Val& v, char* buffer, size_t size) {
	Out out(buffer, size);
	if (!v.present) {
		out.put("none");
		return out.length;
	}
	switch (v.type) {
		case Type::real: {
			char text[48];
			real_literal_text(v.u.r, text, sizeof(text));
			out.put(text);
			break;
		}
		case Type::string: put_quoted(out, v.u.bytes, v.length); break;
		default: put_literal_common(out, v); break;
	}
	return out.length;
}

// ------------------------------------------------------------------------------------------
// Calendar

int64_t days_of(int32_t year, unsigned month, unsigned day) { return days_from_civil(year, month, day); }

void civil_of(int64_t days, int32_t& year, unsigned& month, unsigned& day) {
	// Howard Hinnant's civil_from_days.
	int64_t z = days + 719468;
	int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	int64_t doe = z - era * 146097;
	int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int64_t mp = (5 * doy + 2) / 153;
	day = static_cast<unsigned>(doy - (153 * mp + 2) / 5 + 1);
	month = static_cast<unsigned>(mp < 10 ? mp + 3 : mp - 9);
	year = static_cast<int32_t>(yoe + era * 400 + (month <= 2 ? 1 : 0));
}

uint8_t weekday_of(int32_t year, unsigned month, unsigned day) {
	int64_t days = days_of(year, month, day); // 1970-01-01 was a Thursday (4)
	int64_t index = (days + 3) % 7;
	if (index < 0) {
		index += 7;
	}
	return static_cast<uint8_t>(index + 1);
}

bool valid_date(int32_t year, unsigned month, unsigned day) {
	if (month < 1 || month > 12 || day < 1) {
		return false;
	}
	static const unsigned kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	unsigned limit = kDays[month - 1];
	if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
		limit = 29;
	}
	return day <= limit;
}

} // namespace hc::rules
