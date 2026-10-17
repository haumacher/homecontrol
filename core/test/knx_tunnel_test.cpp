/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The KNXnet/IP tunnel's state machine under the fake clock, against a fake gateway.

#include <string>
#include <vector>

#include "doctest.h"
#include "hc/knx/group_value.h"
#include "hc/knx/tunnel.h"
#include "knx_test_support.h"

using hc::knx::GroupAddress;
using hc::knx::KnxTunnel;
using hc::knx::LData;
using hc::knx::SendOutcome;
using hc::knx::TunnelState;
using hc_test::Bytes;
using hc_test::bytes;
using hc_test::hex;
using hc_test::TunnelFixture;
namespace ip = hc::knx::ip;

namespace {

const char* kConnectRequest = "06 10 02 05 00 1A 08 01 C0 00 02 64 C3 B4 08 01 C0 00 02 64 C3 B4 04 04 02 00";
const char* kHeartbeat = "06 10 02 07 00 10 07 00 08 01 C0 00 02 64 C3 B4";
const char* kDisconnect = "06 10 02 09 00 10 07 00 08 01 C0 00 02 64 C3 B4";

LData switch_on(uint8_t sub = 1) {
	LData frame;
	uint8_t on = 1;
	hc::knx::make_group_request(frame, GroupAddress::of(1, 0, sub), hc::knx::GroupService::write, &on, 1, true);
	return frame;
}

/// The gateway's L_Data.con for switch_on(sub) (source: the tunnel's address 1.1.250).
Bytes confirmation(uint8_t sub = 1, bool error = false) {
	Bytes cemi = bytes("2E 00 BC E0 11 FA 08 01 01 00 81");
	cemi[7] = sub;
	if (error) {
		cemi[2] |= 0x01;
	}
	return cemi;
}

std::string tunneling_request(uint8_t sequence, uint8_t sub = 1) {
	ip::FrameBuffer frame;
	ip::build_tunneling_request(frame, 7, sequence, switch_on(sub));
	return hex(frame);
}

} // namespace

TEST_CASE("KnxTunnel.WaitsForGatewayAndNetwork") {
	TunnelFixture f;
	f.connectivity.link_down("cable pulled");
	f.tunnel.start();
	f.run_for(5000);
	CHECK(f.socket.sent.empty());
	CHECK(f.capture.sink.contains("I hc.knx: knx tunnel waiting: no gateway set (knx.gateway)"));
	f.set_gateway("192.0.2.1");
	f.run_for(5000);
	CHECK(f.socket.sent.empty());
	CHECK(f.capture.sink.contains("I hc.knx: knx tunnel waiting: network not up"));
	CHECK(f.tunnel.state() == TunnelState::down);

	// The network comes up (Connectivity reconnects after its backoff): the tunnel connects at once.
	f.run_for(1000);
	f.connectivity.link_up("192.0.2.100");
	f.run_for(0);
	REQUIRE(f.socket.sent.size() == 1);
	CHECK(f.socket.sent[0].to == hc_test::kGateway);
	CHECK(hex(f.socket.sent[0].data) == kConnectRequest);
	CHECK(f.tunnel.state() == TunnelState::connecting);
	CHECK(f.capture.sink.contains("I hc.knx: knx tunnel connecting to 192.0.2.1:3671 (attempt 1)"));
}

TEST_CASE("KnxTunnel.ConnectAndHeartbeat") {
	TunnelFixture f;
	f.connect(7);
	CHECK(f.recorder.states == std::vector<std::string>{"down->connecting", "connecting->up"});
	CHECK(f.tunnel.channel() == 7);
	CHECK(f.tunnel.address().raw == 0x11FA);
	CHECK(f.capture.sink.contains(
		"I hc.knx: knx tunnel up: channel 7, address 1.1.250, gateway 192.0.2.1:3671 (data endpoint 192.0.2.1:3671)"));
	uint64_t up_at = f.clock.now_ms;

	f.run_until(up_at + 59999);
	CHECK(f.socket.sent.size() == 1);
	f.run_until(up_at + 60000);
	REQUIRE(f.socket.sent.size() == 2);
	CHECK(hex(f.socket.last().data) == kHeartbeat);
	CHECK(f.socket.last().to == hc_test::kGateway);
	f.clock.advance(20);
	f.socket.inject(hc_test::connectionstate_response(7));
	f.run_until(up_at + 120000);
	CHECK(f.socket.sent.size() == 3);
	f.socket.inject(hc_test::connectionstate_response(7));
	f.run_until(up_at + 179999);
	CHECK(f.socket.sent.size() == 3);
	CHECK(f.tunnel.is_up());
}

