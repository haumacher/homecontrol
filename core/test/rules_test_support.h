/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_TEST_RULES_TEST_SUPPORT_H
#define HC_TEST_RULES_TEST_SUPPORT_H

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "hc/json_value.h"
#include "hc/platform/time_zone.h"
#include "hc/rules/value.h"
#include "hc/technology.h"
#include "hc/tz.h"
#include "json_reader.h"

#ifndef HC_SPEC_DIR
#error "HC_SPEC_DIR must name the spec directory"
#endif

namespace hc_test {

/// hc::JsonDocument over the tests' tree reader: a handle's node is a `const Json*`.
class JsonTree : public hc::JsonDocument {
public:
	hc::JsonValue value(const Json& json) const { return hc::JsonValue(this, &json); }

	hc::JsonKind kind(const hc::JsonValue& v) const override {
		switch (node(v).kind) {
			case Json::Kind::null: return hc::JsonKind::null;
			case Json::Kind::boolean: return hc::JsonKind::boolean;
			case Json::Kind::number: return hc::JsonKind::number;
			case Json::Kind::string: return hc::JsonKind::string;
			case Json::Kind::array: return hc::JsonKind::array;
			case Json::Kind::object: return hc::JsonKind::object;
		}
		return hc::JsonKind::null;
	}

	bool get_bool(const hc::JsonValue& v, bool& out) const override {
		if (node(v).kind != Json::Kind::boolean) {
			return false;
		}
		out = node(v).boolean;
		return true;
	}

	bool get_number(const hc::JsonValue& v, double& out) const override {
		if (node(v).kind != Json::Kind::number || !std::isfinite(node(v).number)) {
			return false;
		}
		out = node(v).number;
		return true;
	}

	bool get_string(const hc::JsonValue& v, char* out, size_t size, size_t* length) const override {
		if (size > 0) {
			out[0] = '\0';
		}
		const Json& j = node(v);
		if (j.kind != Json::Kind::string) {
			return false;
		}
		if (length) {
			*length = j.text.size();
		}
		if (j.text.size() + 1 > size || std::memchr(j.text.data(), '\0', j.text.size()) != nullptr) {
			return false;
		}
		std::memcpy(out, j.text.c_str(), j.text.size() + 1);
		return true;
	}

	bool next_item(const hc::JsonValue& array, size_t& position, hc::JsonValue& item) const override {
		const Json& j = node(array);
		if (j.kind != Json::Kind::array || position >= j.items.size()) {
			return false;
		}
		item = value(j.items[position++]);
		return true;
	}

	bool next_member(const hc::JsonValue& object, size_t& position, char* name, size_t name_size,
		size_t& name_length, hc::JsonValue& out) const override {
		const Json& j = node(object);
		if (j.kind != Json::Kind::object || position >= j.members.size()) {
			return false;
		}
		auto it = j.members.begin();
		std::advance(it, static_cast<long>(position++));
		name_length = it->first.size();
		if (name_size > 0) {
			size_t n = name_length < name_size - 1 ? name_length : name_size - 1;
			std::memcpy(name, it->first.data(), n);
			name[n] = '\0';
		}
		out = value(it->second);
		return true;
	}

	bool member(const hc::JsonValue& object, const char* name, hc::JsonValue& out) const override {
		const Json& j = node(object);
		if (j.kind != Json::Kind::object) {
			return false;
		}
		auto it = j.members.find(name);
		if (it == j.members.end()) {
			return false;
		}
		out = value(it->second);
		return true;
	}

private:
	static const Json& node(const hc::JsonValue& v) { return *static_cast<const Json*>(v.node()); }
};

inline bool read_text(const std::string& path, std::string& out) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return false;
	}
	std::ostringstream buffer;
	buffer << in.rdbuf();
	out = buffer.str();
	return true;
}

/// Parses JSON text; an empty (null) tree and a message on error.
inline bool parse_json(const std::string& text, Json& out, std::string& error) {
	JsonReader reader(text);
	return reader.parse(out, error);
}

/// The rule corpus files (spec/fixtures/rules/NN-*.json), sorted.
std::vector<std::string> rule_corpus_files();
/// The case files of `spec/fixtures/<directory>`, sorted.
std::vector<std::string> corpus_files(const char* directory);

/// A time zone given by a POSIX TZ string ("CET-1CEST,M3.5.0,M10.5.0/3", "UTC0"), evaluated here
/// so that a test pins its zone without depending on the host's TZ or C library. Transition rules
/// of the form Mm.w.d[/time] only (what real zones use); parse() refuses the rest.
class PosixRuleZone : public hc::TimeZone {
public:
	bool parse(const char* tz) {
		if (!hc::posix_tz_valid(tz)) {
			return false;
		}
		const char* p = tz;
		if (!name(p, std_name_) || !offset(p, std_offset_)) {
			return false;
		}
		std_offset_ = -std_offset_; // POSIX: west of Greenwich is positive
		has_dst_ = *p != '\0';
		if (!has_dst_) {
			return true;
		}
		if (!name(p, dst_name_)) {
			return false;
		}
		dst_offset_ = std_offset_ + 3600;
		if (*p != ',' && *p != '\0') {
			int32_t parsed = 0;
			if (!offset(p, parsed)) {
				return false;
			}
			dst_offset_ = -parsed;
		}
		return *p == ',' && rule(++p, start_) && *p == ',' && rule(++p, end_) && *p == '\0';
	}

