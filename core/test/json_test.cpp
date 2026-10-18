/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <cstring>
#include <deque>
#include <limits>
#include <string>

#include <cstdint>

#include "doctest.h"
#include "hc/json.h"
#include "hc/value_json.h"
#include "json_reader.h"

using hc::JsonSpan;
using hc::JsonWriter;
using hc::Value;
using hc::ValueType;

namespace {

/// Parses `text`; the span points into a copy that lives until the end of the test run.
JsonSpan parse(const std::string& text) {
	static std::deque<std::string> texts;
	texts.push_back(text);
	JsonSpan span;
	REQUIRE(hc::json_parse(texts.back().data(), texts.back().size(), span));
	return span;
}

bool valid(const std::string& text) {
	JsonSpan span;
	return hc::json_parse(text.data(), text.size(), span);
}

} // namespace

TEST_CASE("JsonWriter writes nested values with commas and escapes") {
	char buffer[256];
	JsonWriter json(buffer, sizeof(buffer));
	json.begin_object()
		.member("a", uint32_t(1))
		.member("text", "q\"\\\n\t\x01 ä")
		.key("list").begin_array().number(1.5).boolean(true).null().begin_object().end_object().end_array()
		.member("neg", int32_t(-7))
		.member("nan", std::numeric_limits<double>::quiet_NaN())
		.end_object();
	REQUIRE_FALSE(json.overflowed());
	CHECK(std::string(json.data()) ==
		"{\"a\":1,\"text\":\"q\\\"\\\\\\n\\t\\u0001 ä\",\"list\":[1.5,true,null,{}],\"neg\":-7,\"nan\":null}");
	// The output is valid JSON for the strict test reader too.
	hc_test::Json parsed;
	std::string error;
	CHECK(hc_test::JsonReader(json.data()).parse(parsed, error));
}

TEST_CASE("JsonWriter replaces invalid UTF-8 and prints reals exactly") {
	char buffer[128];
	JsonWriter json(buffer, sizeof(buffer));
	json.begin_array().string("a\xff" "b").number(0.1).number(21.5).number(1e300).number(uint64_t(18446744073709551615ull)).end_array();
	CHECK(std::string(json.data()) == "[\"a\xEF\xBF\xBD" "b\",0.1,21.5,1e+300,18446744073709551615]");
}

TEST_CASE("JsonWriter prints 64-bit integers by hand (the updater's printf has no %llu)") {
	char buffer[160];
	hc::JsonWriter json(buffer, sizeof(buffer));
	json.begin_array()
		.number(static_cast<uint64_t>(0))
		.number(static_cast<uint32_t>(4294967295u))
		.number(static_cast<uint64_t>(18446744073709551615ull))
		.number(static_cast<int64_t>(-1))
		.number(static_cast<int64_t>(INT64_MIN))
		.number(static_cast<int64_t>(INT64_MAX))
		.number(static_cast<int32_t>(-2147483647 - 1))
		.end_array();
	REQUIRE_FALSE(json.overflowed());
	CHECK(std::string(json.data(), json.length()) ==
		"[0,4294967295,18446744073709551615,-1,-9223372036854775808,9223372036854775807,-2147483648]");
}

TEST_CASE("JsonWriter reports overflow and can roll back to a mark") {
	char buffer[16];
	JsonWriter json(buffer, sizeof(buffer));
	json.begin_array();
	JsonWriter::Mark mark = json.mark();
	json.string("this is much too long");
	CHECK(json.overflowed());
	json.reset(mark);
	CHECK_FALSE(json.overflowed());
	json.number(uint32_t(1)).end_array();
	CHECK(std::string(json.data()) == "[1]");
	// reserve() keeps room for the closing.
	char small[12];
	JsonWriter paged(small, sizeof(small));
	paged.reserve(2);
	paged.begin_array();
	int written = 0;
	for (int i = 0; i < 10; ++i) {
		JsonWriter::Mark m = paged.mark();
		paged.number(uint32_t(10 + i));
		if (paged.overflowed()) {
			paged.reset(m);
			break;
		}
		++written;
	}
	paged.release();
	paged.end_array();
	CHECK_FALSE(paged.overflowed());
	CHECK(written == 3);
	CHECK(std::string(paged.data()) == "[10,11,12]");
}

TEST_CASE("JsonReader validates strictly") {
	CHECK(valid("{}"));
	CHECK(valid(" [1, -0.5e3, \"x\", true, false, null, {\"a\": [ ]}] "));
	CHECK_FALSE(valid(""));
	CHECK_FALSE(valid("{"));
	CHECK_FALSE(valid("{\"a\":1,}"));
	CHECK_FALSE(valid("[01]"));
	CHECK_FALSE(valid("[1.]"));
	CHECK_FALSE(valid("\"\x01\""));
	CHECK_FALSE(valid("\"\\x\""));
	CHECK_FALSE(valid("\"\\u12\""));
	CHECK_FALSE(valid("\"\xc3\x28\""));
	CHECK_FALSE(valid("{} {}"));
	CHECK_FALSE(valid("tru"));
	CHECK_FALSE(valid("{a:1}"));
	CHECK_FALSE(valid(std::string(17, '[') + std::string(17, ']')));
	CHECK(valid(std::string(16, '[') + std::string(16, ']')));
	const char* error = nullptr;
	JsonSpan span;
	CHECK_FALSE(hc::json_parse("[1 2]", 5, span, &error));
	CHECK(std::string(error) == "',' or ']' expected");
}

