/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/dpt.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hc {

namespace {

/// How a datapoint type is encoded: one codec per family of types that share the bits and the
/// value type. The KNX standard (03_07_02 Datapoint Types) defines each.
enum class Codec : uint8_t {
	unsupported,
	boolean,        ///< 1.x: B1, embedded.
	control,        ///< 2.x: B2 (c, v), embedded.
	step,           ///< 3.x: B1U3 (c, step code), embedded.
	char_ascii,     ///< 4.001: A8, 0..127.
	char_latin1,    ///< 4.002: A8, ISO 8859-1.
	u8,             ///< 5.x: U8 0..255.
	u8_percent,     ///< 5.001: U8, 0..100 % scaled to 0..255.
	u8_angle,       ///< 5.003: U8, 0..360 degrees scaled to 0..255.
	u8_tariff,      ///< 5.006: U8 0..254.
	s8,             ///< 6.x: V8.
	u16,            ///< 7.x: U16.
	u16_x10,        ///< 7.003: U16 in units of 10 ms, value in ms.
	u16_x100,       ///< 7.004: U16 in units of 100 ms, value in ms.
	s16,            ///< 8.x: V16.
	s16_x10,        ///< 8.003: V16 in units of 10 ms, value in ms.
	s16_x100,       ///< 8.004: V16 in units of 100 ms, value in ms.
	s16_percent,    ///< 8.010: V16 in units of 0.01 %.
	float16,        ///< 9.x: F16, (0.01 * M) * 2^E.
	time_of_day,    ///< 10.001: N3 U5 r2 U6 r2 U6.
	date,           ///< 11.001: r3 U5 (day) r4 U4 (month) r1 U7 (year).
	u32,            ///< 12.x: U32.
	s32,            ///< 13.x: V32.
	float32,        ///< 14.x: IEEE 754 single.
	string_ascii,   ///< 16.000: A112, ASCII.
	string_latin1,  ///< 16.001: A112, ISO 8859-1.
	scene,          ///< 17.001: r2 U6.
	scene_control,  ///< 18.001: B1 r1 U6.
	date_time,      ///< 19.001: 8 octets.
	enum8,          ///< 20.x: N8.
	rgb,            ///< 232.600: U8 U8 U8.
	rgbw,           ///< 251.600: U8 U8 U8 U8 r8 r4 B4.
};

/// The codec of a type and, for an unsupported one, why.
struct Rule {
	Codec codec;
	const char* unsupported = nullptr;
};

Rule rule_of(Dpt dpt) {
	switch (dpt.main) {
		case 1: return {Codec::boolean};
		case 2: return {Codec::control};
		case 3: return {Codec::step};
		case 4: return {dpt.sub == 2 ? Codec::char_latin1 : Codec::char_ascii};
		case 5:
			switch (dpt.sub) {
				case 1: return {Codec::u8_percent};
				case 3: return {Codec::u8_angle};
				case 6: return {Codec::u8_tariff};
				default: return {Codec::u8};
			}
		case 6:
			if (dpt.sub == 20) {
				return {Codec::unsupported, "6.020 (status with mode) is not implemented"};
			}
			return {Codec::s8};
		case 7:
			switch (dpt.sub) {
				case 3: return {Codec::u16_x10};
				case 4: return {Codec::u16_x100};
				default: return {Codec::u16};
			}
		case 8:
			switch (dpt.sub) {
				case 3: return {Codec::s16_x10};
				case 4: return {Codec::s16_x100};
				case 10: return {Codec::s16_percent};
				default: return {Codec::s16};
			}
		case 9: return {Codec::float16};
		case 10: return {Codec::time_of_day};
		case 11: return {Codec::date};
		case 12: return {Codec::u32};
		case 13: return {Codec::s32};
		case 14: return {Codec::float32};
		case 16: return {dpt.sub == 1 ? Codec::string_latin1 : Codec::string_ascii};
		case 17: return {Codec::scene};
		case 18: return {Codec::scene_control};
		case 19: return {Codec::date_time};
		case 20: return {Codec::enum8};
		case 232:
			if (dpt.sub == 0 || dpt.sub == 600) {
				return {Codec::rgb};
			}
			return {Codec::unsupported, "of main type 232 only 232.600 (RGB) is implemented"};
		case 251:
			if (dpt.sub == 0 || dpt.sub == 600) {
				return {Codec::rgbw};
			}
			return {Codec::unsupported, "of main type 251 only 251.600 (RGBW) is implemented"};
		default: return {Codec::unsupported, "main type not implemented"};
	}
}

size_t size_of(Codec codec) {
	switch (codec) {
		case Codec::unsupported: return 0;
		case Codec::boolean:
		case Codec::control:
		case Codec::step:
		case Codec::char_ascii:
		case Codec::char_latin1:
		case Codec::u8:
		case Codec::u8_percent:
		case Codec::u8_angle:
		case Codec::u8_tariff:
		case Codec::s8:
		case Codec::scene:
		case Codec::scene_control:
		case Codec::enum8: return 1;
		case Codec::u16:
		case Codec::u16_x10:
		case Codec::u16_x100:
		case Codec::s16:
		case Codec::s16_x10:
		case Codec::s16_x100:
		case Codec::s16_percent:
		case Codec::float16: return 2;
		case Codec::time_of_day:
		case Codec::date:
		case Codec::rgb: return 3;
		case Codec::u32:
		case Codec::s32:
		case Codec::float32: return 4;
		case Codec::rgbw: return 6;
		case Codec::date_time: return 8;
		case Codec::string_ascii:
		case Codec::string_latin1: return 14;
	}
	return 0;
}

ValueType value_type_of(Codec codec) {
	switch (codec) {
		case Codec::unsupported: return ValueType::none;
		case Codec::boolean: return ValueType::boolean;
		case Codec::control:
		case Codec::step:
		case Codec::scene_control: return ValueType::controlled;
		case Codec::char_ascii:
		case Codec::char_latin1:
		case Codec::string_ascii:
		case Codec::string_latin1: return ValueType::string;
		case Codec::u8:
		case Codec::u8_tariff:
		case Codec::u16:
		case Codec::u16_x10:
		case Codec::u16_x100:
		case Codec::u32:
		case Codec::scene: return ValueType::unsigned_int;
		case Codec::s8:
		case Codec::s16:
		case Codec::s16_x10:
		case Codec::s16_x100:
		case Codec::s32: return ValueType::signed_int;
		case Codec::u8_percent:
		case Codec::u8_angle:
		case Codec::s16_percent:
		case Codec::float16:
		case Codec::float32: return ValueType::real;
		case Codec::time_of_day: return ValueType::time_of_day;
		case Codec::date: return ValueType::date;
		case Codec::date_time: return ValueType::date_time;
		case Codec::enum8: return ValueType::enumeration;
		case Codec::rgb:
		case Codec::rgbw: return ValueType::color;
	}
	return ValueType::none;
}

constexpr DptResult kOk{DptStatus::ok, ""};

DptResult refuse(DptStatus status, const char* reason) { return DptResult{status, reason}; }

/// Rounds to the nearest integer, a half up (towards positive infinity): the rule of the reference
/// implementation (Java's Math.round), exact for every double.
double round_half_up(double x) {
	double r = std::floor(x);
	return x - r >= 0.5 ? r + 1 : r;
}

/// The number a numeric value holds (unsigned, signed, real, enumeration).
bool number_of(const Value& value, double& out) {
	switch (value.type()) {
		case ValueType::unsigned_int: out = value.as_unsigned(); return true;
		case ValueType::signed_int: out = value.as_signed(); return true;
		case ValueType::real: out = value.as_real(); return true;
		case ValueType::enumeration: out = value.as_enum(); return true;
		default: return false;
	}
}

/// An integer within [min, max] from a numeric value.
DptResult integer_of(const Value& value, double min, double max, const char* range, int64_t& out) {
	double number = 0;
	if (!number_of(value, number)) {
		return refuse(DptStatus::wrong_type, "a number is required");
	}
	if (std::floor(number) != number) {
		return refuse(DptStatus::wrong_type, "an integer is required");
	}
	if (number < min || number > max) {
		return refuse(DptStatus::out_of_range, range);
	}
	out = static_cast<int64_t>(number);
	return kOk;
}

/// A real within [min, max] from a numeric value.
DptResult real_of(const Value& value, double min, double max, const char* range, double& out) {
	if (!number_of(value, out)) {
		return refuse(DptStatus::wrong_type, "a number is required");
	}
	if (!(out >= min && out <= max)) {
		return refuse(DptStatus::out_of_range, range);
	}
	return kOk;
}

DptResult require(const Value& value, ValueType type) {
	if (value.type() != type) {
		static const char* const kRequired[] = {
			"a value is required", "a boolean is required", "an unsigned integer is required",
			"a signed integer is required", "a real is required", "a string is required",
			"a time of day is required", "a date is required", "a date-time is required",
			"an enumeration is required", "raw bytes are required", "a controlled value is required",
			"a color is required"};
		return refuse(DptStatus::wrong_type, kRequired[static_cast<uint8_t>(type)]);
	}
	return kOk;
}

void put16(uint8_t* out, uint32_t v) {
	out[0] = static_cast<uint8_t>(v >> 8);
	out[1] = static_cast<uint8_t>(v);
}

void put32(uint8_t* out, uint32_t v) {
	out[0] = static_cast<uint8_t>(v >> 24);
	out[1] = static_cast<uint8_t>(v >> 16);
	out[2] = static_cast<uint8_t>(v >> 8);
	out[3] = static_cast<uint8_t>(v);
}

uint32_t get16(const uint8_t* in) { return static_cast<uint32_t>(in[0]) << 8 | in[1]; }

uint32_t get32(const uint8_t* in) {
	return static_cast<uint32_t>(in[0]) << 24 | static_cast<uint32_t>(in[1]) << 16 |
		static_cast<uint32_t>(in[2]) << 8 | in[3];
}

// --- 9.x: the 2-octet float -------------------------------------------------------------------

constexpr double kFloat16Max = 670433.28;     ///< 0x7FFE: 2046 * 2^15 * 0.01 (0x7FFF is invalid data).
constexpr double kFloat16Min = -671088.64;    ///< 0xF800: -2048 * 2^15 * 0.01.

/// The lower range limit of the 9.x subtypes whose quantity cannot be negative or lies above
/// absolute zero; every other subtype has the full range.
double float16_min(uint16_t sub, const char*& range) {
	switch (sub) {
		case 1:
			range = "outside -273..670433.28";
			return -273;
		case 4:
		case 5:
		case 6:
		case 7:
		case 8:
		case 28:
		case 29:
		case 30:
			range = "outside 0..670433.28";
			return 0;
		case 27:
			range = "outside -459.6..670433.28";
			return -459.6;
		default:
			range = "outside -671088.64..670433.28";
			return kFloat16Min;
	}
}

/// value = 0.01 * M * 2^E with M a 12-bit two's complement mantissa and E 0..15. The exponent is the
/// smallest for which the hundredfold value, halved E times, lies within -2048..2047 before
/// rounding (the rule of the reference implementation); the sign bit is the mantissa's.
void encode_float16(double value, uint8_t* out) {
	double v = value * 100.0;
	unsigned e = 0;
	while (v < -2048.0) {
		v /= 2;
		++e;
	}
	while (v > 2047.0) {
		v /= 2;
		++e;
	}
	int32_t m = static_cast<int32_t>(round_half_up(v));
	uint32_t bits = (m < 0 ? 0x8000u : 0u) | e << 11 | (static_cast<uint32_t>(m) & 0x7FFu);
	put16(out, bits);
}

double decode_float16(uint32_t bits) {
	int32_t m = static_cast<int32_t>(bits & 0x7FF);
	if ((bits & 0x8000) != 0) {
		m -= 2048;
	}
	unsigned e = (bits >> 11) & 0x0F;
	return static_cast<double>(m * (1 << e)) * 0.01;
}

// --- Strings --------------------------------------------------------------------------------

/// Reads the next code point of UTF-8 text; false at invalid UTF-8 (overlong forms, surrogates).
bool next_code_point(const char* text, size_t length, size_t& pos, uint32_t& cp) {
	uint8_t b = static_cast<uint8_t>(text[pos]);
	size_t extra = 0;
	uint32_t min = 0;
	if (b < 0x80) {
		cp = b;
	} else if ((b & 0xE0) == 0xC0) {
		cp = b & 0x1F;
		extra = 1;
		min = 0x80;
	} else if ((b & 0xF0) == 0xE0) {
		cp = b & 0x0F;
		extra = 2;
		min = 0x800;
	} else if ((b & 0xF8) == 0xF0) {
		cp = b & 0x07;
		extra = 3;
		min = 0x10000;
	} else {
		return false;
	}
	if (pos + extra >= length && extra > 0) {
		return false; // Truncated sequence.
	}
	for (size_t i = 1; i <= extra; ++i) {
		uint8_t c = static_cast<uint8_t>(text[pos + i]);
		if ((c & 0xC0) != 0x80) {
			return false;
		}
		cp = cp << 6 | (c & 0x3F);
	}
	if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
		return false;
	}
	pos += extra + 1;
	return true;
}

/// Encodes a string value into `capacity` single-byte characters up to `max_char` (0x7F ASCII,
/// 0xFF Latin-1), the rest NUL.
DptResult encode_chars(const Value& value, uint32_t max_char, size_t capacity, uint8_t* out) {
	DptResult result = require(value, ValueType::string);
	if (!result) {
		return result;
	}
	const char* text = value.as_string();
	size_t length = value.length();
	size_t pos = 0;
	size_t count = 0;
	std::memset(out, 0, capacity);
	while (pos < length) {
		uint32_t cp = 0;
		if (!next_code_point(text, length, pos, cp)) {
			std::memset(out, 0, capacity);
			return refuse(DptStatus::wrong_type, "the string is no valid UTF-8");
		}
		if (count == capacity) {
			std::memset(out, 0, capacity);
			return refuse(DptStatus::out_of_range,
				capacity == 1 ? "more than one character" : "longer than 14 characters");
		}
		if (cp > max_char) {
			std::memset(out, 0, capacity);
			return refuse(DptStatus::out_of_range,
				max_char == 0x7F ? "a character outside ASCII" : "a character outside ISO 8859-1");
		}
		out[count++] = static_cast<uint8_t>(cp);
	}
	return kOk;
}

/// Decodes single-byte characters up to the first NUL into a UTF-8 string value.
DptResult decode_chars(const uint8_t* data, size_t length, bool latin1, Value& value) {
	char text[Value::kMaxString + 1];
	size_t used = 0;
	for (size_t i = 0; i < length && data[i] != 0; ++i) {
		uint8_t c = data[i];
		if (c < 0x80) {
			text[used++] = static_cast<char>(c);
		} else if (!latin1) {
			return refuse(DptStatus::invalid, "a character outside ASCII");
		} else {
			text[used++] = static_cast<char>(0xC0 | c >> 6);
			text[used++] = static_cast<char>(0x80 | (c & 0x3F));
		}
	}
	value = *Value::of_string(text, used);
	return kOk;
}

// --- Enumerations ---------------------------------------------------------------------------

/// Whether `element` belongs to the enumeration of the 20.x subtype. A subtype without a table here
/// accepts every element (its value set is not known to the core).
bool enum_element_valid(uint16_t sub, uint32_t element) {
	switch (sub) {
		case 102: return element <= 4;                     // HVAC mode: auto, comfort, standby, economy, protection.
		case 105: return element <= 17 || element == 20;   // HVAC control mode (18, 19 reserved).
		default: return element <= 255;
	}
}

// --- Encoding -------------------------------------------------------------------------------

DptResult encode_value(Codec codec, uint16_t sub, const Value& value, uint8_t* out) {
	int64_t n = 0;
	double r = 0;
	DptResult result;
	switch (codec) {
		case Codec::unsupported: return refuse(DptStatus::unsupported, "unsupported");

		case Codec::boolean:
			if (!(result = require(value, ValueType::boolean))) {
				return result;
			}
			out[0] = value.as_bool() ? 1 : 0;
			return kOk;

		case Codec::control:
		case Codec::step:
		case Codec::scene_control: {
			if (!(result = require(value, ValueType::controlled))) {
				return result;
			}
			Controlled c = value.as_controlled();
			if (codec == Codec::control) {
				if (c.value > 1) {
					return refuse(DptStatus::out_of_range, "value outside 0..1");
				}
				out[0] = static_cast<uint8_t>((c.control ? 0x02 : 0) | c.value);
			} else if (codec == Codec::step) {
				if (c.value > 7) {
					return refuse(DptStatus::out_of_range, "step code outside 0..7");
				}
				out[0] = static_cast<uint8_t>((c.control ? 0x08 : 0) | c.value);
			} else {
				if (c.value > 63) {
					return refuse(DptStatus::out_of_range, "scene number outside 0..63");
				}
				out[0] = static_cast<uint8_t>((c.control ? 0x80 : 0) | c.value);
			}
			return kOk;
		}

		case Codec::char_ascii: return encode_chars(value, 0x7F, 1, out);
		case Codec::char_latin1: return encode_chars(value, 0xFF, 1, out);
		case Codec::string_ascii: return encode_chars(value, 0x7F, 14, out);
		case Codec::string_latin1: return encode_chars(value, 0xFF, 14, out);

		case Codec::u8:
			if (!(result = integer_of(value, 0, 255, "outside 0..255", n))) {
				return result;
			}
			out[0] = static_cast<uint8_t>(n);
			return kOk;
		case Codec::u8_tariff:
			if (!(result = integer_of(value, 0, 254, "outside 0..254", n))) {
				return result;
			}
			out[0] = static_cast<uint8_t>(n);
			return kOk;
		case Codec::u8_percent:
			if (!(result = real_of(value, 0, 100, "outside 0..100 %", r))) {
				return result;
			}
			out[0] = static_cast<uint8_t>(round_half_up(r * 255 / 100));
			return kOk;
		case Codec::u8_angle:
			if (!(result = real_of(value, 0, 360, "outside 0..360 degrees", r))) {
				return result;
			}
			out[0] = static_cast<uint8_t>(round_half_up(r * 255 / 360));
			return kOk;
		case Codec::s8:
			if (!(result = integer_of(value, -128, 127, "outside -128..127", n))) {
				return result;
			}
			out[0] = static_cast<uint8_t>(n);
			return kOk;
		case Codec::scene:
			if (!(result = integer_of(value, 0, 63, "scene number outside 0..63", n))) {
				return result;
			}
			out[0] = static_cast<uint8_t>(n);
			return kOk;
		case Codec::enum8:
			if (!(result = integer_of(value, 0, 255, "element outside 0..255", n))) {
				return result;
			}
			if (!enum_element_valid(sub, static_cast<uint32_t>(n))) {
				return refuse(DptStatus::out_of_range, "no element of the enumeration");
			}
			out[0] = static_cast<uint8_t>(n);
			return kOk;

		case Codec::u16:
			if (!(result = integer_of(value, 0, 65535, "outside 0..65535", n))) {
				return result;
			}
			put16(out, static_cast<uint32_t>(n));
			return kOk;
		case Codec::u16_x10:
			if (!(result = integer_of(value, 0, 655350, "outside 0..655350 ms", n))) {
				return result;
			}
			put16(out, static_cast<uint32_t>(round_half_up(static_cast<double>(n) / 10)));
			return kOk;
		case Codec::u16_x100:
			if (!(result = integer_of(value, 0, 6553500, "outside 0..6553500 ms", n))) {
				return result;
			}
			put16(out, static_cast<uint32_t>(round_half_up(static_cast<double>(n) / 100)));
			return kOk;
		case Codec::s16:
			if (!(result = integer_of(value, -32768, 32767, "outside -32768..32767", n))) {
				return result;
			}
			put16(out, static_cast<uint32_t>(n) & 0xFFFF);
			return kOk;
		case Codec::s16_x10:
			if (!(result = integer_of(value, -327680, 327670, "outside -327680..327670 ms", n))) {
				return result;
			}
			put16(out, static_cast<uint32_t>(static_cast<int32_t>(round_half_up(static_cast<double>(n) / 10))) & 0xFFFF);
			return kOk;
		case Codec::s16_x100:
			if (!(result = integer_of(value, -3276800, 3276700, "outside -3276800..3276700 ms", n))) {
				return result;
			}
			put16(out, static_cast<uint32_t>(static_cast<int32_t>(round_half_up(static_cast<double>(n) / 100))) & 0xFFFF);
			return kOk;
		case Codec::s16_percent:
			if (!(result = real_of(value, -327.68, 327.67, "outside -327.68..327.67 %", r))) {
				return result;
			}
			put16(out, static_cast<uint32_t>(static_cast<int32_t>(round_half_up(r * 100))) & 0xFFFF);
			return kOk;
		case Codec::float16: {
			const char* range = "";
			double min = float16_min(sub, range);
			if (!(result = real_of(value, min, kFloat16Max, range, r))) {
				return result;
			}
			encode_float16(r, out);
			return kOk;
		}

		case Codec::u32:
			if (!(result = integer_of(value, 0, 4294967295.0, "outside 0..4294967295", n))) {
				return result;
			}
			put32(out, static_cast<uint32_t>(n));
			return kOk;
		case Codec::s32:
			if (!(result = integer_of(value, -2147483648.0, 2147483647.0, "outside -2147483648..2147483647", n))) {
				return result;
			}
			put32(out, static_cast<uint32_t>(static_cast<int32_t>(n)));
			return kOk;
		case Codec::float32: {
			if (!(result = real_of(value, -FLT_MAX, FLT_MAX, "outside the range of a 4-octet float", r))) {
				return result;
			}
			float f = static_cast<float>(r);
			uint32_t bits = 0;
			std::memcpy(&bits, &f, sizeof(bits));
			put32(out, bits);
			return kOk;
		}

		case Codec::time_of_day: {
			if (!(result = require(value, ValueType::time_of_day))) {
				return result;
			}
			TimeOfDay t = value.as_time();
			if (t.day_of_week > 7) {
				return refuse(DptStatus::out_of_range, "day of week outside 0..7");
			}
			if (t.hour > 23) {
				return refuse(DptStatus::out_of_range, "hour outside 0..23");
			}
			if (t.minute > 59) {
				return refuse(DptStatus::out_of_range, "minute outside 0..59");
			}
			if (t.second > 59) {
				return refuse(DptStatus::out_of_range, "second outside 0..59");
			}
			out[0] = static_cast<uint8_t>(t.day_of_week << 5 | t.hour);
			out[1] = t.minute;
			out[2] = t.second;
			return kOk;
		}

		case Codec::date: {
			if (!(result = require(value, ValueType::date))) {
				return result;
			}
			Date d = value.as_date();
			if (d.year < 1990 || d.year > 2089) {
				return refuse(DptStatus::out_of_range, "year outside 1990..2089");
			}
			if (d.month < 1 || d.month > 12) {
				return refuse(DptStatus::out_of_range, "month outside 1..12");
			}
			if (d.day < 1 || d.day > 31) {
				return refuse(DptStatus::out_of_range, "day outside 1..31");
			}
			out[0] = d.day;
			out[1] = d.month;
			out[2] = static_cast<uint8_t>(d.year % 100);
			return kOk;
		}

		case Codec::date_time: {
			if (!(result = require(value, ValueType::date_time))) {
				return result;
			}
			DateTime d = value.as_date_time();
			if ((d.flags & ~DateTime::kAllFlags) != 0) {
				return refuse(DptStatus::out_of_range, "undefined flag bits");
			}
			if (d.year < 1900 || d.year > 2155) {
				return refuse(DptStatus::out_of_range, "year outside 1900..2155");
			}
			if (d.day_of_week > 7) {
				return refuse(DptStatus::out_of_range, "day of week outside 0..7");
			}
			if ((d.flags & DateTime::kNoDate) == 0) {
				if (d.month < 1 || d.month > 12) {
					return refuse(DptStatus::out_of_range, "month outside 1..12");
				}
				if (d.day < 1 || d.day > 31) {
					return refuse(DptStatus::out_of_range, "day outside 1..31");
				}
			} else if (d.month > 15 || d.day > 31) {
				return refuse(DptStatus::out_of_range, "month or day does not fit its field");
			}
			if ((d.flags & DateTime::kNoTime) == 0) {
				if (d.hour > 24) {
					return refuse(DptStatus::out_of_range, "hour outside 0..24");
				}
				if (d.minute > 59) {
					return refuse(DptStatus::out_of_range, "minute outside 0..59");
				}
				if (d.second > 59) {
					return refuse(DptStatus::out_of_range, "second outside 0..59");
				}
				if (d.hour == 24 && (d.minute != 0 || d.second != 0)) {
					return refuse(DptStatus::out_of_range, "hour 24 only as 24:00:00");
				}
			} else if (d.hour > 31 || d.minute > 63 || d.second > 63) {
				return refuse(DptStatus::out_of_range, "hour, minute or second does not fit its field");
			}
			out[0] = static_cast<uint8_t>(d.year - 1900);
			out[1] = d.month;
			out[2] = d.day;
			out[3] = static_cast<uint8_t>(d.day_of_week << 5 | d.hour);
			out[4] = d.minute;
			out[5] = d.second;
			out[6] = static_cast<uint8_t>(d.flags);
			out[7] = static_cast<uint8_t>(d.flags >> 8);
			return kOk;
		}

		case Codec::rgb: {
			if (!(result = require(value, ValueType::color))) {
				return result;
			}
			Color c = value.as_color();
			if (c.valid != Color::kRgb) {
				return refuse(DptStatus::out_of_range, "RGB transmits exactly red, green and blue");
			}
			if (c.white != 0) {
				return refuse(DptStatus::out_of_range, "RGB has no white channel");
			}
			out[0] = c.red;
			out[1] = c.green;
			out[2] = c.blue;
			return kOk;
		}

		case Codec::rgbw: {
			if (!(result = require(value, ValueType::color))) {
				return result;
			}
			Color c = value.as_color();
			if ((c.valid & ~Color::kAll) != 0) {
				return refuse(DptStatus::out_of_range, "undefined validity bits");
			}
			out[0] = c.red;
			out[1] = c.green;
			out[2] = c.blue;
			out[3] = c.white;
			out[4] = 0;
			out[5] = c.valid;
			return kOk;
		}
	}
	return refuse(DptStatus::unsupported, "unsupported");
}

// --- Decoding -------------------------------------------------------------------------------

DptResult decode_value(Codec codec, uint16_t sub, const uint8_t* in, Value& value) {
	switch (codec) {
		case Codec::unsupported: return refuse(DptStatus::unsupported, "unsupported");
		case Codec::boolean: value = Value::of_bool((in[0] & 0x01) != 0); return kOk;
		case Codec::control: value = Value::of_controlled((in[0] & 0x02) != 0, in[0] & 0x01); return kOk;
		case Codec::step: value = Value::of_controlled((in[0] & 0x08) != 0, in[0] & 0x07); return kOk;
		case Codec::scene_control: value = Value::of_controlled((in[0] & 0x80) != 0, in[0] & 0x3F); return kOk;
		case Codec::scene: value = Value::of_unsigned(in[0] & 0x3Fu); return kOk;
		case Codec::char_ascii: return decode_chars(in, 1, false, value);
		case Codec::char_latin1: return decode_chars(in, 1, true, value);
		case Codec::string_ascii: return decode_chars(in, 14, false, value);
		case Codec::string_latin1: return decode_chars(in, 14, true, value);
		case Codec::u8: value = Value::of_unsigned(in[0]); return kOk;
		case Codec::u8_tariff:
			if (in[0] == 255) {
				return refuse(DptStatus::invalid, "tariff 255 is outside 0..254");
			}
			value = Value::of_unsigned(in[0]);
			return kOk;
		case Codec::u8_percent: value = Value::of_real(in[0] * 100.0 / 255); return kOk;
		case Codec::u8_angle: value = Value::of_real(in[0] * 360.0 / 255); return kOk;
		case Codec::s8: value = Value::of_signed(static_cast<int8_t>(in[0])); return kOk;
		case Codec::enum8:
			if (!enum_element_valid(sub, in[0])) {
				return refuse(DptStatus::invalid, "no element of the enumeration");
			}
			value = Value::of_enum(in[0]);
			return kOk;
		case Codec::u16: value = Value::of_unsigned(get16(in)); return kOk;
		case Codec::u16_x10: value = Value::of_unsigned(get16(in) * 10); return kOk;
		case Codec::u16_x100: value = Value::of_unsigned(get16(in) * 100); return kOk;
		case Codec::s16: value = Value::of_signed(static_cast<int16_t>(get16(in))); return kOk;
		case Codec::s16_x10: value = Value::of_signed(static_cast<int16_t>(get16(in)) * 10); return kOk;
		case Codec::s16_x100: value = Value::of_signed(static_cast<int16_t>(get16(in)) * 100); return kOk;
		case Codec::s16_percent: value = Value::of_real(static_cast<int16_t>(get16(in)) / 100.0); return kOk;
		case Codec::float16: {
			uint32_t bits = get16(in);
			if (bits == 0x7FFF) {
				return refuse(DptStatus::invalid, "0x7FFF is the invalid value");
			}
			value = Value::of_real(decode_float16(bits));
			return kOk;
		}
		case Codec::u32: value = Value::of_unsigned(get32(in)); return kOk;
		case Codec::s32: value = Value::of_signed(static_cast<int32_t>(get32(in))); return kOk;
		case Codec::float32: {
			uint32_t bits = get32(in);
			float f = 0;
			std::memcpy(&f, &bits, sizeof(f));
			if (!std::isfinite(f)) {
				return refuse(DptStatus::invalid, "not a number or infinite");
			}
			value = Value::of_real(f);
			return kOk;
		}
		case Codec::time_of_day: {
			TimeOfDay t;
			t.day_of_week = static_cast<uint8_t>(in[0] >> 5);
			t.hour = in[0] & 0x1F;
			t.minute = in[1] & 0x3F;
			t.second = in[2] & 0x3F;
			if (t.hour > 23) {
				return refuse(DptStatus::invalid, "hour outside 0..23");
			}
			if (t.minute > 59) {
				return refuse(DptStatus::invalid, "minute outside 0..59");
			}
			if (t.second > 59) {
				return refuse(DptStatus::invalid, "second outside 0..59");
			}
			value = Value::of_time(t);
			return kOk;
		}
		case Codec::date: {
			Date d;
			d.day = in[0] & 0x1F;
			d.month = in[1] & 0x0F;
			unsigned year = in[2] & 0x7F;
			if (year > 99) {
				return refuse(DptStatus::invalid, "year outside 0..99");
			}
			if (d.month < 1 || d.month > 12) {
				return refuse(DptStatus::invalid, "month outside 1..12");
			}
			if (d.day < 1) {
				return refuse(DptStatus::invalid, "day outside 1..31");
			}
			d.year = static_cast<uint16_t>(year >= 90 ? 1900 + year : 2000 + year);
			value = Value::of_date(d);
			return kOk;
		}
		case Codec::date_time: {
			DateTime d;
			d.year = static_cast<uint16_t>(1900 + in[0]);
			d.month = in[1] & 0x0F;
			d.day = in[2] & 0x1F;
			d.day_of_week = static_cast<uint8_t>(in[3] >> 5);
			d.hour = in[3] & 0x1F;
			d.minute = in[4] & 0x3F;
			d.second = in[5] & 0x3F;
			d.flags = static_cast<uint16_t>(in[6] | (in[7] & 0xC0) << 8);
			if ((d.flags & DateTime::kNoDate) == 0) {
				if (d.month < 1 || d.month > 12) {
					return refuse(DptStatus::invalid, "month outside 1..12");
				}
				if (d.day < 1) {
					return refuse(DptStatus::invalid, "day outside 1..31");
				}
			}
			if ((d.flags & DateTime::kNoTime) == 0) {
				if (d.hour > 24) {
					return refuse(DptStatus::invalid, "hour outside 0..24");
				}
				if (d.minute > 59) {
					return refuse(DptStatus::invalid, "minute outside 0..59");
				}
				if (d.second > 59) {
					return refuse(DptStatus::invalid, "second outside 0..59");
				}
				if (d.hour == 24 && (d.minute != 0 || d.second != 0)) {
					return refuse(DptStatus::invalid, "hour 24 only as 24:00:00");
				}
			}
			value = Value::of_date_time(d);
			return kOk;
		}
		case Codec::rgb: {
			Color c;
			c.red = in[0];
			c.green = in[1];
			c.blue = in[2];
			c.valid = Color::kRgb;
			value = Value::of_color(c);
			return kOk;
		}
		case Codec::rgbw: {
			Color c;
			c.red = in[0];
			c.green = in[1];
			c.blue = in[2];
			c.white = in[3];
			c.valid = in[5] & Color::kAll;
			value = Value::of_color(c);
			return kOk;
		}
	}
	return refuse(DptStatus::unsupported, "unsupported");
}

/// Parses a decimal number of 1..5 digits up to 65535; advances `pos`.
bool parse_number(const char* text, size_t& pos, uint16_t& out) {
	uint32_t v = 0;
	size_t start = pos;
	while (text[pos] >= '0' && text[pos] <= '9') {
		v = v * 10 + static_cast<uint32_t>(text[pos] - '0');
		if (v > 65535 || pos - start >= 5) {
			return false;
		}
		++pos;
	}
	if (pos == start) {
		return false;
	}
	out = static_cast<uint16_t>(v);
	return true;
}

bool has_prefix(const char* text, const char* prefix) {
	for (; *prefix != '\0'; ++text, ++prefix) {
		char c = *text;
		if (c >= 'a' && c <= 'z') {
			c = static_cast<char>(c - 'a' + 'A');
		}
		if (c != *prefix) {
			return false;
		}
	}
	return true;
}

} // namespace

