/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The POSIX UDP socket over the loopback interface.

#include <string>
#include <vector>

#include "doctest.h"
#include "hc/common/bsd_udp.h"
#include "hc/connectivity.h"
#include "hc/event_loop.h"
#include "hc/knx/gateway_search.h"
#include "hc/posix/udp_socket.h"
#include "test_support.h"

namespace {

constexpr uint32_t kLoopback = 0x7F000001;

struct Receiver : hc::UdpReceiver {
	void udp_received(const uint8_t* data, size_t length, const hc::Ipv4Endpoint& from_) override {
		datagrams.emplace_back(reinterpret_cast<const char*>(data), length);
		from = from_;
	}
	void udp_error(const char* reason) override { errors.push_back(reason); }
	void udp_resolved(bool ok_, uint32_t address_, const char* error_) override {
		resolved = true;
		ok = ok_;
		address = address_;
		error = error_;
	}

	std::vector<std::string> datagrams;
	std::vector<std::string> errors;
	hc::Ipv4Endpoint from;
	bool resolved = false;
	bool ok = false;
	uint32_t address = 0;
	std::string error;
};

} // namespace

TEST_CASE("PosixUdp.LoopbackExchange") {
	hc_test::ScopedCapture capture;
	hc::EventLoop loop;
	hc::posix::PosixUdpSocket socket(loop);
	Receiver receiver;
	const char* error = nullptr;
	REQUIRE(socket.open(receiver, &error));
	CHECK(socket.is_open());
	hc::Ipv4Endpoint local;
	REQUIRE(socket.local_endpoint(hc::Ipv4Endpoint{kLoopback, 3671}, local, &error));
	CHECK(local.address == kLoopback);
	CHECK(local.port != 0);

	char text[hc::bsd::kMaxError];
	int peer = hc::bsd::udp_open(text, sizeof(text));
	REQUIRE(peer >= 0);
	hc::Ipv4Endpoint peer_endpoint;
	REQUIRE(hc::bsd::udp_local_endpoint(peer, hc::Ipv4Endpoint{kLoopback, 1}, peer_endpoint, text, sizeof(text)));
	const uint8_t hello[] = {'h', 'e', 'l', 'l', 'o'};
	REQUIRE(hc::bsd::udp_send_to(peer, hc::Ipv4Endpoint{kLoopback, local.port}, hello, sizeof(hello), text, sizeof(text)));
	for (int i = 0; i < 20 && receiver.datagrams.empty(); ++i) {
		socket.wait(50);
	}
	REQUIRE(receiver.datagrams.size() == 1);
	CHECK(receiver.datagrams[0] == "hello");
	CHECK(receiver.from == peer_endpoint);

	const uint8_t back[] = {'o', 'k'};
	REQUIRE(socket.send_to(peer_endpoint, back, sizeof(back), &error));
	uint8_t buffer[16];
	hc::Ipv4Endpoint from;
	int length = 0;
	for (int i = 0; i < 100 && length == 0; ++i) {
		length = hc::bsd::udp_receive(peer, buffer, sizeof(buffer), from, text, sizeof(text));
		if (length == 0) {
			socket.wait(10);
		}
	}
	CHECK(length == 2);
	CHECK(from.port == local.port);
	hc::bsd::udp_close(peer);

	socket.close();
	CHECK_FALSE(socket.is_open());
	CHECK_FALSE(socket.send_to(peer_endpoint, back, sizeof(back), &error));
	CHECK(std::string(error) == "socket is closed");
}

TEST_CASE("PosixUdp.ResolvesLocalhostThroughTheLoop") {
	hc::EventLoop loop;
	hc::posix::PosixUdpSocket socket(loop);
	Receiver receiver;
	const char* error = nullptr;
	REQUIRE(socket.resolve("localhost", receiver, &error));
	CHECK_FALSE(receiver.resolved); // never from within the call
	loop.run_once(0);
	REQUIRE(receiver.resolved);
	CHECK(receiver.ok);
	CHECK(receiver.address == kLoopback);
}

