/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Runs the DPT conformance corpus spec/fixtures/dpt/*.json (format: spec/dpt-corpus.md), which
// E-Home's codec passes too.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "hc/dpt.h"
#include "json_reader.h"

#ifndef HC_SPEC_DIR
#error "HC_SPEC_DIR must name the spec directory"
#endif

using hc::Dpt;
using hc::DptBytes;
using hc::DptResult;
using hc::DptStatus;
using hc::Value;
using hc::ValueType;
using hc_test::Json;
using hc_test::JsonReader;

namespace {

bool parse_hex(const std::string& text, std::vector<uint8_t>& out) {
	out.clear();
	std::istringstream in(text);
	std::string octet;
	while (in >> octet) {
		if (octet.size() != 2) {
			return false;
		}
		char* end = nullptr;
		unsigned long v = std::strtoul(octet.c_str(), &end, 16);
		if (*end != '\0') {
			return false;
		}
		out.push_back(static_cast<uint8_t>(v));
	}
	return true;
}

std::string hex(const uint8_t* data, size_t length) {
	std::string result;
	char octet[4];
	for (size_t i = 0; i < length; ++i) {
		std::snprintf(octet, sizeof(octet), i == 0 ? "%02X" : " %02X", data[i]);
		result += octet;
	}
	return result;
}

/// An integer member within [0, max]; false if absent (with `required`) or outside.
bool field(const Json& object, const char* name, uint32_t max, uint32_t& out, bool required = true) {
	const Json& member = object[name];
	if (member.kind == Json::Kind::null) {
		return !required;
	}
	if (member.kind != Json::Kind::number || std::floor(member.number) != member.number || member.number < 0 ||
		member.number > max) {
		return false;
	}
	out = static_cast<uint32_t>(member.number);
	return true;
}

bool flag(const Json& object, const char* name) {
	const Json& member = object[name];
	return member.kind == Json::Kind::boolean && member.boolean;
}

/// The corpus value as a Value: by the JSON kind, numbers and objects refined by the type the DPT
/// decodes to. False if the JSON does not fit a Value at all (a field wider than its type): the
/// API cannot even express it, the corpus counts that as out_of_range.
bool to_value(const Json& json, Dpt dpt, Value& out) {
	ValueType type = dpt.value_type();
	switch (json.kind) {
		case Json::Kind::boolean: out = Value::of_bool(json.boolean); return true;
		case Json::Kind::string: {
			std::optional<Value> v = Value::of_string(json.text.c_str(), json.text.size());
			if (!v) {
				return false;
			}
			out = *v;
			return true;
		}
		case Json::Kind::number: {
			bool integral = json.text.find_first_of(".eE") == std::string::npos;
			if (type == ValueType::real || !integral) {
				out = Value::of_real(json.number);
				return true;
			}
			if (json.number < -2147483648.0 || json.number > 4294967295.0) {
				out = Value::of_real(json.number); // Beyond 32 bits: only a real can carry it.
				return true;
			}
			if (type == ValueType::signed_int && json.number <= 2147483647.0) {
				out = Value::of_signed(static_cast<int32_t>(json.number));
			} else if (type == ValueType::enumeration && json.number >= 0) {
				out = Value::of_enum(static_cast<uint32_t>(json.number));
			} else if (json.number < 0) {
				out = Value::of_signed(static_cast<int32_t>(json.number));
			} else {
				out = Value::of_unsigned(static_cast<uint32_t>(json.number));
			}
			return true;
		}
		case Json::Kind::object: break;
		default: return false;
	}
	uint32_t a = 0, b = 0, c = 0, d = 0;
	switch (type) {
		case ValueType::controlled: {
			const Json& control = json["control"];
			if (control.kind != Json::Kind::boolean || !field(json, "value", 255, a)) {
				return false;
			}
			out = Value::of_controlled(control.boolean, static_cast<uint8_t>(a));
			return true;
		}
		case ValueType::time_of_day: {
			if (!field(json, "dayOfWeek", 255, a) || !field(json, "hour", 255, b) || !field(json, "minute", 255, c) ||
				!field(json, "second", 255, d)) {
				return false;
			}
			out = Value::of_time(hc::TimeOfDay{static_cast<uint8_t>(a), static_cast<uint8_t>(b), static_cast<uint8_t>(c),
				static_cast<uint8_t>(d)});
			return true;
		}
		case ValueType::date: {
			if (!field(json, "year", 65535, a) || !field(json, "month", 255, b) || !field(json, "day", 255, c)) {
				return false;
			}
			out = Value::of_date(hc::Date{static_cast<uint16_t>(a), static_cast<uint8_t>(b), static_cast<uint8_t>(c)});
			return true;
		}
		case ValueType::date_time: {
			hc::DateTime v;
			uint32_t mo = 0, dow = 0, h = 0, mi = 0, s = 0;
			if (!field(json, "year", 65535, a) || !field(json, "month", 255, mo) || !field(json, "day", 255, b) ||
				!field(json, "dayOfWeek", 255, dow) || !field(json, "hour", 255, h) || !field(json, "minute", 255, mi) ||
				!field(json, "second", 255, s)) {
				return false;
			}
			v.year = static_cast<uint16_t>(a);
			v.month = static_cast<uint8_t>(mo);
			v.day = static_cast<uint8_t>(b);
			v.day_of_week = static_cast<uint8_t>(dow);
			v.hour = static_cast<uint8_t>(h);
			v.minute = static_cast<uint8_t>(mi);
			v.second = static_cast<uint8_t>(s);
			v.flags = static_cast<uint16_t>((flag(json, "summerTime") ? hc::DateTime::kSummerTime : 0) |
				(flag(json, "noTime") ? hc::DateTime::kNoTime : 0) |
				(flag(json, "noDayOfWeek") ? hc::DateTime::kNoDayOfWeek : 0) |
				(flag(json, "noDate") ? hc::DateTime::kNoDate : 0) | (flag(json, "noYear") ? hc::DateTime::kNoYear : 0) |
				(flag(json, "noWorkingDay") ? hc::DateTime::kNoWorkingDay : 0) |
				(flag(json, "workingDay") ? hc::DateTime::kWorkingDay : 0) |
				(flag(json, "fault") ? hc::DateTime::kFault : 0) |
				(flag(json, "syncReliable") ? hc::DateTime::kSyncReliable : 0) |
				(flag(json, "clockQuality") ? hc::DateTime::kClockQuality : 0));
			out = Value::of_date_time(v);
			return true;
		}
		case ValueType::color: {
			hc::Color v;
			if (!field(json, "red", 255, a) || !field(json, "green", 255, b) || !field(json, "blue", 255, c) ||
				!field(json, "white", 255, d, false)) {
				return false;
			}
			v.red = static_cast<uint8_t>(a);
			v.green = static_cast<uint8_t>(b);
			v.blue = static_cast<uint8_t>(c);
			v.white = static_cast<uint8_t>(d);
			bool rgbw = dpt.main == 251;
			v.valid = rgbw ? hc::Color::kAll : hc::Color::kRgb;
			if (!rgbw && json.has("white")) {
				v.valid |= hc::Color::kWhite;
			}
			if (json.has("valid")) {
				v.valid = 0;
				for (const Json& channel : json["valid"].items) {
					v.valid |= channel.text == "red" ? hc::Color::kRed
						: channel.text == "green"    ? hc::Color::kGreen
						: channel.text == "blue"     ? hc::Color::kBlue
						: channel.text == "white"    ? hc::Color::kWhite
													 : 0x80;
				}
			}
			out = Value::of_color(v);
			return true;
		}
		default: return false;
	}
}

/// Equal values; reals within a relative 1e-9 (the corpus writes decimal approximations).
bool same(const Value& actual, const Value& expected) {
	if (actual.type() == ValueType::real && expected.type() == ValueType::real) {
		double a = actual.as_real();
		double e = expected.as_real();
		return std::fabs(a - e) <= 1e-9 * std::fmax(1.0, std::fabs(e));
	}
	return actual == expected;
}

std::string describe(const Value& value) {
	char text[96];
	return std::string(value_type_name(value.type())) + " " + value.format(text, sizeof(text));
}

/// Runs one corpus file; returns the number of entries checked.
size_t run_corpus(int main_type) {
	char name[32];
	std::snprintf(name, sizeof(name), "dpt-%03d.json", main_type);
	std::string path = std::string(HC_SPEC_DIR) + "/fixtures/dpt/" + name;
	std::ifstream file(path, std::ios::binary);
	REQUIRE_MESSAGE(file.good(), "missing corpus file " << path);
	std::stringstream buffer;
	buffer << file.rdbuf();
	std::string text = buffer.str();
	Json root;
	std::string error;
	REQUIRE_MESSAGE(JsonReader(text).parse(root, error), path << ": " << error);
	REQUIRE(root["main"].number == main_type);
	const Json& entries = root["entries"];
	REQUIRE(entries.kind == Json::Kind::array);
	REQUIRE(!entries.items.empty());

	size_t index = 0;
	for (const Json& entry : entries.items) {
		std::string where = path + " entry " + std::to_string(index++) + " (" + entry["dpt"].text + ")";
		std::optional<Dpt> dpt = Dpt::parse(entry["dpt"].text.c_str());
		REQUIRE_MESSAGE(dpt.has_value(), where << ": invalid dpt");
		CHECK_MESSAGE(dpt->main == main_type, where << ": belongs to another file");
		CHECK_MESSAGE(dpt->supported(), where << ": " << dpt->unsupported_reason());
		std::string direction = entry["direction"].text;
		const Json& error_member = entry["error"];

		std::vector<uint8_t> bytes;
		bool has_bytes = entry.has("bytes");
		if (has_bytes) {
			REQUIRE_MESSAGE(parse_hex(entry["bytes"].text, bytes), where << ": invalid bytes");
		}

		if (error_member.kind == Json::Kind::string) {
			std::string expected = error_member.text;
			if (has_bytes) {
				// A decoding refusal.
				Value value = Value::of_bool(true);
				DptResult result = hc::decode(*dpt, bytes.data(), bytes.size(), value);
				CHECK_MESSAGE(std::string(hc::dpt_status_name(result.status)) == expected,
					where << ": decoding " << entry["bytes"].text << " gave " << hc::dpt_status_name(result.status)
						  << ", expected " << expected);
				CHECK_MESSAGE(value.empty(), where << ": a refused decode leaves no value");
				if (!result) {
					CHECK_MESSAGE(result.reason[0] != '\0', where << ": a refusal gives a reason");
				}
			} else {
				// An encoding refusal.
				Value value;
				std::string actual = "out_of_range";
				DptResult result;
				if (to_value(entry["value"], *dpt, value)) {
					DptBytes out;
					result = hc::encode(*dpt, value, out);
					actual = hc::dpt_status_name(result.status);
					CHECK_MESSAGE(out.length == 0, where << ": a refused encode writes nothing");
					if (!result) {
						CHECK_MESSAGE(result.reason[0] != '\0', where << ": a refusal gives a reason");
					}
				}
				CHECK_MESSAGE(actual == expected, where << ": encoding gave " << actual << ", expected " << expected);
			}
			continue;
		}

		REQUIRE_MESSAGE(has_bytes, where << ": a value entry needs bytes");
		Value expected;
		REQUIRE_MESSAGE(to_value(entry["value"], *dpt, expected), where << ": value does not fit");

		if (direction.empty() || direction == "decode") {
			Value decoded;
			DptResult result = hc::decode(*dpt, bytes.data(), bytes.size(), decoded);
			CHECK_MESSAGE(result.status == DptStatus::ok, where << ": decoding refused: " << result.reason);
			CHECK_MESSAGE(same(decoded, expected),
				where << ": decoded " << describe(decoded) << ", expected " << describe(expected));
			if (direction.empty()) {
				// Identity: the decoded value encodes to the same octets.
				DptBytes again;
				CHECK(hc::encode(*dpt, decoded, again).status == DptStatus::ok);
				CHECK_MESSAGE(hex(again.data, again.length) == hex(bytes.data(), bytes.size()),
					where << ": decode then encode gave " << hex(again.data, again.length));
			}
		}
		if (direction.empty() || direction == "encode") {
			DptBytes encoded;
			DptResult result = hc::encode(*dpt, expected, encoded);
			CHECK_MESSAGE(result.status == DptStatus::ok, where << ": encoding refused: " << result.reason);
			CHECK_MESSAGE(hex(encoded.data, encoded.length) == hex(bytes.data(), bytes.size()),
				where << ": encoded " << hex(encoded.data, encoded.length) << ", expected " << entry["bytes"].text);
			if (direction.empty()) {
				// Identity: the encoded octets decode to the value.
				Value back;
				CHECK(hc::decode(*dpt, encoded, back).status == DptStatus::ok);
				CHECK_MESSAGE(same(back, expected), where << ": encode then decode gave " << describe(back));
			}
		} else {
			CHECK_MESSAGE(direction == "decode", where << ": unknown direction " << direction);
		}
	}
	return index;
}

} // namespace

TEST_CASE("DptCorpus 1 boolean") { run_corpus(1); }
TEST_CASE("DptCorpus 2 controlled") { run_corpus(2); }
TEST_CASE("DptCorpus 3 step") { run_corpus(3); }
TEST_CASE("DptCorpus 4 character") { run_corpus(4); }
TEST_CASE("DptCorpus 5 unsigned 8") { run_corpus(5); }
TEST_CASE("DptCorpus 6 signed 8") { run_corpus(6); }
TEST_CASE("DptCorpus 7 unsigned 16") { run_corpus(7); }
TEST_CASE("DptCorpus 8 signed 16") { run_corpus(8); }
TEST_CASE("DptCorpus 9 float 16") { run_corpus(9); }
TEST_CASE("DptCorpus 10 time of day") { run_corpus(10); }
TEST_CASE("DptCorpus 11 date") { run_corpus(11); }
TEST_CASE("DptCorpus 12 unsigned 32") { run_corpus(12); }
TEST_CASE("DptCorpus 13 signed 32") { run_corpus(13); }
TEST_CASE("DptCorpus 14 float 32") { run_corpus(14); }
TEST_CASE("DptCorpus 16 string") { run_corpus(16); }
TEST_CASE("DptCorpus 17 scene number") { run_corpus(17); }
TEST_CASE("DptCorpus 18 scene control") { run_corpus(18); }
TEST_CASE("DptCorpus 19 date-time") { run_corpus(19); }
TEST_CASE("DptCorpus 20 enumeration") { run_corpus(20); }
TEST_CASE("DptCorpus 232 RGB") { run_corpus(232); }
TEST_CASE("DptCorpus 251 RGBW") { run_corpus(251); }
