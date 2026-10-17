/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The Improv Wi-Fi serial frames byte for byte. The checksums were computed by hand: the magic
// "IMPROV" sums to 0x1DD, so a frame's checksum is 0xDD plus the sum of version, type, length and
// data, modulo 256.

#include <string>
#include <vector>

#include "doctest.h"
#include "hc/improv_protocol.h"

using hc::ImprovReader;
namespace improv = hc::improv;

namespace {

using Bytes = std::vector<uint8_t>;

Bytes frame(std::initializer_list<uint8_t> tail) {
	Bytes result = {'I', 'M', 'P', 'R', 'O', 'V'};
	result.insert(result.end(), tail);
	return result;
}

Bytes text(const char* s) {
	return Bytes(s, s + std::char_traits<char>::length(s));
}

/// Feeds `bytes` and collects what comes out.
struct Collector {
	ImprovReader reader;
	std::string console;
	std::vector<Bytes> frames; ///< type followed by data
	std::vector<std::string> bad;
	uint64_t now = 1000;

	void take(ImprovReader::Event event) {
		switch (event) {
		case ImprovReader::Event::none:
			break;
		case ImprovReader::Event::console:
			console.append(reinterpret_cast<const char*>(reader.console_data()), reader.console_length());
			break;
		case ImprovReader::Event::frame: {
			Bytes f = {reader.frame_type()};
			f.insert(f.end(), reader.frame_data(), reader.frame_data() + reader.frame_length());
			frames.push_back(f);
			break;
		}
		case ImprovReader::Event::bad_frame:
			bad.push_back(reader.bad_reason());
			break;
		}
	}

	void feed(const Bytes& bytes) {
		for (uint8_t b : bytes) {
			take(reader.feed(b, now));
		}
	}

	void poll(uint64_t at) {
		now = at;
		take(reader.poll(at));
	}
};

} // namespace

TEST_CASE("ImprovProtocol.StateAndErrorFramesAreByteExact") {
	uint8_t out[improv::kMaxFrame];
	size_t n = improv::build_current_state(improv::State::authorized, out, sizeof(out));
	CHECK(Bytes(out, out + n) == frame({0x01, 0x01, 0x01, 0x02, 0xE2}));
	n = improv::build_current_state(improv::State::provisioning, out, sizeof(out));
	CHECK(Bytes(out, out + n) == frame({0x01, 0x01, 0x01, 0x03, 0xE3}));
	n = improv::build_current_state(improv::State::provisioned, out, sizeof(out));
	CHECK(Bytes(out, out + n) == frame({0x01, 0x01, 0x01, 0x04, 0xE4}));
	n = improv::build_error(improv::Error::none, out, sizeof(out));
	CHECK(Bytes(out, out + n) == frame({0x01, 0x02, 0x01, 0x00, 0xE1}));
	n = improv::build_error(improv::Error::invalid_rpc, out, sizeof(out));
	CHECK(Bytes(out, out + n) == frame({0x01, 0x02, 0x01, 0x01, 0xE2}));
	n = improv::build_error(improv::Error::unknown_rpc, out, sizeof(out));
	CHECK(Bytes(out, out + n) == frame({0x01, 0x02, 0x01, 0x02, 0xE3}));
	n = improv::build_error(improv::Error::unable_to_connect, out, sizeof(out));
	CHECK(Bytes(out, out + n) == frame({0x01, 0x02, 0x01, 0x03, 0xE4}));
	CHECK(improv::build_error(improv::Error::none, out, 9) == 0); // no room for the checksum
}

