/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/ipv4.h"

#include <cstdio>

namespace hc {

const char* Ipv4Endpoint::format(char* buffer, size_t size) const {
	char ip[kMaxIpv4Text];
	std::snprintf(buffer, size, "%s:%u", format_ipv4(address, ip, sizeof(ip)), static_cast<unsigned>(port));
	return buffer;
}

bool parse_ipv4(const char* text, uint32_t& address) {
	if (text == nullptr) {
		return false;
	}
	uint32_t result = 0;
	const char* p = text;
	for (int part = 0; part < 4; ++part) {
		if (part > 0) {
			if (*p != '.') {
				return false;
			}
			++p;
		}
		uint32_t value = 0;
		int digits = 0;
		while (*p >= '0' && *p <= '9') {
			if (++digits > 3) {
				return false;
			}
			value = value * 10 + static_cast<uint32_t>(*p - '0');
			++p;
		}
		if (digits == 0 || value > 255) {
			return false;
		}
		result = (result << 8) | value;
	}
	if (*p != '\0') {
		return false;
	}
	address = result;
	return true;
}

const char* format_ipv4(uint32_t address, char* buffer, size_t size) {
	std::snprintf(buffer, size, "%u.%u.%u.%u", static_cast<unsigned>(address >> 24),
		static_cast<unsigned>((address >> 16) & 0xFF), static_cast<unsigned>((address >> 8) & 0xFF),
		static_cast<unsigned>(address & 0xFF));
	return buffer;
}

} // namespace hc