const char* dpt_status_name(DptStatus status) {
	switch (status) {
		case DptStatus::ok: return "ok";
		case DptStatus::unsupported: return "unsupported";
		case DptStatus::wrong_length: return "wrong_length";
		case DptStatus::out_of_range: return "out_of_range";
		case DptStatus::invalid: return "invalid";
		case DptStatus::wrong_type: return "wrong_type";
	}
	return "?";
}

std::optional<Dpt> Dpt::parse(const char* text) {
	if (text == nullptr) {
		return std::nullopt;
	}
	Dpt dpt;
	size_t pos = 0;
	if (has_prefix(text, "DPST-") || has_prefix(text, "DPT-")) {
		pos = has_prefix(text, "DPST-") ? 5 : 4;
		if (!parse_number(text, pos, dpt.main)) {
			return std::nullopt;
		}
		if (text[pos] == '-') {
			++pos;
			if (!parse_number(text, pos, dpt.sub)) {
				return std::nullopt;
			}
		}
	} else {
		if (!parse_number(text, pos, dpt.main)) {
			return std::nullopt;
		}
		if (text[pos] == '.') {
			++pos;
			if (text[pos] == '*') {
				++pos;
			} else if (!parse_number(text, pos, dpt.sub)) {
				return std::nullopt;
			}
		}
	}
	if (text[pos] != '\0' || dpt.main == 0) {
		return std::nullopt;
	}
	return dpt;
}

