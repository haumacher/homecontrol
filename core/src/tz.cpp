/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/tz.h"

#include <cstdio>

namespace hc {

namespace {

bool is_alpha(char c) {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool is_digit(char c) {
	return c >= '0' && c <= '9';
}

/// Parses an unsigned decimal of at most `max_digits` digits; fails on none.
bool parse_number(const char*& p, unsigned max_digits, unsigned& value) {
	unsigned digits = 0;
	value = 0;
	while (is_digit(*p) && digits < max_digits) {
		value = value * 10 + static_cast<unsigned>(*p - '0');
		++p;
		++digits;
	}
	return digits > 0 && !is_digit(*p);
}

/// A zone name: three or more letters, or <...> with three or more of [A-Za-z0-9+-].
bool parse_name(const char*& p) {
	unsigned length = 0;
	if (*p == '<') {
		++p;
		while (is_alpha(*p) || is_digit(*p) || *p == '+' || *p == '-') {
			++p;
			++length;
		}
		if (*p != '>') {
			return false;
		}
		++p;
		return length >= 3;
	}
	while (is_alpha(*p)) {
		++p;
		++length;
	}
	return length >= 3;
}

/// [+-]hh[:mm[:ss]] with hh <= max_hours.
bool parse_time(const char*& p, unsigned max_hours) {
	if (*p == '+' || *p == '-') {
		++p;
	}
	unsigned value;
	if (!parse_number(p, 3, value) || value > max_hours) {
		return false;
	}
	for (int part = 0; part < 2 && *p == ':'; ++part) {
		++p;
		if (!parse_number(p, 2, value) || value > 59) {
			return false;
		}
	}
	return true;
}

/// Jn, n or Mm.w.d, then an optional /time.
bool parse_rule(const char*& p) {
	unsigned value;
	if (*p == 'J') {
		++p;
		if (!parse_number(p, 3, value) || value < 1 || value > 365) {
			return false;
		}
	} else if (*p == 'M') {
		++p;
		if (!parse_number(p, 2, value) || value < 1 || value > 12 || *p != '.') {
			return false;
		}
		++p;
		if (!parse_number(p, 1, value) || value < 1 || value > 5 || *p != '.') {
			return false;
		}
		++p;
		if (!parse_number(p, 1, value) || value > 6) {
			return false;
		}
	} else if (!parse_number(p, 3, value) || value > 365) {
		return false;
	}
	if (*p == '/') {
		++p;
		return parse_time(p, 167);
	}
	return true;
}

bool refuse(const char** error, const char* text) {
	if (error != nullptr) {
		*error = text;
	}
	return false;
}

} // namespace

bool posix_tz_valid(const char* tz, const char** error) {
	if (tz == nullptr || *tz == '\0') {
		return refuse(error, "empty time zone");
	}
	const char* p = tz;
	if (*p == ':') {
		return refuse(error, "zone files (\":name\") are not supported, give a POSIX TZ rule");
	}
	if (!parse_name(p)) {
		return refuse(error, "standard zone name must be 3+ letters or <...>");
	}
	if (!parse_time(p, 24)) {
		return refuse(error, "standard offset missing or invalid ([+-]hh[:mm[:ss]], hh <= 24)");
	}
	if (*p == '\0') {
		return true;
	}
	if (!parse_name(p)) {
		return refuse(error, "daylight zone name must be 3+ letters or <...>");
	}
	if (*p != ',' && *p != '\0' && !parse_time(p, 24)) {
		return refuse(error, "daylight offset invalid ([+-]hh[:mm[:ss]], hh <= 24)");
	}
	if (*p != ',') {
		return refuse(error, "a zone with daylight saving time needs both transition rules (,start,end)");
	}
	++p;
	if (!parse_rule(p)) {
		return refuse(error, "start rule invalid (Jn, n or Mm.w.d, optional /time)");
	}
	if (*p != ',') {
		return refuse(error, "end rule missing");
	}
	++p;
	if (!parse_rule(p)) {
		return refuse(error, "end rule invalid (Jn, n or Mm.w.d, optional /time)");
	}
	if (*p != '\0') {
		return refuse(error, "unexpected characters after the end rule");
	}
	return true;
}

int64_t days_from_civil(int32_t year, unsigned month, unsigned day) {
	// Howard Hinnant's algorithm.
	int64_t y = static_cast<int64_t>(year) - (month <= 2 ? 1 : 0);
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	int64_t yoe = y - era * 400;
	int64_t mp = (static_cast<int64_t>(month) + 9) % 12;
	int64_t doy = (153 * mp + 2) / 5 + static_cast<int64_t>(day) - 1;
	int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

int32_t utc_offset_seconds(const LocalTime& local, int64_t utc_seconds) {
	int64_t local_seconds = days_from_civil(local.year, local.month, local.day) * 86400 +
		local.hour * 3600 + local.minute * 60 + local.second;
	return static_cast<int32_t>(local_seconds - utc_seconds);
}

void format_local_time(const LocalTime& local, int64_t utc_seconds, char* buffer, size_t size) {
	int32_t offset = utc_offset_seconds(local, utc_seconds);
	char sign = offset < 0 ? '-' : '+';
	int32_t magnitude = offset < 0 ? -offset : offset;
	std::snprintf(buffer, size, "%04d-%02u-%02u %02u:%02u:%02u%s%s (UTC%c%02d:%02d)",
		static_cast<int>(local.year), static_cast<unsigned>(local.month), static_cast<unsigned>(local.day),
		static_cast<unsigned>(local.hour), static_cast<unsigned>(local.minute),
		static_cast<unsigned>(local.second), local.zone[0] != '\0' ? " " : "", local.zone, sign,
		static_cast<int>(magnitude / 3600), static_cast<int>(magnitude % 3600 / 60));
}

} // namespace hc