	void apply(const char*) override {}

	bool to_local(int64_t utc, hc::LocalTime& out) const override {
		bool dst = false;
		if (has_dst_) {
			int64_t days = utc >= 0 ? utc / 86400 : (utc - 86399) / 86400;
			int32_t year = 0;
			unsigned month = 0, day = 0;
			hc::rules::civil_of(days, year, month, day);
			int64_t start = local_instant(year, start_) - std_offset_;
			int64_t end = local_instant(year, end_) - dst_offset_;
			dst = start < end ? (utc >= start && utc < end) : !(utc >= end && utc < start);
		}
		int64_t local = utc + (dst ? dst_offset_ : std_offset_);
		int64_t days = local >= 0 ? local / 86400 : (local - 86399) / 86400;
		int64_t rest = local - days * 86400;
		int32_t year = 0;
		unsigned month = 0, day = 0;
		hc::rules::civil_of(days, year, month, day);
		out.year = year;
		out.month = static_cast<uint8_t>(month);
		out.day = static_cast<uint8_t>(day);
		out.hour = static_cast<uint8_t>(rest / 3600);
		out.minute = static_cast<uint8_t>(rest % 3600 / 60);
		out.second = static_cast<uint8_t>(rest % 60);
		out.dst = dst;
		std::snprintf(out.zone, sizeof(out.zone), "%s", (dst ? dst_name_ : std_name_).c_str());
		return true;
	}

private:
	struct Rule {
		unsigned month = 0, week = 0, weekday = 0; ///< weekday 0 Sunday .. 6
		int32_t time = 7200;
	};

	static bool name(const char*& p, std::string& out) {
		out.clear();
		if (*p == '<') {
			for (++p; *p && *p != '>'; ++p) {
				out += *p;
			}
			if (*p != '>') {
				return false;
			}
			++p;
		} else {
			while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
				out += *p++;
			}
		}
		return out.size() >= 3;
	}

	static bool number(const char*& p, int& out) {
		if (*p < '0' || *p > '9') {
			return false;
		}
		out = 0;
		while (*p >= '0' && *p <= '9') {
			out = out * 10 + (*p++ - '0');
		}
		return true;
	}

	/// [+|-]hh[:mm[:ss]] in seconds.
	static bool offset(const char*& p, int32_t& out) {
		int sign = 1;
		if (*p == '+' || *p == '-') {
			sign = *p++ == '-' ? -1 : 1;
		}
		int h = 0, m = 0, sec = 0;
		if (!number(p, h)) {
			return false;
		}
		if (*p == ':' && (++p, !number(p, m))) {
			return false;
		}
		if (*p == ':' && (++p, !number(p, sec))) {
			return false;
		}
		out = sign * (h * 3600 + m * 60 + sec);
		return true;
	}

	static bool rule(const char*& p, Rule& out) {
		int m = 0, w = 0, d = 0;
		if (*p != 'M' || !number(++p, m) || *p != '.' || !number(++p, w) || *p != '.' || !number(++p, d)) {
			return false;
		}
		out.month = static_cast<unsigned>(m);
		out.week = static_cast<unsigned>(w);
		out.weekday = static_cast<unsigned>(d);
		out.time = 7200;
		if (*p == '/') {
			return offset(++p, out.time);
		}
		return true;
	}

	/// The local seconds (as if UTC) of a rule's moment in `year`.
	static int64_t local_instant(int32_t year, const Rule& r) {
		int64_t first = hc::days_from_civil(year, r.month, 1);
		unsigned first_weekday = static_cast<unsigned>(((first % 7) + 11) % 7); // 1970-01-01 was a Thursday (4)
		unsigned day = 1 + (r.weekday + 7 - first_weekday) % 7 + (r.week - 1) * 7;
		static const unsigned kDays[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
		while (day > kDays[r.month - 1] || !hc::rules::valid_date(year, r.month, day)) {
			day -= 7;
		}
		return hc::days_from_civil(year, r.month, day) * 86400 + r.time;
	}

	std::string std_name_, dst_name_;
	int32_t std_offset_ = 0, dst_offset_ = 0;
	bool has_dst_ = false;
	Rule start_, end_;
};

/// A technology that accepts every command (without reporting it back), or refuses all; reads
/// likewise (a read sends nothing back either).
class TestTechnology : public hc::Technology {
public:
	const char* name() const override { return "test"; }
	bool command(const hc::Datapoint& datapoint, const hc::Value& value) override {
		(void)value;
		if (refuse) {
			return false;
		}
		(void)datapoint;
		return true;
	}

	bool read(const hc::Datapoint& datapoint) override {
		(void)datapoint;
		return !refuse_reads;
	}

	bool refuse = false;
	bool refuse_reads = false;
};

} // namespace hc_test

#endif // HC_TEST_RULES_TEST_SUPPORT_H
