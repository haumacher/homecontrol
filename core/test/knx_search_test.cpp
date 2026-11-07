/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The KNXnet/IP gateway search (doc/knx.md "Gateway search", spec/api.md "KNX gateway search"): the
// SEARCH_REQUEST byte for byte, the SEARCH_RESPONSE of a typical IP router, refused datagrams,
// deduplication, the state machine under the fake clock and the route /api/knx/search.

#include <string>
#include <vector>

#include "doctest.h"
#include "hc/knx/gateway_search.h"
#include "knx_test_support.h"
#include "web_test_support.h"

using hc::knx::FoundGateway;
using hc::knx::GatewaySearch;
using hc::knx::SearchState;
using hc_test::Bytes;
using hc_test::bytes;
using hc_test::hex;
namespace ip = hc::knx::ip;

namespace {

/// The SEARCH_REQUEST of the controller at 192.0.2.100:50100 (FakeUdpSocket's local endpoint):
/// header 06 10, SEARCH_REQUEST 02 01, total length 14, then the discovery endpoint's HPAI (length
/// 8, UDP over IPv4, the address, the port).
const char* kSearchRequest = "06 10 02 01 00 0E 08 01 C0 00 02 64 C3 B4";

/// The SEARCH_RESPONSE of a typical KNX IP router at 192.0.2.30:3671 (documentation range), 78
/// octets: the header; the control endpoint's HPAI; the device information DIB (54 octets: length,
/// type 01, medium TP1, device status 0, individual address 1.1.0, project-installation id 0,
/// serial number 0083:12345678, routing multicast 224.0.23.12, MAC 00:24:6d:01:02:03, friendly name
/// "IP-Router Küche" in ISO 8859-1, NUL-padded to 30 octets); the supported service families DIB
/// (10 octets: core, device management, tunnelling and routing, each version 1).
const char* kRouterResponse =
	"06 10 02 02 00 4E"
	" 08 01 C0 00 02 1E 0E 57"
	" 36 01 02 00 11 00 00 00 00 83 12 34 56 78 E0 00 17 0C 00 24 6D 01 02 03"
	" 49 50 2D 52 6F 75 74 65 72 20 4B FC 63 68 65 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"
	" 0A 02 02 01 03 01 04 01 05 01";

/// A response with the control endpoint `address`:`port`, the individual address 1.1.<device>
/// and the service families `families` (each family with version 1).
Bytes response(uint32_t address, uint16_t port, uint8_t device, const std::vector<uint8_t>& families,
	const char* name = "Gateway") {
	Bytes frame = bytes("06 10 02 02 00 00 08 01");
	for (int shift = 24; shift >= 0; shift -= 8) {
		frame.push_back(static_cast<uint8_t>(address >> shift));
	}
	frame.push_back(static_cast<uint8_t>(port >> 8));
	frame.push_back(static_cast<uint8_t>(port));
	Bytes device_info = bytes("36 01 02 00 11 00 00 00 00 83 00 00 00 01 E0 00 17 0C 00 24 6D 00 00 01");
	device_info[5] = device;
	device_info[23] = device;
	frame.insert(frame.end(), device_info.begin(), device_info.end());
	for (size_t i = 0; i < 30; ++i) {
		frame.push_back(static_cast<uint8_t>(name[0] != '\0' && i < std::string(name).size() ? name[i] : 0));
	}
	frame.push_back(static_cast<uint8_t>(2 + 2 * families.size()));
	frame.push_back(0x02);
	for (uint8_t family : families) {
		frame.push_back(family);
		frame.push_back(1);
	}
	frame[4] = static_cast<uint8_t>(frame.size() >> 8);
	frame[5] = static_cast<uint8_t>(frame.size());
	return frame;
}

constexpr uint32_t kRouterIp = 0xC000021E; // 192.0.2.30
const hc::Ipv4Endpoint kRouter{kRouterIp, 3671};
const hc::Ipv4Endpoint kGroup{GatewaySearch::kMulticastAddress, 3671};

struct SearchFixture {
	hc_test::ScopedCapture capture{hc::LogLevel::debug};
	hc_test::FakeClock clock;
	hc::EventLoop loop;
	hc_test::FakeNetworkDriver driver{clock};
	hc::Connectivity connectivity{loop, clock, driver};
	hc_test::FakeUdpSocketFactory sockets{clock};
	GatewaySearch search{loop, clock, connectivity, sockets};