TEST_CASE("ImprovProtocol.RpcResultLayout") {
	uint8_t out[improv::kMaxFrame];
	// The empty result that ends a network list.
	size_t n = improv::build_rpc_result(improv::kRpcGetWifiNetworks, nullptr, 0, out, sizeof(out));
	CHECK(Bytes(out, out + n) == frame({0x01, 0x04, 0x02, 0x04, 0x00, 0xE8}));

	// The device URL after provisioning: [command, total, length, bytes].
	const char* url[] = {"http://homecontrol-a1b2c3.local/"};
	n = improv::build_rpc_result(improv::kRpcWifiSettings, url, 1, out, sizeof(out));
	Bytes expected = frame({0x01, 0x04, 35, 0x01, 33, 32});
	Bytes u = text(url[0]);
	expected.insert(expected.end(), u.begin(), u.end());
	expected.push_back(0x9A);
	CHECK(Bytes(out, out + n) == expected);

	const char* strings[] = {"A", "BC"};
	n = improv::build_rpc_result(improv::kRpcGetDeviceInfo, strings, 2, out, sizeof(out));
	CHECK(Bytes(out, out + n) == frame({0x01, 0x04, 0x07, 0x03, 0x05, 0x01, 'A', 0x02, 'B', 'C',
		static_cast<uint8_t>((0xDD + 1 + 4 + 7 + 3 + 5 + 1 + 'A' + 2 + 'B' + 'C') & 0xFF)}));

	// Strings that do not fit 255 data bytes are refused, not truncated.
	std::string big(200, 'x');
	const char* too_much[] = {big.c_str(), big.c_str()};
	CHECK(improv::build_rpc_result(improv::kRpcGetDeviceInfo, too_much, 2, out, sizeof(out)) == 0);
}

TEST_CASE("ImprovProtocol.ParseWifiSettings") {
	char ssid[33];
	char psk[65];
	Bytes ok = {3, 'N', 'e', 't', 8, 's', 'e', 'c', 'r', 'e', 't', '1', '2'};
	REQUIRE(improv::parse_wifi_settings(ok.data(), ok.size(), ssid, sizeof(ssid), psk, sizeof(psk)));
	CHECK(std::string(ssid) == "Net");
	CHECK(std::string(psk) == "secret12");

	Bytes open = {3, 'N', 'e', 't', 0};
	REQUIRE(improv::parse_wifi_settings(open.data(), open.size(), ssid, sizeof(ssid), psk, sizeof(psk)));
	CHECK(std::string(psk).empty());

	Bytes short_by_one = {3, 'N', 'e', 't', 2, 'x'};
	CHECK_FALSE(improv::parse_wifi_settings(short_by_one.data(), short_by_one.size(), ssid, sizeof(ssid), psk, sizeof(psk)));
	Bytes trailing = {3, 'N', 'e', 't', 0, 'x'};
	CHECK_FALSE(improv::parse_wifi_settings(trailing.data(), trailing.size(), ssid, sizeof(ssid), psk, sizeof(psk)));
	Bytes empty_ssid = {0, 0};
	CHECK_FALSE(improv::parse_wifi_settings(empty_ssid.data(), empty_ssid.size(), ssid, sizeof(ssid), psk, sizeof(psk)));
	CHECK_FALSE(improv::parse_wifi_settings(ok.data(), 0, ssid, sizeof(ssid), psk, sizeof(psk)));
	char small[3];
	CHECK_FALSE(improv::parse_wifi_settings(ok.data(), ok.size(), small, sizeof(small), psk, sizeof(psk)));
}