TEST_CASE("KnxTunnel.ConnectTimeoutRetriesWithBackoff") {
	TunnelFixture f;
	f.set_gateway("192.0.2.1");
	f.tunnel.start();
	f.run_for(0);
	uint64_t start = f.clock.now_ms;
	f.run_until(start + 9999);
	CHECK(f.tunnel.state() == TunnelState::connecting);
	f.run_until(start + 10000);
	CHECK(f.tunnel.state() == TunnelState::down);
	CHECK(f.capture.sink.contains("W hc.knx: knx tunnel down: no CONNECT_RESPONSE within 10 s; reconnect in 1 s (failure 1)"));
	CHECK(f.tunnel.next_attempt_at() == start + 11000);
	CHECK(f.socket.closes == 1);
	f.run_until(start + 11000);
	CHECK(f.socket.count(ip::Service::connect_request) == 2);
	f.run_until(start + 21000);
	CHECK(f.capture.sink.contains("reconnect in 2 s (failure 2)"));
	f.run_until(start + 23000);
	CHECK(f.socket.count(ip::Service::connect_request) == 3);
	// Success resets the backoff.
	f.socket.inject(hc_test::connect_response(7));
	CHECK(f.tunnel.is_up());
	CHECK(f.tunnel.backoff().failures() == 0);
}

TEST_CASE("KnxTunnel.ConnectRefused") {
	TunnelFixture f;
	f.set_gateway("192.0.2.1");
	f.tunnel.start();
	f.run_for(0);
	f.socket.inject(hc_test::connect_response(0, ip::status::kNoMoreConnections));
	CHECK(f.tunnel.state() == TunnelState::down);
	CHECK(std::string(f.tunnel.reason()) == "gateway refused the connection: E_NO_MORE_CONNECTIONS (0x24)");
	CHECK(f.capture.sink.contains("reconnect in 1 s (failure 1)"));
	f.run_for(1000);
	CHECK(f.socket.count(ip::Service::connect_request) == 2);
}

TEST_CASE("KnxTunnel.ThreeMissedHeartbeatsReconnect") {
	TunnelFixture f;
	f.connect(7);
	uint64_t up_at = f.clock.now_ms;
	f.run_until(up_at + 60000);
	CHECK(f.socket.count(ip::Service::connectionstate_request) == 1);
	f.run_until(up_at + 70000);
	CHECK(f.socket.count(ip::Service::connectionstate_request) == 2);
	CHECK(f.capture.sink.contains("W hc.knx: knx heartbeat unanswered within 10 s (1 of 3), repeating"));
	f.run_until(up_at + 80000);
	CHECK(f.socket.count(ip::Service::connectionstate_request) == 3);
	CHECK(f.tunnel.is_up());
	f.run_until(up_at + 90000);
	CHECK(f.tunnel.state() == TunnelState::down);
	CHECK(hex(f.socket.last().data) == kDisconnect);
	CHECK(f.capture.sink.contains(
		"W hc.knx: knx tunnel down: no answer to 3 heartbeats (CONNECTIONSTATE_REQUEST); reconnect in 1 s (failure 1)"));
	f.run_until(up_at + 91000);
	CHECK(f.socket.count(ip::Service::connect_request) == 2);
	CHECK(f.tunnel.state() == TunnelState::connecting);
}

TEST_CASE("KnxTunnel.AnsweredRepetitionKeepsTheTunnel") {
	TunnelFixture f;
	f.connect(7);
	uint64_t up_at = f.clock.now_ms;
	f.run_until(up_at + 70000);
	f.socket.inject(hc_test::connectionstate_response(7));
	f.run_until(up_at + 119999);
	CHECK(f.tunnel.is_up());
	CHECK(f.socket.count(ip::Service::connectionstate_request) == 2);
}

TEST_CASE("KnxTunnel.HeartbeatErrorStatusReconnects") {
	TunnelFixture f;
	f.connect(7);
	f.run_for(60000);
	f.socket.inject(hc_test::connectionstate_response(7, ip::status::kConnectionId));
	CHECK(f.tunnel.state() == TunnelState::down);
	CHECK(std::string(f.tunnel.reason()) == "heartbeat answered with E_CONNECTION_ID (0x21)");
	CHECK(hex(f.socket.last().data) == kDisconnect);
}