	hc_test::FakeUdpSocket& socket() { return sockets.last(); }

	SearchFixture() {
		connectivity.start();
		connectivity.link_up("192.0.2.100");
	}

	void run_until(uint64_t until_ms) {
		for (;;) {
			loop.run_once(clock.now_ms);
			uint64_t deadline = loop.next_deadline();
			if (deadline > until_ms) {
				clock.now_ms = until_ms;
				loop.run_once(clock.now_ms);
				return;
			}
			if (deadline > clock.now_ms) {
				clock.now_ms = deadline;
			}
		}
	}

	void run_for(uint64_t ms) { run_until(clock.now_ms + ms); }

	void start() {
		const char* error = nullptr;
		bool started = false;
		REQUIRE(search.start(&error, &started));
		REQUIRE(started);
	}
};

} // namespace

TEST_CASE("KnxSearch.RequestBytes") {
	ip::FrameBuffer frame;
	ip::build_search_request(frame, hc::Ipv4Endpoint{hc_test::kLocalIp, hc_test::kLocalPort});
	CHECK(hex(frame) == kSearchRequest);

	SearchFixture f;
	f.start();
	REQUIRE(f.socket().sent.size() == 1);
	CHECK(f.socket().sent[0].to == kGroup);
	CHECK(hex(f.socket().sent[0].data) == kSearchRequest);
	CHECK(GatewaySearch::kMulticastAddress == 0xE000170C); // 224.0.23.12
}

TEST_CASE("KnxSearch.ParsesTheRouterResponse") {
	Bytes frame = bytes(kRouterResponse);
	REQUIRE(frame.size() == 78);
	ip::SearchResponse parsed;
	REQUIRE(ip::parse_search_response(frame.data(), frame.size(), parsed));
	CHECK(parsed.control == kRouter);

	SearchFixture f;
	f.start();
	f.socket().inject(frame, kRouter);
	REQUIRE(f.search.count() == 1);
	const FoundGateway& g = f.search.gateway(0);
	CHECK(g.control == kRouter);
	char text[16];
	CHECK(std::string(g.address.format(text, sizeof(text))) == "1.1.0");
	CHECK(std::string(g.name) == "IP-Router K\xC3\xBC" "che"); // UTF-8 from ISO 8859-1
	CHECK(hex(Bytes(g.mac, g.mac + 6)) == "00 24 6D 01 02 03");
	CHECK(hex(Bytes(g.serial, g.serial + 6)) == "00 83 12 34 56 78");
	CHECK(g.tunnelling);
	CHECK(g.routing);
}

TEST_CASE("KnxSearch.RefusesTruncatedAndForeignDatagrams") {
	SearchFixture f;
	f.start();
	Bytes frame = bytes(kRouterResponse);
	uint32_t expected = 0;
	for (size_t n = 0; n < frame.size(); ++n) {
		CAPTURE(n);
		Bytes cut(frame.begin(), frame.begin() + static_cast<long>(n));
		f.socket().inject(cut, kRouter); // the total length disagrees with the datagram
		++expected;
		if (n >= 6) {
			cut[4] = static_cast<uint8_t>(n >> 8);
			cut[5] = static_cast<uint8_t>(n); // the length agrees, a structure is cut short or missing
			f.socket().inject(cut, kRouter);
			++expected;
		}
	}
	// Foreign: not KNXnet/IP, another service, a version 2 header, a DIB of length 0 or beyond the end.
	for (const char* foreign : {"68 65 6C 6C 6F", "06 10 04 21 00 0A 04 07 03 00", "06 20 02 02 00 08 00 00"}) {
		f.socket().inject(bytes(foreign), kRouter);
		++expected;
	}
	Bytes zero_dib = frame;
	zero_dib[14] = 0x00;
	f.socket().inject(zero_dib, kRouter);
	Bytes long_dib = frame;
	long_dib[68] = 0x20;
	f.socket().inject(long_dib, kRouter);
	Bytes odd_services = frame;
	odd_services[68] = 0x09;
	f.socket().inject(odd_services, kRouter);
	Bytes short_device = frame;
	short_device[14] = 0x35;
	f.socket().inject(short_device, kRouter);
	expected += 4;
	CHECK(f.search.count() == 0);
	CHECK(f.search.ignored() == expected);

	// Garbage of every length, some with a valid header in front: never a crash, never a gateway.
	uint32_t seed = 12345;
	auto next = [&seed]() {
		seed = seed * 1103515245u + 12345u;
		return static_cast<uint8_t>(seed >> 16);
	};
	for (int round = 0; round < 2000; ++round) {
		Bytes garbage(static_cast<size_t>(next() % 120));
		for (uint8_t& octet : garbage) {
			octet = next();
		}
		if (round % 2 == 0 && garbage.size() >= 6) {
			garbage[0] = 0x06;
			garbage[1] = 0x10;
			garbage[2] = 0x02;
			garbage[3] = 0x02;
			garbage[4] = 0;
			garbage[5] = static_cast<uint8_t>(garbage.size());
		}
		f.socket().inject(garbage, kRouter);
	}
	CHECK(f.search.count() == 0);
	CHECK(f.search.state() == SearchState::running);
}

