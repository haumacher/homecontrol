/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/knx/address.h"

#include <cstdio>

namespace hc {
namespace knx {

namespace {

/// Reads up to `max_parts` decimal numbers separated by `separator`. Returns the number of parts,
/// 0 on a syntax error (empty part, a non-digit, a leading '+' or '-', more than 5 digits).
size_t split_numbers(const char* text, char separator, uint32_t* parts, size_t max_parts, const char** error) {
	if (text == nullptr || text[0] == '\0') {
		*error = "address is empty";
		return 0;
	}
	size_t count = 0;
	const char* p = text;
	for (;;) {
		if (count == max_parts) {
			*error = "address has too many parts";
			return 0;
		}
		uint32_t value = 0;
		size_t digits = 0;
		while (*p >= '0' && *p <= '9') {
			if (++digits > 5) {
				*error = "address part has too many digits";
				return 0;
			}
			value = value * 10 + static_cast<uint32_t>(*p - '0');
			++p;
		}
		if (digits == 0) {
			*error = "address part is not a number";
			return 0;
		}
		parts[count++] = value;
		if (*p == '\0') {
			return count;
		}
		if (*p != separator) {
			*error = "address contains an unexpected character";
			return 0;
		}
		++p;
	}
}

template <class T>
std::optional<T> refuse(const char** error, const char* reason) {
	if (error != nullptr) {
		*error = reason;
	}
	return std::nullopt;
}

} // namespace

std::optional<GroupAddress> GroupAddress::parse(const char* text, const char** error) {
	uint32_t parts[3];
	const char* reason = nullptr;
	size_t count = split_numbers(text, '/', parts, 3, &reason);
	if (count == 0) {
		return refuse<GroupAddress>(error, reason);
	}
	if (count == 1) {
		return refuse<GroupAddress>(error, "group address needs two or three levels (1/2/3 or 1/515)");
	}
	if (parts[0] > 31) {
		return refuse<GroupAddress>(error, "main group must be 0..31");
	}
	if (count == 2) {
		if (parts[1] > 2047) {
			return refuse<GroupAddress>(error, "sub group of a two-level address must be 0..2047");
		}
		return GroupAddress{static_cast<uint16_t>((parts[0] << 11) | parts[1])};
	}
	if (parts[1] > 7) {
		return refuse<GroupAddress>(error, "middle group must be 0..7");
	}
	if (parts[2] > 255) {
		return refuse<GroupAddress>(error, "sub group must be 0..255");
	}
	return of(static_cast<uint8_t>(parts[0]), static_cast<uint8_t>(parts[1]), static_cast<uint8_t>(parts[2]));
}

const char* GroupAddress::format(char* buffer, size_t size) const {
	std::snprintf(buffer, size, "%u/%u/%u", static_cast<unsigned>(main()), static_cast<unsigned>(middle()),
		static_cast<unsigned>(sub()));
	return buffer;
}

std::optional<IndividualAddress> IndividualAddress::parse(const char* text, const char** error) {
	uint32_t parts[3];
	const char* reason = nullptr;
	size_t count = split_numbers(text, '.', parts, 3, &reason);
	if (count == 0) {
		return refuse<IndividualAddress>(error, reason);
	}
	if (count != 3) {
		return refuse<IndividualAddress>(error, "individual address needs three parts (area.line.device)");
	}
	if (parts[0] > 15) {
		return refuse<IndividualAddress>(error, "area must be 0..15");
	}
	if (parts[1] > 15) {
		return refuse<IndividualAddress>(error, "line must be 0..15");
	}
	if (parts[2] > 255) {
		return refuse<IndividualAddress>(error, "device must be 0..255");
	}
	return of(static_cast<uint8_t>(parts[0]), static_cast<uint8_t>(parts[1]), static_cast<uint8_t>(parts[2]));
}

const char* IndividualAddress::format(char* buffer, size_t size) const {
	std::snprintf(buffer, size, "%u.%u.%u", static_cast<unsigned>(raw >> 12), static_cast<unsigned>((raw >> 8) & 0x0F),
		static_cast<unsigned>(raw & 0xFF));
	return buffer;
}

} // namespace knx
} // namespace hc
