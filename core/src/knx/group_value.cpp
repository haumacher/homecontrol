/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/knx/group_value.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hc {
namespace knx {

namespace {

bool refuse(const char** error, const char* reason) {
	if (error != nullptr) {
		*error = reason;
	}
	return false;
}

/// Reads `count` unsigned decimal numbers separated by `separator` (exactly `count`).
bool read_numbers(const char* text, char separator, long* out, int count) {
	const char* p = text;
	for (int i = 0; i < count; ++i) {
		if (i > 0) {
			if (*p != separator) {
				return false;
			}
			++p;
		}
		if (*p < '0' || *p > '9') {
			return false;
		}
		char* end = nullptr;
		out[i] = std::strtol(p, &end, 10);
		if (end - p > 5) {
			return false;
		}
		p = end;
	}
	return *p == '\0';
}

bool in(long value, long low, long high) {
	return value >= low && value <= high;
}

} // namespace

bool make_group_value(LData& frame, GroupAddress destination, GroupService service, Dpt dpt, const Value& value,
	const char** error) {
	if (service != GroupService::write && service != GroupService::response) {
		return refuse(error, "a value travels in a group write or response");
	}
	DptBytes bytes;
	DptResult encoded = encode(dpt, value, bytes);
	if (!encoded) {
		return refuse(error, encoded.reason);
	}
	CemiResult result = make_group_request(frame, destination, service, bytes.data, bytes.length, dpt.embedded());
	if (!result) {
		return refuse(error, result.reason);
	}
	return true;
}

void make_group_read(LData& frame, GroupAddress destination) {
	make_group_request(frame, destination, GroupService::read, nullptr, 0, false);
}

DptResult decode_group_value(Dpt dpt, const GroupData& data, Value& value) {
	value = Value();
	if (!dpt.supported()) {
		return DptResult{DptStatus::unsupported, dpt.unsupported_reason()};
	}
	if (dpt.embedded() && !data.short_form) {
		return DptResult{DptStatus::wrong_length, "a value of up to 6 bits belongs in the APCI octet"};
	}
	if (!dpt.embedded() && data.short_form) {
		return DptResult{DptStatus::wrong_length, "the telegram carries only 6 bits in the APCI octet"};
	}
	return decode(dpt, data.data, data.length, value);
}

bool parse_value_text(Dpt dpt, const char* text, Value& value, const char** error) {
	if (text == nullptr) {
		return refuse(error, "value is missing");
	}
	if (!dpt.supported()) {
		return refuse(error, dpt.unsupported_reason());
	}
	switch (dpt.value_type()) {
	case ValueType::boolean:
		if (std::strcmp(text, "1") == 0 || std::strcmp(text, "on") == 0 || std::strcmp(text, "true") == 0) {
			value = Value::of_bool(true);
			return true;
		}
		if (std::strcmp(text, "0") == 0 || std::strcmp(text, "off") == 0 || std::strcmp(text, "false") == 0) {
			value = Value::of_bool(false);
			return true;
		}
		return refuse(error, "expected on, off, true, false, 1 or 0");
	case ValueType::unsigned_int:
	case ValueType::signed_int:
	case ValueType::real:
	case ValueType::enumeration: {
		char* end = nullptr;
		double number = std::strtod(text, &end);
		if (text[0] == '\0' || end == nullptr || *end != '\0' || !std::isfinite(number)) {
			return refuse(error, "expected a decimal number");
		}
		// The codec converts where exact and checks the range.
		value = Value::of_real(number);
		return true;
	}
	case ValueType::string: {
		std::optional<Value> string = Value::of_string(text);
		if (!string) {
			return refuse(error, "string longer than 28 bytes");
		}
		value = *string;
		return true;
	}
	case ValueType::controlled: {
		long parts[2];
		if (!read_numbers(text, ',', parts, 2) || !in(parts[0], 0, 1) || !in(parts[1], 0, 255)) {
			return refuse(error, "expected <control 0|1>,<value>");
		}
		value = Value::of_controlled(parts[0] != 0, static_cast<uint8_t>(parts[1]));
		return true;
	}
	case ValueType::time_of_day: {
		long parts[3] = {0, 0, 0};
		if (!read_numbers(text, ':', parts, 3) && !read_numbers(text, ':', parts, 2)) {
			return refuse(error, "expected HH:MM or HH:MM:SS");
		}
		if (!in(parts[0], 0, 23) || !in(parts[1], 0, 59) || !in(parts[2], 0, 59)) {
			return refuse(error, "time of day out of range");
		}
		TimeOfDay time;
		time.hour = static_cast<uint8_t>(parts[0]);
		time.minute = static_cast<uint8_t>(parts[1]);
		time.second = static_cast<uint8_t>(parts[2]);
		value = Value::of_time(time);
		return true;
	}
	case ValueType::date: {
		long parts[3];
		if (!read_numbers(text, '-', parts, 3) || !in(parts[0], 0, 9999) || !in(parts[1], 1, 12) ||
			!in(parts[2], 1, 31)) {
			return refuse(error, "expected YYYY-MM-DD");
		}
		Date date;
		date.year = static_cast<uint16_t>(parts[0]);
		date.month = static_cast<uint8_t>(parts[1]);
		date.day = static_cast<uint8_t>(parts[2]);
		value = Value::of_date(date);
		return true;
	}
	case ValueType::date_time: {
		const char* t = std::strchr(text, 'T');
		char date_text[16];
		long date[3];
		long time[3];
		if (t == nullptr || static_cast<size_t>(t - text) >= sizeof(date_text)) {
			return refuse(error, "expected YYYY-MM-DDTHH:MM:SS");
		}
		std::memcpy(date_text, text, static_cast<size_t>(t - text));
		date_text[t - text] = '\0';
		if (!read_numbers(date_text, '-', date, 3) || !read_numbers(t + 1, ':', time, 3) || !in(date[0], 0, 9999) ||
			!in(date[1], 1, 12) || !in(date[2], 1, 31) || !in(time[0], 0, 24) || !in(time[1], 0, 59) ||
			!in(time[2], 0, 59)) {
			return refuse(error, "expected YYYY-MM-DDTHH:MM:SS");
		}
		DateTime dt;
		dt.year = static_cast<uint16_t>(date[0]);
		dt.month = static_cast<uint8_t>(date[1]);
		dt.day = static_cast<uint8_t>(date[2]);
		dt.hour = static_cast<uint8_t>(time[0]);
		dt.minute = static_cast<uint8_t>(time[1]);
		dt.second = static_cast<uint8_t>(time[2]);
		dt.flags = DateTime::kNoWorkingDay;
		value = Value::of_date_time(dt);
		return true;
	}
	case ValueType::color: {
		long parts[4];
		Color color;
		if (read_numbers(text, ',', parts, 3)) {
			color.valid = Color::kRgb;
			parts[3] = 0;
		} else if (read_numbers(text, ',', parts, 4)) {
			color.valid = Color::kAll;
		} else {
			return refuse(error, "expected R,G,B or R,G,B,W");
		}
		for (long part : parts) {
			if (!in(part, 0, 255)) {
				return refuse(error, "color level out of 0..255");
			}
		}
		color.red = static_cast<uint8_t>(parts[0]);
		color.green = static_cast<uint8_t>(parts[1]);
		color.blue = static_cast<uint8_t>(parts[2]);
		color.white = static_cast<uint8_t>(parts[3]);
		value = Value::of_color(color);
		return true;
	}
	case ValueType::none:
	case ValueType::raw:
		break;
	}
	return refuse(error, "no text form for this type");
}

const char* format_hex(const uint8_t* data, size_t length, char* buffer, size_t size) {
	if (size == 0) {
		return buffer;
	}
	buffer[0] = '\0';
	size_t used = 0;
	for (size_t i = 0; i < length && used + 4 <= size; ++i) {
		used += static_cast<size_t>(std::snprintf(buffer + used, size - used, i == 0 ? "%02X" : " %02X", data[i]));
	}
	return buffer;
}

} // namespace knx
} // namespace hc