TEST_CASE("KnxTunnel.SendWaitsForAckAndConfirmation") {
	TunnelFixture f;
	f.connect(7);
	size_t base = f.socket.sent.size();
	uint32_t first = f.tunnel.send(switch_on(1));
	uint32_t second = f.tunnel.send(switch_on(2));
	CHECK(first != 0);
	CHECK(second != 0);
	CHECK(first != second);
	REQUIRE(f.socket.sent.size() == base + 1);
	CHECK(hex(f.socket.last().data) == tunneling_request(0, 1));
	CHECK(f.socket.last().to == hc_test::kGateway); // the data endpoint of the CONNECT_RESPONSE
	CHECK(f.tunnel.queued() == 2);

	f.socket.inject(hc_test::tunneling_ack(7, 0));
	CHECK(f.socket.sent.size() == base + 1); // the second waits for the confirmation of the first
	f.socket.inject(hc_test::tunneling_request(7, 0, confirmation(1)));
	CHECK(hex(f.socket.last().data) == tunneling_request(1, 2));
	REQUIRE(f.recorder.sent.size() == 1);
	CHECK(f.recorder.sent[0].token == first);
	CHECK(f.recorder.sent[0].outcome == SendOutcome::confirmed);
	CHECK(f.recorder.sent[0].frame.source.raw == 0x11FA);
	// The confirmation was acknowledged (sequence 0 of the gateway).
	CHECK(f.socket.sent[f.socket.sent.size() - 2].data == hc_test::tunneling_ack(7, 0));

	// No confirmation for the second within 3 s: reported, the queue moves on.
	f.socket.inject(hc_test::tunneling_ack(7, 1));
	f.run_for(2999);
	CHECK(f.recorder.sent.size() == 1);
	f.run_for(1);
	REQUIRE(f.recorder.sent.size() == 2);
	CHECK(f.recorder.sent[1].token == second);
	CHECK(f.recorder.sent[1].outcome == SendOutcome::no_confirmation);
	CHECK(f.tunnel.queued() == 0);
	CHECK(f.tunnel.is_up());
}

TEST_CASE("KnxTunnel.NegativeAndEarlyConfirmation") {
	TunnelFixture f;
	f.connect(7);
	f.tunnel.send(switch_on(1));
	// The confirmation overtakes the acknowledgement.
	f.socket.inject(hc_test::tunneling_request(7, 0, confirmation(1, true)));
	CHECK(f.recorder.sent.empty());
	f.socket.inject(hc_test::tunneling_ack(7, 0));
	REQUIRE(f.recorder.sent.size() == 1);
	CHECK(f.recorder.sent[0].outcome == SendOutcome::negative);
}

TEST_CASE("KnxTunnel.AckTimeoutRepeatsOnceThenReconnects") {
	TunnelFixture f;
	f.connect(7);
	uint32_t token = f.tunnel.send(switch_on(1));
	f.tunnel.send(switch_on(2));
	uint64_t sent_at = f.clock.now_ms;
	f.run_until(sent_at + 999);
	CHECK(f.socket.count(ip::Service::tunneling_request) == 1);
	f.run_until(sent_at + 1000);
	REQUIRE(f.socket.count(ip::Service::tunneling_request) == 2);
	CHECK(hex(f.socket.last().data) == tunneling_request(0, 1)); // same frame, same sequence
	CHECK(f.capture.sink.contains("W hc.knx: no TUNNELING_ACK for sequence 0 within 1 s, repeating"));
	// An ACK with an error status does not count.
	f.socket.inject(hc_test::tunneling_ack(7, 0, ip::status::kTunnellingLayer));
	f.run_until(sent_at + 2000);
	CHECK(f.tunnel.state() == TunnelState::down);
	CHECK(std::string(f.tunnel.reason()) == "no TUNNELING_ACK for sequence 0 after a repetition");
	CHECK(hex(f.socket.last().data) == kDisconnect);
	REQUIRE(f.recorder.sent.size() == 2);
	CHECK(f.recorder.sent[0].token == token);
	CHECK(f.recorder.sent[0].outcome == SendOutcome::lost);
	CHECK(f.recorder.sent[1].outcome == SendOutcome::lost);
	f.run_until(sent_at + 3000);
	CHECK(f.socket.count(ip::Service::connect_request) == 2);
	// Reconnected: the sequence counters start over, nothing old is sent.
	f.socket.inject(hc_test::connect_response(8));
	CHECK(f.tunnel.queued() == 0);
	CHECK(f.tunnel.channel() == 8);
}

