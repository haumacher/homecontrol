/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "hc/websocket.h"

namespace ws = hc::websocket;
using Event = ws::FrameDecoder::Event;

namespace {

/// A client frame (masked) with the given payload.
std::vector<uint8_t> client_frame(ws::Opcode opcode, bool fin, const std::string& payload,
	const uint8_t mask[4] = nullptr) {
	static const uint8_t kMask[4] = {0x37, 0xfa, 0x21, 0x3d};
	if (mask == nullptr) {
		mask = kMask;
	}
	uint8_t header[ws::kMaxHeader];
	size_t n = ws::encode_header(header, opcode, fin, payload.size(), mask);
	std::vector<uint8_t> frame(header, header + n);
	std::vector<uint8_t> body(payload.begin(), payload.end());
	ws::apply_mask(body.data(), body.size(), mask);
	frame.insert(frame.end(), body.begin(), body.end());
	return frame;
}

struct Decoded {
	Event event;
	ws::Opcode opcode;
	std::string payload;
};

/// Feeds `bytes` in pieces of `step` and collects the events.
std::vector<Decoded> decode(ws::FrameDecoder& decoder, const std::vector<uint8_t>& bytes, size_t step = 1 << 20) {
	std::vector<Decoded> events;
	size_t at = 0;
	while (at < bytes.size()) {
		size_t piece = bytes.size() - at < step ? bytes.size() - at : step;
		size_t offset = 0;
		while (offset < piece) {
			size_t consumed = 0;
			Event e = decoder.push(bytes.data() + at + offset, piece - offset, consumed);
			offset += consumed;
			if (e == Event::need_more) {
				break;
			}
			events.push_back({e, decoder.opcode(),
				e == Event::error ? std::string() : std::string(reinterpret_cast<const char*>(decoder.data()), decoder.length())});
			if (e == Event::error) {
				return events;
			}
		}
		at += piece;
	}
	return events;
}

} // namespace

TEST_CASE("WebSocketAcceptKey matches RFC 6455 section 1.3") {
	char out[ws::kAcceptKeyLength + 1];
	REQUIRE(ws::accept_key("dGhlIHNhbXBsZSBub25jZQ==", out));
	CHECK(std::string(out) == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
	CHECK_FALSE(ws::accept_key("dGhlIHNhbXBsZSBub25jZQ=", out));   // too short
	CHECK_FALSE(ws::accept_key("dGhlIHNhbXBsZSBub25jZQ!=", out));  // not base64
	CHECK_FALSE(ws::accept_key("dGhlIHNhbXBsZSBub25jZR==", out));  // stray low bits
	CHECK_FALSE(ws::accept_key(nullptr, out));
}

TEST_CASE("WebSocketDecoder reads the RFC example frame") {
	// RFC 6455 section 5.7: a masked "Hello".
	const std::vector<uint8_t> frame = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58};
	uint8_t buffer[64];
	ws::FrameDecoder decoder(buffer, sizeof(buffer));
	auto events = decode(decoder, frame);
	REQUIRE(events.size() == 1);
	CHECK(events[0].event == Event::message);
	CHECK(events[0].opcode == ws::Opcode::text);
	CHECK(events[0].payload == "Hello");
}

TEST_CASE("WebSocketDecoder encode then decode is identity for every length class") {
	for (size_t length : {size_t(0), size_t(1), size_t(125), size_t(126), size_t(65535), size_t(65536), size_t(70000)}) {
		std::string payload(length, 'x');
		for (size_t i = 0; i < length; ++i) {
			payload[i] = static_cast<char>('a' + i % 26);
		}
		std::vector<uint8_t> buffer(80000);
		for (size_t step : {size_t(1), size_t(7), size_t(1) << 20}) {
			if (length > 1000 && step == 1) {
				continue; // byte-wise feeding of the long ones takes too long to be useful
			}
			ws::FrameDecoder decoder(buffer.data(), buffer.size());
			auto events = decode(decoder, client_frame(ws::Opcode::binary, true, payload), step);
			REQUIRE(events.size() == 1);
			CHECK(events[0].event == Event::message);
			CHECK(events[0].opcode == ws::Opcode::binary);
			CHECK(events[0].payload == payload);
		}
	}
}

