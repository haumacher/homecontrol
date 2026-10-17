/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The DPT codec API around the corpus (dpt_corpus_test.cpp has the encodings): type identifiers,
// framing information, value conversion, refusals.
#include <cmath>
#include <cstring>
#include <string>

#include "doctest.h"
#include "hc/dpt.h"

using hc::Dpt;
using hc::DptBytes;
using hc::DptResult;
using hc::DptStatus;
using hc::Value;
using hc::ValueType;

namespace {

Dpt dpt(const char* text) {
	std::optional<Dpt> parsed = Dpt::parse(text);
	REQUIRE_MESSAGE(parsed.has_value(), text);
	return *parsed;
}

std::string format(Dpt d) {
	char text[Dpt::kMaxText];
	return d.format(text, sizeof(text));
}

} // namespace

TEST_CASE("Dpt parse and format") {
	CHECK(dpt("9.001") == Dpt{9, 1});
	CHECK(dpt("9.1") == Dpt{9, 1});
	CHECK(dpt("9") == Dpt{9, 0});
	CHECK(dpt("9.*") == Dpt{9, 0});
	CHECK(dpt("DPST-9-1") == Dpt{9, 1});
	CHECK(dpt("dpst-20-102") == Dpt{20, 102});
	CHECK(dpt("DPT-9") == Dpt{9, 0});
	CHECK(dpt("232.600") == Dpt{232, 600});
	CHECK(dpt("12.1200") == Dpt{12, 1200});
	for (const char* invalid : {"", "9.", ".1", "9.001x", "x9", "0.001", "65536.1", "9.65536", "DPST-9-", "9 .1",
			 "DPST-", "-9", "9..1", "123456"}) {
		CHECK_MESSAGE(!Dpt::parse(invalid).has_value(), invalid);
	}
	CHECK(!Dpt::parse(nullptr).has_value());
	CHECK(format(Dpt{9, 1}) == "9.001");
	CHECK(format(Dpt{16, 0}) == "16.000");
	CHECK(format(Dpt{232, 600}) == "232.600");
	CHECK(format(Dpt{12, 1200}) == "12.1200");
	CHECK(format(Dpt{65535, 65535}) == "65535.65535");
}

TEST_CASE("Dpt framing: embedded types and sizes") {
	for (const char* small : {"1.001", "1.017", "2.001", "3.007", "3.008", "1"}) {
		CHECK_MESSAGE(dpt(small).embedded(), small);
		CHECK_MESSAGE(dpt(small).size() == 1, small);
	}
	// Scene control is a full octet: not embedded, unlike the 6-bit types.
	CHECK(!dpt("18.001").embedded());
	CHECK(!dpt("17.001").embedded());
	CHECK(!dpt("5.001").embedded());
	CHECK(dpt("5.001").size() == 1);
	CHECK(dpt("9.001").size() == 2);
	CHECK(dpt("10.001").size() == 3);
	CHECK(dpt("11.001").size() == 3);
	CHECK(dpt("12.001").size() == 4);
	CHECK(dpt("14.056").size() == 4);
	CHECK(dpt("16.001").size() == 14);
	CHECK(dpt("19.001").size() == 8);
	CHECK(dpt("232.600").size() == 3);
	CHECK(dpt("251.600").size() == 6);
	CHECK(dpt("15.000").size() == 0);
}

TEST_CASE("Dpt value types") {
	CHECK(dpt("1.001").value_type() == ValueType::boolean);
	CHECK(dpt("3.007").value_type() == ValueType::controlled);
	CHECK(dpt("5.001").value_type() == ValueType::real);
	CHECK(dpt("5.010").value_type() == ValueType::unsigned_int);
	CHECK(dpt("7.003").value_type() == ValueType::unsigned_int);
	CHECK(dpt("8.010").value_type() == ValueType::real);
	CHECK(dpt("9.001").value_type() == ValueType::real);
	CHECK(dpt("13.001").value_type() == ValueType::signed_int);
	CHECK(dpt("16.000").value_type() == ValueType::string);
	CHECK(dpt("19.001").value_type() == ValueType::date_time);
	CHECK(dpt("20.102").value_type() == ValueType::enumeration);
	CHECK(dpt("251.600").value_type() == ValueType::color);
	CHECK(dpt("15.000").value_type() == ValueType::none);
}