TEST_CASE("KnxSearch.StateMachine") {
	SearchFixture f;
	CHECK(f.search.state() == SearchState::idle);
	CHECK(std::string(hc::knx::search_state_name(f.search.state())) == "idle");
	CHECK(f.search.started_ms() == 0);
	CHECK(f.sockets.created == 0); // nothing while idle
	f.start();
	uint64_t started = f.clock.now_ms;
	CHECK(f.sockets.live() == 1);
	CHECK(f.search.state() == SearchState::running);
	CHECK(f.search.ends_ms() == started + GatewaySearch::kWindowMs);
	CHECK(f.socket().opened);

	// A second start while it runs: the running one, nothing sent again.
	const char* error = nullptr;
	bool again = true;
	CHECK(f.search.start(&error, &again));
	CHECK_FALSE(again);
	CHECK(f.socket().sent.size() == 1);
	CHECK(f.socket().opens == 1);

	f.socket().inject(bytes(kRouterResponse), kRouter);
	f.run_for(GatewaySearch::kRepeatMs);
	REQUIRE(f.socket().sent.size() == 2); // the repeated request
	CHECK(f.socket().sent[1].at == started + GatewaySearch::kRepeatMs);
	CHECK(hex(f.socket().sent[1].data) == kSearchRequest);
	f.socket().inject(bytes(kRouterResponse), kRouter); // the router's answer to the repeat
	CHECK(f.search.count() == 1);

	// An interface that only tunnels, and an old router that only routes.
	f.socket().inject(response(0xC000021F, 3671, 1, {0x02, 0x03, 0x04}), hc::Ipv4Endpoint{0xC000021F, 3671});
	f.socket().inject(response(0xC0000220, 3671, 2, {0x02, 0x05}), hc::Ipv4Endpoint{0xC0000220, 3671});
	REQUIRE(f.search.count() == 3);
	CHECK(f.search.gateway(1).tunnelling);
	CHECK_FALSE(f.search.gateway(1).routing);
	CHECK_FALSE(f.search.gateway(2).tunnelling);
	CHECK(f.search.gateway(2).routing);

	f.run_until(started + GatewaySearch::kWindowMs - 1);
	CHECK(f.search.state() == SearchState::running);
	f.run_until(started + GatewaySearch::kWindowMs);
	CHECK(f.search.state() == SearchState::done);
	CHECK_FALSE(f.socket().opened);
	CHECK(f.sockets.live() == 0); // the socket is given back with the window
	CHECK(f.socket().sent.size() == 2);
	CHECK(f.search.count() == 3); // the result stays
	CHECK(std::string(f.search.gateway(2).name) == "Gateway"); // in the table shrunk to what was found
	// A late answer (the socket is closed; a platform that still delivers one) is not taken.
	Bytes late = response(0xC0000221, 3671, 3, {0x02, 0x04});
	f.search.udp_received(late.data(), late.size(), hc::Ipv4Endpoint{0xC0000221, 3671});
	CHECK(f.search.count() == 3);
	f.run_for(10000);
	CHECK(f.socket().sent.size() == 2);

	// A new search forgets the old result.
	f.start();
	CHECK(f.search.state() == SearchState::running);
	CHECK(f.search.count() == 0);
	CHECK(f.search.ignored() == 0);
	CHECK(f.sockets.created == 2);
	CHECK(f.sockets.live() == 1);
	CHECK(f.socket().sent.size() == 1);
	f.run_for(GatewaySearch::kWindowMs);
	CHECK(f.search.state() == SearchState::done);
	CHECK(f.search.count() == 0);
	CHECK(f.sockets.live() == 0);
}