TEST_CASE("WebSocketDecoder reassembles fragments around a ping") {
	uint8_t buffer[64];
	ws::FrameDecoder decoder(buffer, sizeof(buffer));
	std::vector<uint8_t> bytes = client_frame(ws::Opcode::text, false, "Hel");
	auto ping = client_frame(ws::Opcode::ping, true, "p");
	bytes.insert(bytes.end(), ping.begin(), ping.end());
	auto rest = client_frame(ws::Opcode::continuation, true, "lo");
	bytes.insert(bytes.end(), rest.begin(), rest.end());
	for (size_t step : {size_t(1), size_t(3), size_t(100)}) {
		ws::FrameDecoder d(buffer, sizeof(buffer));
		auto events = decode(d, bytes, step);
		REQUIRE(events.size() == 2);
		CHECK(events[0].event == Event::control);
		CHECK(events[0].opcode == ws::Opcode::ping);
		CHECK(events[0].payload == "p");
		CHECK(events[1].event == Event::message);
		CHECK(events[1].payload == "Hello");
	}
}

TEST_CASE("WebSocketDecoder refuses protocol violations with the right close code") {
	uint8_t buffer[16];
	struct Case {
		std::vector<uint8_t> bytes;
		uint16_t code;
	};
	std::vector<Case> cases;
	// Unmasked client frame.
	cases.push_back({{0x81, 0x01, 'a'}, ws::kCloseProtocolError});
	// Reserved bit.
	auto rsv = client_frame(ws::Opcode::text, true, "a");
	rsv[0] |= 0x40;
	cases.push_back({rsv, ws::kCloseProtocolError});
	// Unknown opcode 3.
	auto unknown = client_frame(ws::Opcode::text, true, "a");
	unknown[0] = 0x83;
	cases.push_back({unknown, ws::kCloseProtocolError});
	// Fragmented ping.
	cases.push_back({client_frame(ws::Opcode::ping, false, "a"), ws::kCloseProtocolError});
	// Continuation without a start.
	cases.push_back({client_frame(ws::Opcode::continuation, true, "a"), ws::kCloseProtocolError});
	// Too long for the buffer.
	cases.push_back({client_frame(ws::Opcode::text, true, std::string(17, 'a')), ws::kCloseTooBig});
	// Invalid UTF-8 in a text message.
	cases.push_back({client_frame(ws::Opcode::text, true, "\xc3\x28"), ws::kCloseInvalidPayload});
	// Close frame with one byte.
	cases.push_back({client_frame(ws::Opcode::close, true, "x"), ws::kCloseProtocolError});
	// A new message inside a fragmented one.
	auto two = client_frame(ws::Opcode::text, false, "a");
	auto second = client_frame(ws::Opcode::text, true, "b");
	two.insert(two.end(), second.begin(), second.end());
	cases.push_back({two, ws::kCloseProtocolError});
	// Length 5 encoded in 16 bits (not minimal).
	cases.push_back({{0x81, 0xFE, 0x00, 0x05, 0, 0, 0, 0, 'a', 'b', 'c', 'd', 'e'}, ws::kCloseProtocolError});
	for (const Case& c : cases) {
		ws::FrameDecoder decoder(buffer, sizeof(buffer));
		auto events = decode(decoder, c.bytes);
		REQUIRE(!events.empty());
		CHECK(events.back().event == Event::error);
		CHECK(decoder.close_code() == c.code);
		CHECK(std::strlen(decoder.error()) > 0);
		// Stays failed.
		size_t consumed = 0;
		CHECK(decoder.push(c.bytes.data(), c.bytes.size(), consumed) == Event::error);
	}
}

TEST_CASE("WebSocketHeader and close payload encode as specified") {
	uint8_t header[ws::kMaxHeader];
	CHECK(ws::encode_header(header, ws::Opcode::text, true, 5) == 2);
	CHECK(header[0] == 0x81);
	CHECK(header[1] == 0x05);
	CHECK(ws::encode_header(header, ws::Opcode::binary, true, 256) == 4);
	CHECK(header[1] == 126);
	CHECK(header[2] == 1);
	CHECK(header[3] == 0);
	CHECK(ws::encode_header(header, ws::Opcode::binary, false, 65536) == 10);
	CHECK(header[0] == 0x02);
	CHECK(header[1] == 127);
	CHECK(header[7] == 1);
	uint8_t close[ws::kMaxControlPayload];
	size_t n = ws::encode_close(close, 1013, "busy");
	CHECK(n == 6);
	CHECK(close[0] == 0x03);
	CHECK(close[1] == 0xF5);
	// A long reason is cut to 125 bytes without splitting a UTF-8 sequence.
	std::string reason;
	for (int i = 0; i < 70; ++i) {
		reason += "\xc3\xa4";
	}
	n = ws::encode_close(close, 1000, reason.c_str());
	CHECK(n <= ws::kMaxControlPayload);
	CHECK(ws::utf8_valid(close + 2, n - 2));
}