TEST_CASE("Dpt unsupported types are refused with a reason") {
	for (const char* text : {"15.000", "6.020", "21.001", "232.601", "251.1", "29.010"}) {
		Dpt d = dpt(text);
		CHECK_MESSAGE(!d.supported(), text);
		REQUIRE(d.unsupported_reason() != nullptr);
		DptBytes bytes;
		DptResult result = hc::encode(d, Value::of_unsigned(0), bytes);
		CHECK(result.status == DptStatus::unsupported);
		CHECK(std::strlen(result.reason) > 0);
		CHECK(bytes.length == 0);
		uint8_t data[1] = {0};
		Value value = Value::of_bool(true);
		CHECK(hc::decode(d, data, 1, value).status == DptStatus::unsupported);
		CHECK(value.empty());
	}
	CHECK(dpt("9.001").unsupported_reason() == nullptr);
}

TEST_CASE("Dpt unknown subtypes use the rules of their main type") {
	DptBytes bytes;
	REQUIRE(hc::encode(dpt("9.999"), Value::of_real(21.0), bytes));
	CHECK(bytes.length == 2);
	CHECK(bytes.data[0] == 0x0C);
	CHECK(bytes.data[1] == 0x1A);
	// An unknown 20.x subtype has no known value set: every element is accepted.
	REQUIRE(hc::encode(dpt("20.999"), Value::of_enum(200), bytes));
	CHECK(bytes.data[0] == 200);
	Value value;
	REQUIRE(hc::decode(dpt("20.999"), bytes, value));
	CHECK(value == Value::of_enum(200));
	// The main type alone is the unscaled number, not the first subtype (5.001 would scale).
	const uint8_t octet[1] = {0x2A};
	REQUIRE(hc::decode(dpt("5"), octet, 1, value));
	CHECK(value == Value::of_unsigned(42));
	// 16.000 is ASCII, 16.001 Latin-1, another subtype ASCII.
	CHECK(hc::encode(dpt("16.002"), *Value::of_string("\xc3\xa4"), bytes).status == DptStatus::out_of_range);
}

TEST_CASE("Dpt encode converts numbers exactly") {
	DptBytes bytes;
	// An integer for a real type.
	REQUIRE(hc::encode(dpt("9.001"), Value::of_signed(-5), bytes));
	CHECK(bytes.data[0] == 0x86); // -500 = 0x60C in 11 bits with the sign bit
	CHECK(bytes.data[1] == 0x0C);
	// A real holding an integer for an integer type; an enumeration from an unsigned.
	CHECK(hc::encode(dpt("7.001"), Value::of_real(4660.0), bytes));
	CHECK(bytes.data[0] == 0x12);
	CHECK(hc::encode(dpt("20.102"), Value::of_unsigned(1), bytes));
	CHECK(bytes.data[0] == 1);
	// A fraction for an integer type is refused, not rounded.
	DptResult result = hc::encode(dpt("7.001"), Value::of_real(1.5), bytes);
	CHECK(result.status == DptStatus::wrong_type);
	CHECK(std::string(result.reason) == "an integer is required");
	CHECK(bytes.length == 0);
	CHECK(hc::encode(dpt("5.010"), Value::of_real(21.5), bytes).status == DptStatus::wrong_type);
	CHECK(hc::encode(dpt("13.001"), Value::of_real(1.5), bytes).status == DptStatus::wrong_type);
	// Other kinds never convert.
	CHECK(hc::encode(dpt("1.001"), Value::of_unsigned(1), bytes).status == DptStatus::wrong_type);
	CHECK(hc::encode(dpt("9.001"), Value::of_bool(true), bytes).status == DptStatus::wrong_type);
	CHECK(hc::encode(dpt("16.000"), Value::of_unsigned(1), bytes).status == DptStatus::wrong_type);
	CHECK(hc::encode(dpt("10.001"), Value::of_date(hc::Date{2026, 10, 17}), bytes).status == DptStatus::wrong_type);
	CHECK(hc::encode(dpt("9.001"), Value(), bytes).status == DptStatus::wrong_type);
}

TEST_CASE("Dpt out-of-range values are refused, never clamped") {
	DptBytes bytes;
	DptResult result = hc::encode(dpt("5.001"), Value::of_real(100.5), bytes);
	CHECK(result.status == DptStatus::out_of_range);
	CHECK(std::string(result.reason) == "outside 0..100 %");
	CHECK(bytes.length == 0);
	result = hc::encode(dpt("11.001"), Value::of_date(hc::Date{2090, 1, 1}), bytes);
	CHECK(result.status == DptStatus::out_of_range);
	CHECK(std::string(result.reason) == "year outside 1990..2089");
	result = hc::encode(dpt("9.001"), Value::of_real(-300), bytes);
	CHECK(std::string(result.reason) == "outside -273..670433.28");
	hc::DateTime bad;
	bad.flags = 0x0100;
	CHECK(hc::encode(dpt("19.001"), Value::of_date_time(bad), bytes).status == DptStatus::out_of_range);
	hc::Color color;
	color.valid = 0x10;
	CHECK(hc::encode(dpt("251.600"), Value::of_color(color), bytes).status == DptStatus::out_of_range);
}

