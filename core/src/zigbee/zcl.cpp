/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/zigbee/zcl.h"

#include <cstring>

namespace hc {
namespace zigbee {

namespace {

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

void fail(const char** error, const char* reason) {
	if (error != nullptr) {
		*error = reason;
	}
}

} // namespace

void format_ieee(Ieee ieee, char out[kIeeeText]) {
	static const char kDigits[] = "0123456789abcdef";
	for (int i = 0; i < 16; ++i) {
		out[i] = kDigits[(ieee >> (60 - 4 * i)) & 0xF];
	}
	out[16] = '\0';
}

bool parse_ieee(const char* text, Ieee& out) {
	if (text == nullptr) {
		return false;
	}
	size_t length = std::strlen(text);
	bool colons = length == 23;
	if (length != 16 && !colons) {
		return false;
	}
	Ieee value = 0;
	int digits = 0;
	for (size_t i = 0; i < length; ++i) {
		if (colons && i % 3 == 2) {
			if (text[i] != ':') {
				return false;
			}
			continue;
		}
		int d = hex_digit(text[i]);
		if (d < 0) {
			return false;
		}
		value = (value << 4) | static_cast<Ieee>(d);
		++digits;
	}
	if (digits != 16 || value == 0 || value == ~static_cast<Ieee>(0)) {
		return false;
	}
	out = value;
	return true;
}

size_t zcl_type_size(uint8_t type) {
	switch (type) {
		case zcl_type::kBool:
		case zcl_type::kBitmap8:
		case zcl_type::kUint8:
		case zcl_type::kInt8:
		case zcl_type::kEnum8:
			return 1;
		case zcl_type::kBitmap16:
		case zcl_type::kUint16:
		case zcl_type::kInt16:
		case zcl_type::kEnum16:
			return 2;
		case zcl_type::kUint24:
		case zcl_type::kInt24:
			return 3;
		case zcl_type::kUint32:
		case zcl_type::kInt32:
			return 4;
		default:
			return 0;
	}
}

bool decode_zcl(uint8_t type, const uint8_t* data, size_t length, ZclValue& out, const char** error) {
	ZclValue value;
	value.type = type;
	if (type == zcl_type::kCharString) {
		if (length < 1 || data == nullptr) {
			fail(error, "character string without its length octet");
			return false;
		}
		size_t chars = data[0] == 0xFF ? 0 : data[0];
		if (chars + 1 > length) {
			fail(error, "character string shorter than its length octet");
			return false;
		}
		if (chars > ZclValue::kMaxString) {
			fail(error, "character string longer than 32 characters");
			return false;
		}
		std::memcpy(value.text, data + 1, chars);
		value.text[chars] = '\0';
		if (std::strlen(value.text) != chars) {
			fail(error, "character string contains a NUL");
			return false;
		}
		out = value;
		return true;
	}
	size_t size = zcl_type_size(type);
	if (size == 0) {
		fail(error, "unsupported ZCL data type");
		return false;
	}
	if (length != size || data == nullptr) {
		fail(error, "length does not match the ZCL data type");
		return false;
	}
	uint64_t raw = 0;
	for (size_t i = 0; i < size; ++i) {
		raw |= static_cast<uint64_t>(data[i]) << (8 * i);
	}
	bool is_signed = type == zcl_type::kInt8 || type == zcl_type::kInt16 || type == zcl_type::kInt24 ||
		type == zcl_type::kInt32;
	if (is_signed && (raw & (1ull << (8 * size - 1))) != 0) {
		raw |= ~0ull << (8 * size); // sign extension
	}
	value.number = static_cast<int64_t>(raw);
	if (type == zcl_type::kBool) {
		value.number = raw != 0 ? 1 : 0;
	}
	out = value;
	return true;
}

} // namespace zigbee
} // namespace hc