TEST_CASE("KnxSearch.RepeatedSearchesLeakNothing") {
	// start -> done -> start ...: every socket the search took comes back to the factory.
	SearchFixture f;
	for (int round = 0; round < 20; ++round) {
		f.start();
		if (round % 2 == 0) {
			f.socket().inject(bytes(kRouterResponse), kRouter);
		}
		f.run_for(GatewaySearch::kWindowMs);
		CHECK(f.search.state() == SearchState::done);
		CHECK(f.sockets.live() == 0);
	}
	CHECK(f.sockets.created == 20);
	CHECK(f.sockets.released == 20);
	// A search running when its owner ends gives its socket back too.
	{
		hc_test::FakeUdpSocketFactory sockets{f.clock};
		{
			GatewaySearch search{f.loop, f.clock, f.connectivity, sockets};
			const char* error = nullptr;
			REQUIRE(search.start(&error));
			CHECK(sockets.live() == 1);
		}
		CHECK(sockets.live() == 0);
	}
}

TEST_CASE("KnxSearch.Deduplicates") {
	SearchFixture f;
	f.start();
	f.socket().inject(bytes(kRouterResponse), kRouter);
	f.socket().inject(bytes(kRouterResponse), kRouter);
	CHECK(f.search.count() == 1);
	// The same device on another port is another endpoint.
	f.socket().inject(response(kRouterIp, 3672, 0, {0x02, 0x04}), kRouter);
	CHECK(f.search.count() == 2);
	// A gateway behind NAT names 0.0.0.0:0: the endpoint is where it answered from, once.
	const hc::Ipv4Endpoint nat_sender{0xC0000240, 40000};
	f.socket().inject(response(0, 0, 9, {0x02, 0x04}), nat_sender);
	f.socket().inject(response(0, 0, 9, {0x02, 0x04}), nat_sender);
	REQUIRE(f.search.count() == 3);
	CHECK(f.search.gateway(2).control == nat_sender);
}

TEST_CASE("KnxSearch.CapacityIsBounded") {
	SearchFixture f;
	f.start();
	for (uint32_t i = 0; i < GatewaySearch::kCapacity + 3; ++i) {
		hc::Ipv4Endpoint from{0xC0000201 + i, 3671};
		f.socket().inject(response(from.address, 3671, static_cast<uint8_t>(i), {0x02, 0x04}), from);
	}
	CHECK(f.search.count() == GatewaySearch::kCapacity);
	CHECK(f.search.overflow() == 3);
}

TEST_CASE("KnxSearch.RefusalsSpeak") {
	SearchFixture f;
	f.connectivity.link_down("cable pulled");
	const char* error = nullptr;
	CHECK_FALSE(f.search.start(&error));
	CHECK(std::string(error) == "the network is not up");
	CHECK(f.search.state() == SearchState::idle);
	CHECK(f.sockets.created == 0);
	CHECK(f.capture.sink.contains("gateway search refused: the network is not up"));

	// The platform has no memory for the socket (the ESP's heap floor): refused with its reason.
	f.connectivity.link_up("192.0.2.100");
	f.sockets.fail_create = true;
	CHECK_FALSE(f.search.start(&error));
	CHECK(std::string(error) == "no socket for the search: not enough memory");
	CHECK(f.search.state() == SearchState::idle);
	CHECK(f.capture.sink.contains("gateway search refused: no socket for the search: not enough memory"));
	f.sockets.fail_create = false;

	f.sockets.fail_open = true;
	CHECK_FALSE(f.search.start(&error));
	CHECK(std::string(error).find("no sockets left") != std::string::npos);
	CHECK(f.search.state() == SearchState::idle);
	CHECK(f.sockets.live() == 0); // the socket that did not open is given back
	f.sockets.fail_open = false;
	f.start();
	CHECK(f.search.state() == SearchState::running);
}