TEST_CASE("KnxTunnel.ReceiveSequenceRule") {
	TunnelFixture f;
	f.connect(7);
	Bytes ind = bytes("29 00 BC D0 11 0A 08 01 01 00 81");
	size_t base = f.socket.sent.size();

	f.socket.inject(hc_test::tunneling_request(7, 0, ind));
	CHECK(f.recorder.frames.size() == 1);
	CHECK(f.socket.last().data == hc_test::tunneling_ack(7, 0));

	// The gateway missed our ACK and repeats: acknowledged again, not delivered.
	f.socket.inject(hc_test::tunneling_request(7, 0, ind));
	CHECK(f.recorder.frames.size() == 1);
	CHECK(f.socket.sent.size() == base + 2);
	CHECK(f.socket.last().data == hc_test::tunneling_ack(7, 0));

	// Out of sequence: ignored, no ACK.
	f.socket.inject(hc_test::tunneling_request(7, 5, ind));
	CHECK(f.recorder.frames.size() == 1);
	CHECK(f.socket.sent.size() == base + 2);
	CHECK(f.capture.sink.contains("W hc.knx: ignoring TUNNELING_REQUEST with sequence 5 (expected 1)"));

	f.socket.inject(hc_test::tunneling_request(7, 1, ind));
	CHECK(f.recorder.frames.size() == 2);
	CHECK(f.socket.last().data == hc_test::tunneling_ack(7, 1));

	// Another channel, another sender: ignored.
	f.socket.inject(hc_test::tunneling_request(9, 2, ind));
	f.socket.inject(hc_test::tunneling_request(7, 2, ind), hc::Ipv4Endpoint{0xC0000202, 3671});
	CHECK(f.recorder.frames.size() == 2);
	CHECK(f.socket.sent.size() == base + 3);

	// A cEMI frame that does not decode is acknowledged (the sequence moves on) and dropped.
	f.socket.inject(hc_test::tunneling_request(7, 2, bytes("FC 00 00 01 01 00")));
	CHECK(f.socket.last().data == hc_test::tunneling_ack(7, 2));
	CHECK(f.capture.sink.contains("W hc.knx: dropping a tunnelled frame: cEMI message code is not L_Data.req/con/ind"));
	f.socket.inject(hc_test::tunneling_request(7, 3, ind));
	CHECK(f.recorder.frames.size() == 3);

	// The counter wraps: 255, then 0, and a repeated 255 after 0 is not "the previous".
	for (int s = 4; s <= 255; ++s) {
		f.socket.inject(hc_test::tunneling_request(7, static_cast<uint8_t>(s), ind));
	}
	f.socket.inject(hc_test::tunneling_request(7, 0, ind));
	CHECK(f.recorder.frames.size() == 3 + 252 + 1);
	f.socket.inject(hc_test::tunneling_request(7, 0, ind));
	CHECK(f.recorder.frames.size() == 256);
}

TEST_CASE("KnxTunnel.GatewayDisconnects") {
	TunnelFixture f;
	f.connect(7);
	f.socket.inject(hc_test::disconnect_request(7));
	CHECK(f.tunnel.state() == TunnelState::down);
	CHECK(hex(f.socket.last().data) == "06 10 02 0A 00 08 07 00");
	CHECK(std::string(f.tunnel.reason()) == "the gateway closed the tunnel (DISCONNECT_REQUEST)");
	f.run_for(1000);
	CHECK(f.socket.count(ip::Service::connect_request) == 2);
}

TEST_CASE("KnxTunnel.NetworkDownTearsDownUpRebuilds") {
	TunnelFixture f;
	f.connect(7);
	f.tunnel.send(switch_on(1));
	size_t base = f.socket.sent.size();
	f.connectivity.link_down("beacon timeout");
	CHECK(f.tunnel.state() == TunnelState::down);
	CHECK(std::string(f.tunnel.reason()) == "network down");
	CHECK(f.socket.sent.size() == base); // no DISCONNECT_REQUEST over a dead link
	CHECK(f.recorder.sent.size() == 1);
	CHECK(f.recorder.sent[0].outcome == SendOutcome::lost);
	CHECK(f.tunnel.next_attempt_at() == hc::EventLoop::kNever);
	f.run_for(30000);
	CHECK(f.socket.count(ip::Service::connect_request) == 1);

	f.connectivity.link_up("192.0.2.100");
	f.run_for(0);
	CHECK(f.socket.count(ip::Service::connect_request) == 2);
	f.socket.inject(hc_test::connect_response(9));
	CHECK(f.tunnel.is_up());
	CHECK(f.recorder.states.back() == "connecting->up");
}