TEST_CASE("JsonReader finds members and unescapes strings") {
	JsonSpan body = parse("{\"type\":\"command\",\"id\":7,\"nested\":{\"x\":[1,{\"y\":2}]},\"s\":\"a\\u00e4\\ud83d\\ude00\\n\",\"id\":8}");
	JsonSpan member;
	REQUIRE(hc::json_member(body, "type", member));
	CHECK(hc::json_string_equals(member, "command"));
	REQUIRE(hc::json_member(body, "id", member));
	double number = 0;
	CHECK(hc::json_number(member, number));
	CHECK(number == 7); // the first of repeated members
	REQUIRE(hc::json_member(body, "nested", member));
	JsonSpan x;
	REQUIRE(hc::json_member(member, "x", x));
	CHECK(x.kind == JsonSpan::Kind::array);
	size_t position = 0;
	JsonSpan item;
	int items = 0;
	while (hc::json_next_item(x, position, item)) {
		++items;
	}
	CHECK(items == 2);
	REQUIRE(hc::json_member(body, "s", member));
	char text[32];
	size_t length = 0;
	REQUIRE(hc::json_string(member, text, sizeof(text), &length));
	CHECK(std::string(text, length) == "a\xc3\xa4\xf0\x9f\x98\x80\n");
	CHECK(hc::json_string_equals(member, "a\xc3\xa4\xf0\x9f\x98\x80\n"));
	CHECK_FALSE(hc::json_member(body, "missing", member));
	// Does not fit.
	char small[4];
	CHECK_FALSE(hc::json_string(member, small, sizeof(small)));
	// A lone surrogate and an embedded NUL are refused.
	CHECK_FALSE(hc::json_string(parse("\"\\ud83d\""), text, sizeof(text)));
	CHECK_FALSE(hc::json_string(parse("\"a\\u0000b\""), text, sizeof(text)));
}

TEST_CASE("ValueJson encode then decode is identity for every type") {
	hc::TimeOfDay t;
	t.day_of_week = 2;
	t.hour = 13;
	t.minute = 45;
	t.second = 30;
	hc::Date d;
	d.year = 2026;
	d.month = 10;
	d.day = 17;
	hc::DateTime dt;
	dt.year = 2026;
	dt.month = 10;
	dt.day = 17;
	dt.day_of_week = 6;
	dt.hour = 14;
	dt.minute = 3;
	dt.second = 12;
	dt.flags = hc::DateTime::kSummerTime | hc::DateTime::kSyncReliable;
	hc::Color c;
	c.red = 255;
	c.green = 128;
	c.blue = 0;
	c.white = 12;
	c.valid = hc::Color::kAll;
	const uint8_t raw[] = {0x00, 0xab, 0xff};
	const Value values[] = {
		Value::of_bool(true),
		Value::of_unsigned(4000000000u),
		Value::of_signed(-123456),
		Value::of_real(21.5),
		Value::of_real(-0.001),
		Value::of_enum(3),
		*Value::of_string("Küche \"links\""),
		*Value::of_raw(raw, sizeof(raw)),
		Value::of_time(t),
		Value::of_date(d),
		Value::of_date_time(dt),
		Value::of_controlled(true, 5),
		Value::of_color(c),
	};
	for (const Value& value : values) {
		char buffer[256];
		JsonWriter json(buffer, sizeof(buffer));
		hc::write_value_json(json, value);
		REQUIRE_FALSE(json.overflowed());
		JsonSpan span = parse(json.data());
		const char* error = nullptr;
		std::optional<Value> back = hc::read_value_json(span, value.type(), &error);
		INFO(json.data());
		REQUIRE(back.has_value());
		CHECK(*back == value);
	}
	char buffer[16];
	JsonWriter json(buffer, sizeof(buffer));
	hc::write_value_json(json, Value());
	CHECK(std::string(json.data()) == "null");
}

TEST_CASE("ValueJson refuses values that do not fit the type") {
	const char* error = nullptr;
	CHECK_FALSE(hc::read_value_json(parse("1"), ValueType::boolean, &error));
	CHECK(std::string(error).find("true or false") != std::string::npos);
	CHECK_FALSE(hc::read_value_json(parse("1.5"), ValueType::unsigned_int, &error));
	CHECK_FALSE(hc::read_value_json(parse("-1"), ValueType::unsigned_int, &error));
	CHECK_FALSE(hc::read_value_json(parse("\"1\""), ValueType::real, &error));
	CHECK(hc::read_value_json(parse("2.0"), ValueType::unsigned_int, &error) == Value::of_unsigned(2));
	CHECK(hc::read_value_json(parse("2"), ValueType::real, &error) == Value::of_real(2));
	CHECK_FALSE(hc::read_value_json(parse("\"this string is longer than 28 bytes\""), ValueType::string, &error));
	CHECK_FALSE(hc::read_value_json(parse("\"abc\""), ValueType::raw, &error));
	CHECK_FALSE(hc::read_value_json(parse("{\"day\":0,\"hour\":24,\"minute\":0,\"second\":0}"), ValueType::time_of_day, &error));
	CHECK_FALSE(hc::read_value_json(parse("{\"day\":0,\"hour\":1,\"minute\":0}"), ValueType::time_of_day, &error));
	CHECK(std::string(error).find("missing") != std::string::npos);
	CHECK_FALSE(hc::read_value_json(parse("{\"control\":1,\"value\":2}"), ValueType::controlled, &error));
	CHECK_FALSE(hc::read_value_json(parse("{\"red\":256,\"green\":0,\"blue\":0}"), ValueType::color, &error));
	CHECK(hc::read_value_json(parse("{\"red\":1,\"green\":2,\"blue\":3}"), ValueType::color, &error).has_value());
	CHECK_FALSE(hc::read_value_json(parse("true"), ValueType::none, &error));
}