TEST_CASE("Dpt decode refuses wrong lengths with a reason") {
	uint8_t data[16] = {};
	Value value;
	DptResult result = hc::decode(dpt("9.001"), data, 3, value);
	CHECK(result.status == DptStatus::wrong_length);
	CHECK(std::string(result.reason) == "2 octets expected");
	CHECK(std::string(hc::decode(dpt("16.000"), data, 15, value).reason) == "14 octets expected");
	CHECK(std::string(hc::decode(dpt("19.001"), data, 0, value).reason) == "8 octets expected");
	CHECK(hc::decode(dpt("1.001"), nullptr, 1, value).status == DptStatus::wrong_length);
}

TEST_CASE("Dpt 2-octet float: every code decodes and re-encodes to itself") {
	// Except 0x7FFF (invalid) and the non-canonical codes: a code whose mantissa fits a smaller
	// exponent encodes with that exponent. The decoded value must survive a round trip exactly.
	unsigned canonical = 0;
	for (uint32_t code = 0; code <= 0xFFFF; ++code) {
		uint8_t data[2] = {static_cast<uint8_t>(code >> 8), static_cast<uint8_t>(code)};
		Value value;
		DptResult result = hc::decode(dpt("9.002"), data, 2, value);
		if (code == 0x7FFF) {
			CHECK(result.status == DptStatus::invalid);
			continue;
		}
		REQUIRE(result);
		DptBytes bytes;
		REQUIRE(hc::encode(dpt("9.002"), value, bytes));
		Value back;
		REQUIRE(hc::decode(dpt("9.002"), bytes, back));
		if (std::fabs(back.as_real() - value.as_real()) > 1e-9 * std::fmax(1.0, std::fabs(value.as_real()))) {
			FAIL_CHECK("code " << code << " decodes to " << value.as_real() << ", re-encodes to " << back.as_real());
		}
		if (bytes.data[0] == data[0] && bytes.data[1] == data[1]) {
			++canonical;
		}
	}
	// Exponent 0 is canonical for all 4096 mantissas, each larger exponent for the 2048 mantissas
	// whose magnitude exceeds the previous exponent's range (minus 0x7FFF).
	CHECK(canonical > 30000);
}

TEST_CASE("Dpt 5.001 percent: every octet survives decode and encode") {
	for (unsigned octet = 0; octet <= 255; ++octet) {
		uint8_t data[1] = {static_cast<uint8_t>(octet)};
		Value value;
		REQUIRE(hc::decode(dpt("5.001"), data, 1, value));
		DptBytes bytes;
		REQUIRE(hc::encode(dpt("5.001"), value, bytes));
		CHECK(bytes.data[0] == octet);
	}
}

TEST_CASE("Dpt string encode refuses invalid UTF-8") {
	DptBytes bytes;
	for (const char* invalid : {"\xc3", "\xc3\x28", "\xe2\x82", "\xc0\x80", "\xed\xa0\x80", "\xff", "a\x80"}) {
		std::optional<Value> value = Value::of_string(invalid);
		REQUIRE(value.has_value());
		CHECK_MESSAGE(hc::encode(dpt("16.001"), *value, bytes).status == DptStatus::wrong_type, invalid);
		CHECK(bytes.length == 0);
	}
	REQUIRE(hc::encode(dpt("16.001"), *Value::of_string("\xc3\xa4"), bytes));
	CHECK(bytes.data[0] == 0xE4);
	CHECK(bytes.data[1] == 0);
}

TEST_CASE("Dpt status names are the corpus names") {
	CHECK(std::string(hc::dpt_status_name(DptStatus::ok)) == "ok");
	CHECK(std::string(hc::dpt_status_name(DptStatus::unsupported)) == "unsupported");
	CHECK(std::string(hc::dpt_status_name(DptStatus::wrong_length)) == "wrong_length");
	CHECK(std::string(hc::dpt_status_name(DptStatus::out_of_range)) == "out_of_range");
	CHECK(std::string(hc::dpt_status_name(DptStatus::invalid)) == "invalid");
	CHECK(std::string(hc::dpt_status_name(DptStatus::wrong_type)) == "wrong_type");
}