TEST_CASE("KnxTunnel.NetworkDownWhileWaitingForARetry") {
	TunnelFixture f;
	f.set_gateway("192.0.2.1");
	f.tunnel.start();
	f.run_for(0);
	f.socket.inject(hc_test::connect_response(0, ip::status::kNoMoreConnections));
	CHECK(f.tunnel.next_attempt_at() != hc::EventLoop::kNever);
	f.connectivity.link_down("beacon timeout");
	CHECK(f.tunnel.next_attempt_at() == hc::EventLoop::kNever);
	f.run_for(5000);
	CHECK(f.socket.count(ip::Service::connect_request) == 1);
}

TEST_CASE("KnxTunnel.StopDisconnectsGracefully") {
	TunnelFixture f;
	f.connect(7);
	f.tunnel.stop();
	CHECK(f.tunnel.state() == TunnelState::down);
	CHECK(hex(f.socket.last().data) == kDisconnect);
	CHECK_FALSE(f.socket.opened);
	f.run_for(120000);
	CHECK(f.socket.count(ip::Service::connect_request) == 1);
	f.tunnel.start();
	f.run_for(0);
	CHECK(f.socket.count(ip::Service::connect_request) == 2);
}

TEST_CASE("KnxTunnel.GatewayChangeReconnects") {
	TunnelFixture f;
	f.connect(7);
	f.set_gateway("192.0.2.2:3700");
	CHECK(hex(f.socket.last().data) == kDisconnect);
	f.run_for(0);
	CHECK(f.socket.sent[f.socket.sent.size() - 2].data == bytes(kDisconnect));
	CHECK(f.socket.last().to == (hc::Ipv4Endpoint{0xC0000202, 3700}));
	CHECK(f.tunnel.state() == TunnelState::connecting);
	f.tunnel.set_gateway(nullptr, false);
	CHECK(f.tunnel.state() == TunnelState::down);
	CHECK(std::string(f.tunnel.reason()) == "gateway cleared");
	f.run_for(60000);
	CHECK(f.socket.count(ip::Service::connect_request) == 2);
	CHECK(f.capture.sink.contains("I hc.knx: knx gateway cleared"));
}

TEST_CASE("KnxTunnel.ResolvesAHostName") {
	TunnelFixture f;
	f.set_gateway("knx-gateway.example");
	f.tunnel.start();
	f.run_for(0);
	REQUIRE(f.socket.resolves == std::vector<std::string>{"knx-gateway.example"});
	CHECK(f.tunnel.state() == TunnelState::connecting);
	CHECK(f.socket.sent.empty());
	f.socket.resolver->udp_resolved(false, 0, "host not found");
	CHECK(f.tunnel.state() == TunnelState::down);
	CHECK(std::string(f.tunnel.reason()) == "cannot resolve knx-gateway.example: host not found");
	f.run_for(1000);
	REQUIRE(f.socket.resolves.size() == 2);
	f.socket.resolver->udp_resolved(true, hc_test::kGatewayIp, "");
	REQUIRE(f.socket.sent.size() == 1);
	CHECK(f.socket.sent[0].to == hc_test::kGateway);
	CHECK(f.capture.sink.contains("I hc.knx: knx gateway knx-gateway.example is 192.0.2.1"));
	f.socket.inject(hc_test::connect_response(7));
	CHECK(f.tunnel.is_up());
	// A late answer of a resolution that is over changes nothing.
	f.socket.resolver->udp_resolved(true, 0xC0000203, "");
	CHECK(f.tunnel.is_up());
}

