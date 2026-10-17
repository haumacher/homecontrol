/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The POSIX UDP socket over the loopback interface.

#include <string>
#include <vector>

#include "doctest.h"
#include "hc/common/bsd_udp.h"
#include "hc/event_loop.h"
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