TEST_CASE("PosixUdp.GatewaySearchEndToEnd") {
	// The search over the real socket, against a fake gateway on the loopback interface: the request
	// goes to the gateway's port instead of the multicast group, so nothing leaves the host.
	hc_test::ScopedCapture capture;
	hc_test::FakeClock clock;
	hc::EventLoop loop;
	hc_test::FakeNetworkDriver driver{clock};
	hc::Connectivity connectivity{loop, clock, driver};
	connectivity.start();
	connectivity.link_up("127.0.0.1");

	char text[hc::bsd::kMaxError];
	int gateway = hc::bsd::udp_open(text, sizeof(text));
	REQUIRE(gateway >= 0);
	hc::Ipv4Endpoint gateway_endpoint;
	REQUIRE(hc::bsd::udp_local_endpoint(gateway, hc::Ipv4Endpoint{kLoopback, 1}, gateway_endpoint, text, sizeof(text)));

	hc::posix::PosixUdpSocketFactory sockets(loop);
	hc::knx::GatewaySearch search(loop, clock, connectivity, sockets, gateway_endpoint);
	CHECK(sockets.current() == nullptr); // nothing while idle
	const char* error = nullptr;
	bool started = false;
	REQUIRE(search.start(&error, &started));
	REQUIRE(started);
	REQUIRE(sockets.current() != nullptr);
	hc::posix::PosixUdpSocket& socket = *sockets.current();

	// The gateway reads the SEARCH_REQUEST and answers to its HPAI, with its own control endpoint.
	uint8_t request[64];
	hc::Ipv4Endpoint from;
	int length = 0;
	for (int i = 0; i < 100 && length == 0; ++i) {
		length = hc::bsd::udp_receive(gateway, request, sizeof(request), from, text, sizeof(text));
		if (length == 0) {
			socket.wait(10);
		}
	}
	REQUIRE(length == 14);
	CHECK(request[0] == 0x06);
	CHECK(request[2] == 0x02);
	CHECK(request[3] == 0x01);
	hc::Ipv4Endpoint hpai{static_cast<uint32_t>(request[8] << 24 | request[9] << 16 | request[10] << 8 | request[11]),
		static_cast<uint16_t>(request[12] << 8 | request[13])};
	CHECK(hpai.address == kLoopback);
	CHECK(hpai == from);
	std::vector<uint8_t> answer = {0x06, 0x10, 0x02, 0x02, 0x00, 0x4E, 0x08, 0x01, 0x7F, 0x00, 0x00, 0x01,
		static_cast<uint8_t>(gateway_endpoint.port >> 8), static_cast<uint8_t>(gateway_endpoint.port),
		0x36, 0x01, 0x02, 0x00, 0x11, 0x00, 0x00, 0x00, 0x00, 0x83, 0x12, 0x34, 0x56, 0x78, 0xE0, 0x00, 0x17, 0x0C,
		0x00, 0x24, 0x6D, 0x01, 0x02, 0x03};
	const char name[30] = "Loopback router";
	answer.insert(answer.end(), name, name + 30);
	for (uint8_t octet : {0x0A, 0x02, 0x02, 0x01, 0x03, 0x01, 0x04, 0x01, 0x05, 0x01}) {
		answer.push_back(octet);
	}
	REQUIRE(answer.size() == 78);
	REQUIRE(hc::bsd::udp_send_to(gateway, hpai, answer.data(), answer.size(), text, sizeof(text)));
	for (int i = 0; i < 40 && search.count() == 0; ++i) {
		socket.wait(50);
		loop.run_once(clock.now_ms);
	}
	REQUIRE(search.count() == 1);
	CHECK(std::string(search.gateway(0).name) == "Loopback router");
	CHECK(search.gateway(0).control == gateway_endpoint);
	CHECK(search.gateway(0).tunnelling);

	// The window closes on the loop's clock; the socket is given back.
	clock.advance(hc::knx::GatewaySearch::kWindowMs);
	loop.run_once(clock.now_ms);
	loop.run_once(clock.now_ms);
	CHECK(search.state() == hc::knx::SearchState::done);
	CHECK(sockets.current() == nullptr); // closed and deleted
	CHECK(search.count() == 1);
	// A second search takes a new socket and gives it back again.
	REQUIRE(search.start(&error, &started));
	CHECK(sockets.current() != nullptr);
	clock.advance(hc::knx::GatewaySearch::kWindowMs);
	loop.run_once(clock.now_ms);
	loop.run_once(clock.now_ms);
	CHECK(sockets.current() == nullptr);
	hc::bsd::udp_close(gateway);
}