TEST_CASE("KnxTunnel.NatMode") {
	TunnelFixture f;
	f.set_gateway("192.0.2.1", true);
	f.tunnel.start();
	f.run_for(0);
	REQUIRE(f.socket.sent.size() == 1);
	CHECK(hex(f.socket.sent[0].data) == "06 10 02 05 00 1A 08 01 00 00 00 00 00 00 08 01 00 00 00 00 00 00 04 04 02 00");
	// A response whose data endpoint is 0.0.0.0:0: the data endpoint is where it came from.
	f.socket.inject(bytes("06 10 02 06 00 14 07 00 08 01 00 00 00 00 00 00 04 04 11 FA"),
		hc::Ipv4Endpoint{hc_test::kGatewayIp, 40000});
	REQUIRE(f.tunnel.is_up());
	CHECK(f.tunnel.data_endpoint() == (hc::Ipv4Endpoint{hc_test::kGatewayIp, 40000}));
	f.run_for(60000);
	CHECK(hex(f.socket.last().data) == "06 10 02 07 00 10 07 00 08 01 00 00 00 00 00 00");
}

TEST_CASE("KnxTunnel.SendRefusals") {
	TunnelFixture f;
	CHECK(f.tunnel.send(switch_on()) == 0);
	CHECK(f.capture.sink.contains("W hc.knx: refusing to send: the tunnel is down"));
	f.connect(7);
	for (size_t i = 0; i < KnxTunnel::kQueueCapacity; ++i) {
		CHECK(f.tunnel.send(switch_on()) != 0);
	}
	CHECK(f.tunnel.send(switch_on()) == 0);
	CHECK(f.capture.sink.contains("W hc.knx: refusing to send: the queue of 32 frames is full"));
	LData empty;
	f.socket.inject(hc_test::tunneling_ack(7, 0));
	f.socket.inject(hc_test::tunneling_request(7, 0, confirmation(1)));
	CHECK(f.tunnel.send(empty) == 0);
	CHECK(f.capture.sink.contains("W hc.knx: refusing to send a frame that does not encode: frame has no TPDU"));
}

TEST_CASE("KnxTunnel.SocketFailureRetries") {
	TunnelFixture f;
	f.socket.fail_open = true;
	f.set_gateway("192.0.2.1");
	f.tunnel.start();
	f.run_for(0);
	CHECK(f.tunnel.state() == TunnelState::down);
	CHECK(std::string(f.tunnel.reason()) == "cannot open a UDP socket: no sockets left");
	f.socket.fail_open = false;
	f.run_for(1000);
	CHECK(f.socket.count(ip::Service::connect_request) == 1);
}

TEST_CASE("KnxTunnel.FullPath") {
	// connect -> heartbeat -> tunnelling -> ack timeout -> repetition -> reconnect -> tunnelling again
	TunnelFixture f;
	f.connect(7);
	uint64_t up_at = f.clock.now_ms;
	f.run_until(up_at + 60000);
	CHECK(hex(f.socket.last().data) == kHeartbeat);
	f.socket.inject(hc_test::connectionstate_response(7));
	uint32_t first = f.tunnel.send(switch_on(1));
	f.socket.inject(hc_test::tunneling_ack(7, 0));
	f.socket.inject(hc_test::tunneling_request(7, 0, confirmation(1)));
	REQUIRE(f.recorder.sent.size() == 1);
	CHECK(f.recorder.sent[0].token == first);
	CHECK(f.recorder.sent[0].outcome == SendOutcome::confirmed);
	f.tunnel.send(switch_on(2));
	CHECK(hex(f.socket.last().data) == tunneling_request(1, 2));
	f.run_for(1000);
	CHECK(hex(f.socket.last().data) == tunneling_request(1, 2));
	f.run_for(1000);
	CHECK(hex(f.socket.last().data) == kDisconnect);
	CHECK(f.recorder.sent.back().outcome == SendOutcome::lost);
	f.run_for(1000);
	CHECK(hex(f.socket.last().data) == kConnectRequest);
	f.socket.inject(hc_test::connect_response(7));
	REQUIRE(f.tunnel.is_up());
	f.tunnel.send(switch_on(3));
	CHECK(hex(f.socket.last().data) == tunneling_request(0, 3)); // the counter starts over
	CHECK(f.recorder.states == std::vector<std::string>{"down->connecting", "connecting->up", "up->down",
		"down->connecting", "connecting->up"});
}

TEST_CASE("KnxTunnel.TimersAreReleased") {
	TunnelFixture f;
	size_t idle = f.loop.timer_count();
	f.connect(7);
	f.tunnel.send(switch_on());
	f.tunnel.stop();
	CHECK(f.loop.timer_count() == idle);
}