const char* Dpt::format(char* buffer, size_t size) const {
	if (size > 0) {
		std::snprintf(buffer, size, "%u.%03u", static_cast<unsigned>(main), static_cast<unsigned>(sub));
	}
	return buffer;
}

bool Dpt::supported() const { return rule_of(*this).codec != Codec::unsupported; }

const char* Dpt::unsupported_reason() const { return rule_of(*this).unsupported; }

bool Dpt::embedded() const {
	Codec codec = rule_of(*this).codec;
	return codec == Codec::boolean || codec == Codec::control || codec == Codec::step;
}

size_t Dpt::size() const { return size_of(rule_of(*this).codec); }

ValueType Dpt::value_type() const { return value_type_of(rule_of(*this).codec); }

DptResult encode(Dpt dpt, const Value& value, DptBytes& bytes) {
	bytes = DptBytes();
	Rule rule = rule_of(dpt);
	if (rule.codec == Codec::unsupported) {
		return refuse(DptStatus::unsupported, rule.unsupported);
	}
	DptResult result = encode_value(rule.codec, dpt.sub, value, bytes.data);
	if (!result) {
		bytes = DptBytes();
		return result;
	}
	bytes.length = static_cast<uint8_t>(size_of(rule.codec));
	return result;
}

DptResult decode(Dpt dpt, const uint8_t* data, size_t length, Value& value) {
	value = Value();
	Rule rule = rule_of(dpt);
	if (rule.codec == Codec::unsupported) {
		return refuse(DptStatus::unsupported, rule.unsupported);
	}
	if (length != size_of(rule.codec) || data == nullptr) {
		static const char* const kExpected[] = {"", "1 octet expected", "2 octets expected",
			"3 octets expected", "4 octets expected", "", "6 octets expected", "", "8 octets expected"};
		size_t expected = size_of(rule.codec);
		return refuse(DptStatus::wrong_length, expected == 14 ? "14 octets expected" : kExpected[expected]);
	}
	DptResult result = decode_value(rule.codec, dpt.sub, data, value);
	if (!result) {
		value = Value();
	}
	return result;
}

} // namespace hc