TEST_CASE("ImprovReader.ClientFrameRoundTrip") {
	Collector c;
	// wifi settings "Net" / "secret12": the payload length is 0x0D, a carriage return inside the
	// binary frame that must neither end it nor reach the console.
	Bytes rpc = frame({0x01, 0x03, 0x0F, 0x01, 0x0D, 3, 'N', 'e', 't', 8, 's', 'e', 'c', 'r', 'e', 't', '1', '2', 0x19});
	c.feed(rpc);
	REQUIRE(c.frames.size() == 1);
	Bytes expected = {improv::kTypeRpcCommand, 0x01, 0x0D, 3, 'N', 'e', 't', 8, 's', 'e', 'c', 'r', 'e', 't', '1', '2'};
	CHECK(c.frames[0] == expected);
	CHECK(c.console.empty());
	CHECK(c.bad.empty());
	CHECK_FALSE(c.reader.holding());

	// get current state, get device info, back to back.
	c.feed(frame({0x01, 0x03, 0x02, 0x02, 0x00, 0xE5}));
	c.feed(frame({0x01, 0x03, 0x02, 0x03, 0x00, 0xE6}));
	REQUIRE(c.frames.size() == 3);
	CHECK(c.frames[1] == Bytes{0x03, 0x02, 0x00});
	CHECK(c.frames[2] == Bytes{0x03, 0x03, 0x00});
}

TEST_CASE("ImprovReader.ConsoleTextPassesThroughAtOnce") {
	Collector c;
	ImprovReader::Event event = c.reader.feed('h', c.now);
	CHECK(event == ImprovReader::Event::console);
	c.take(event);
	c.feed(text("elp\r"));
	CHECK(c.console == "help\r");
	CHECK(c.frames.empty());
}

TEST_CASE("ImprovReader.MagicLookalikesAreReleasedAsText") {
	Collector c;
	c.feed(text("IMP"));
	CHECK(c.console.empty()); // held: could be a frame
	CHECK(c.reader.holding());
	c.feed(text("ORT x\r"));
	CHECK(c.console == "IMPORT x\r");
	CHECK_FALSE(c.reader.holding());

	// "IMPROVE": the version byte decides that this is text.
	Collector d;
	d.feed(text("say IMPROVED\r"));
	CHECK(d.console == "say IMPROVED\r");
	CHECK(d.frames.empty());
	CHECK(d.bad.empty());
}

TEST_CASE("ImprovReader.TheByteThatBreaksAMatchMayStartAFrame") {
	Collector c;
	Bytes input = text("xIIMPROV");
	Bytes rest = {0x01, 0x03, 0x02, 0x02, 0x00, 0xE5};
	input.insert(input.end(), rest.begin(), rest.end());
	c.feed(input);
	CHECK(c.console == "xI");
	REQUIRE(c.frames.size() == 1);
	CHECK(c.frames[0] == Bytes{0x03, 0x02, 0x00});
}

TEST_CASE("ImprovReader.BadChecksumIsReportedAndTheNextMagicResynchronizes") {
	Collector c;
	c.feed(frame({0x01, 0x03, 0x02, 0x02, 0x00, 0xE4})); // checksum off by one
	CHECK(c.frames.empty());
	REQUIRE(c.bad.size() == 1);
	CHECK(c.bad[0] == "checksum mismatch");
	CHECK(c.console.empty()); // binary never reaches the console
	c.feed(text("ab"));
	c.feed(frame({0x01, 0x03, 0x02, 0x02, 0x00, 0xE5}));
	CHECK(c.console == "ab");
	REQUIRE(c.frames.size() == 1);
	CHECK(c.frames[0] == Bytes{0x03, 0x02, 0x00});
}

TEST_CASE("ImprovReader.StalledFramesAreGivenUpAfterTheGap") {
	Collector c;
	c.feed(text("IMP"));
	c.poll(c.now + ImprovReader::kGapMs - 1);
	CHECK(c.console.empty());
	c.poll(c.now + 1);
	CHECK(c.console == "IMP");
	CHECK_FALSE(c.reader.holding());

	// Past the version byte the bytes are binary: dropped, reported.
	c.feed(frame({0x01, 0x03, 0x05, 0x01}));
	c.poll(c.now + ImprovReader::kGapMs);
	REQUIRE(c.bad.size() == 1);
	CHECK(c.bad[0] == "incomplete frame");
	CHECK(c.console == "IMP");

	// flush() releases held magic bytes at once.
	c.feed(text("IM"));
	c.take(c.reader.flush());
	CHECK(c.console == "IMPIM");
}