// ---- The route --------------------------------------------------------------------------------

namespace {

struct SearchApiFixture : hc_test::WebFixture {
	std::string cookie;
	hc_test::FakeUdpSocketFactory sockets{fake.clock};
	GatewaySearch search{controller.loop(), fake.clock, controller.connectivity(), sockets};

	hc_test::FakeUdpSocket& socket() { return sockets.last(); }

	SearchApiFixture() {
		controller.start();
		cookie = onboard();
	}

	Result call(hc::HttpMethod method, const std::string& body = "", bool csrf = true) {
		return request(method, "/api/knx/search", body, cookie, "", csrf);
	}

	void advance(uint64_t ms) {
		uint64_t end = fake.clock.now_ms + ms;
		while (fake.clock.now_ms < end) {
			fake.clock.advance(50);
			controller.loop().run_once(fake.clock.now_ms);
		}
	}
};

} // namespace

TEST_CASE("KnxSearch.Api") {
	SearchApiFixture f;
	auto r = f.call(hc::HttpMethod::get);
	CHECK(r.status == 501);
	CHECK(r.json["code"].text == "system.unavailable");
	f.api.set_knx_search(&f.search);

	// The principal and the CSRF header, as for every route.
	CHECK(f.request(hc::HttpMethod::get, "/api/knx/search").status == 401);
	CHECK(f.request(hc::HttpMethod::post, "/api/knx/search", "", "", "", true).status == 401);
	CHECK(f.call(hc::HttpMethod::post, "", false).json["code"].text == "auth.csrf");
	CHECK(f.call(hc::HttpMethod::put).status == 405);

	r = f.call(hc::HttpMethod::get);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["state"].text == "idle");
	CHECK(r.json["gateways"].items.empty());

	// Network down: 409 with the reason.
	r = f.call(hc::HttpMethod::post);
	CHECK(r.status == 409);
	CHECK(r.json["code"].text == "knx.search_refused");
	CHECK(r.json["error"].text == "the network is not up");

	f.controller.connectivity().link_up("192.0.2.100");
	f.sockets.fail_create = true;
	r = f.call(hc::HttpMethod::post);
	CHECK(r.status == 409);
	CHECK(r.json["error"].text == "no socket for the search: not enough memory");
	f.sockets.fail_create = false;
	r = f.call(hc::HttpMethod::post);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["state"].text == "running");
	CHECK(r.json["started"].boolean);
	CHECK(r.json["window_ms"].number == 3000);
	CHECK(r.json["ends_ms"].number - r.json["started_ms"].number == 3000);
	r = f.call(hc::HttpMethod::post);
	CHECK(r.status == 200);
	CHECK(r.json["state"].text == "running");
	CHECK_FALSE(r.json["started"].boolean);
	CHECK(f.socket().sent.size() == 1);

	f.socket().inject(bytes(kRouterResponse), kRouter);
	f.socket().inject(response(0xC0000220, 3700, 2, {0x02, 0x05}, "Old router"), hc::Ipv4Endpoint{0xC0000220, 3700});
	f.advance(GatewaySearch::kWindowMs);
	r = f.call(hc::HttpMethod::get);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["state"].text == "done");
	REQUIRE(r.json["gateways"].items.size() == 2);
	const hc_test::Json& router = r.json["gateways"].items[0];
	CHECK(router["name"].text == "IP-Router K\xC3\xBC" "che");
	CHECK(router["host"].text == "192.0.2.30");
	CHECK(router["port"].number == 3671);
	CHECK(router["individual_address"].text == "1.1.0");
	CHECK(router["mac"].text == "00:24:6d:01:02:03");
	CHECK(router["serial"].text == "0083:12345678");
	CHECK(router["tunnelling"].boolean);
	CHECK(router["routing"].boolean);
	const hc_test::Json& old = r.json["gateways"].items[1];
	CHECK(old["name"].text == "Old router");
	CHECK(old["port"].number == 3700);
	CHECK_FALSE(old["tunnelling"].boolean);
	CHECK(old["routing"].boolean);
	CHECK(r.json["overflow"].number == 0);
}
