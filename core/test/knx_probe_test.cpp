/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe tests written at review time for the KNX tunnel and adapter (issue #7): commands echoed to
// co-listening datapoints, reads answered only from good values, a queue lost with the tunnel,
// the duplicate-sequence rule seen from the datapoints, and a decoding refusal.

#include <optional>
#include <string>
#include <vector>

#include "doctest.h"
#include "hc/datapoint.h"
#include "hc/knx/adapter.h"
#include "hc/knx/telegram_log.h"
#include "knx_test_support.h"

using hc::DatapointFlags;
using hc::DatapointListener;
using hc::DatapointSpec;
using hc::DatapointStatus;
using hc::DatapointUpdate;
using hc::Dpt;
using hc::Quality;
using hc::Value;
using hc::ValueType;
using hc::knx::GroupAddress;
using hc::knx::KnxAdapter;
using hc::knx::KnxMapping;
using hc::knx::SendOutcome;
using hc::knx::TelegramLog;
using hc_test::Bytes;
using hc_test::bytes;
using hc_test::hex;
namespace ip = hc::knx::ip;

namespace {

constexpr uint8_t kRWT = DatapointFlags::kWritable | DatapointFlags::kTransmit | DatapointFlags::kReadable;

struct Counting : DatapointListener {
	void datapoint_updated(const DatapointUpdate& u) override {
		char buffer[64];
		updates.push_back(std::string(u.datapoint.name()) + "=" + u.datapoint.value().format(buffer, sizeof(buffer)));
	}
	std::vector<std::string> updates;
};

/// Ids: 0 light (1.001, sends 1/0/1), 1 other.light (1.001, listens 1/0/1), 2 dimmer (5.001, sends
/// 1/1/1, readable), 3 temperature (9.001, listens 1/2/3, read on start).
struct Probe : hc_test::TunnelFixture {
	hc::DatapointModel model{clock};
	TelegramLog log{clock};
	KnxAdapter adapter{loop, clock, model, tunnel, &log};
	Counting listener;
	GroupAddress other_listen[1] = {GroupAddress::of(1, 0, 1)};
	GroupAddress temperature_listen[1] = {GroupAddress::of(1, 2, 3)};
	uint8_t gateway_sequence = 0;

	Probe() {
		DatapointSpec specs[] = {
			{"light", ValueType::boolean, kRWT},
			{"other.light", ValueType::boolean, 0},
			{"dimmer", ValueType::real, kRWT},
			{"temperature", ValueType::real, DatapointFlags::kReadOnStart},
		};
		REQUIRE(model.load(specs, 4) == DatapointStatus::ok);
		REQUIRE(model.add_listener(listener) == DatapointStatus::ok);
		adapter.start();
		KnxMapping m[4];
		m[0] = mapping(0, Dpt{1, 1}, GroupAddress::of(1, 0, 1), nullptr, 0);
		m[1] = mapping(1, Dpt{1, 1}, std::nullopt, other_listen, 1);
		m[2] = mapping(2, Dpt{5, 1}, GroupAddress::of(1, 1, 1), nullptr, 0);
		m[3] = mapping(3, Dpt{9, 1}, std::nullopt, temperature_listen, 1);
		const char* error = nullptr;
		bool ok = adapter.configure(m, 4, &error);
		if (!ok) {
			MESSAGE("configure refused: ", error);
		}
		REQUIRE(ok);
	}
	/// Lets the read-on-start read of the temperature go out and confirms it.
	void settle_reads() {
		run_for(200);
		if (socket.count(ip::Service::tunneling_request) > 0 && socket.last().data[3] == 0x20) {
			confirm_last();
		}
		run_for(0);
	}
	static KnxMapping mapping(uint16_t id, Dpt dpt, std::optional<GroupAddress> send, const GroupAddress* listen,
		size_t count) {
		KnxMapping m;
		m.datapoint = id;
		m.dpt = dpt;
		m.has_send = send.has_value();
		if (send) {
			m.send = *send;
		}
		m.listen = listen;
		m.listen_count = count;
		return m;
	}
	void receive(uint16_t destination, const char* tpdu, uint8_t channel = 7) {
		Bytes cemi = bytes("29 00 BC D0 11 0A");
		cemi.push_back(static_cast<uint8_t>(destination >> 8));
		cemi.push_back(static_cast<uint8_t>(destination));
		Bytes t = bytes(tpdu);
		cemi.push_back(static_cast<uint8_t>(t.size() - 1));
		cemi.insert(cemi.end(), t.begin(), t.end());
		socket.inject(hc_test::tunneling_request(channel, gateway_sequence++, cemi));
	}
	void confirm_last(uint8_t channel = 7) {
		const Bytes& request = socket.last().data;
		REQUIRE(request[3] == 0x20);
		socket.inject(hc_test::tunneling_ack(channel, request[8]));
		Bytes cemi(request.begin() + 10, request.end());
		cemi[0] = 0x2E;
		cemi[4] = 0x11;
		cemi[5] = 0xFA;
		socket.inject(hc_test::tunneling_request(channel, gateway_sequence++, cemi));
	}
	std::string last_telegram() const {
		const Bytes& request = socket.last().data;
		return hex(Bytes(request.begin() + 16, request.end()));
	}
	const hc::Datapoint& dp(uint16_t id) const { return *model.get(id); }
};

} // namespace

TEST_CASE("KnxProbe.AConfirmedCommandReachesTheCoListeningDatapoint") {
	Probe p;
	p.connect(7);
	p.settle_reads();
	REQUIRE(p.model.command(0, Value::of_bool(true)) == DatapointStatus::ok);
	p.run_for(0);
	REQUIRE(p.last_telegram() == "08 01 01 00 81"); // 1/0/1, GroupValueWrite, value 1 embedded
	p.confirm_last();
	CHECK(p.dp(0).value() == Value::of_bool(true));
	CHECK(p.dp(1).value() == Value::of_bool(true)); // other.light listens on 1/0/1
	CHECK(p.dp(1).quality() == Quality::good);
	CHECK(p.listener.updates.size() >= 2);
}

TEST_CASE("KnxProbe.ReadsAreAnsweredOnlyFromGoodValues") {
	Probe p;
	p.connect(7);
	p.settle_reads();
	size_t before = p.socket.count(ip::Service::tunneling_request);
	p.receive(0x0901, "00 00"); // GroupValueRead on 1/1/1 (the dimmer's sending address)
	p.run_for(0);
	CHECK(p.socket.count(ip::Service::tunneling_request) == before); // unknown value: no response
	CHECK(p.capture.sink.count("hc.knx") > 0);
	// Now the dimmer has a value (commanded), and the read is answered with it.
	REQUIRE(p.model.command(2, Value::of_real(50.0)) == DatapointStatus::ok);
	p.run_for(0);
	CHECK(p.last_telegram() == "09 01 02 00 80 80"); // GroupValueWrite 50 % = 0x80 (rounded 127.5 -> 128)
	p.confirm_last();
	before = p.socket.count(ip::Service::tunneling_request);
	p.receive(0x0901, "00 00");
	p.run_for(0);
	REQUIRE(p.socket.count(ip::Service::tunneling_request) == before + 1);
	CHECK(p.last_telegram() == "09 01 02 00 40 80"); // GroupValueResponse with 0x80
	p.confirm_last();
}

TEST_CASE("KnxProbe.QueuedCommandsAreLostWithTheTunnelAndNotReplayed") {
	Probe p;
	p.connect(7);
	p.settle_reads();
	size_t sent_before = p.socket.count(ip::Service::tunneling_request);
	REQUIRE(p.model.command(0, Value::of_bool(true)) == DatapointStatus::ok);
	REQUIRE(p.model.command(2, Value::of_real(10.0)) == DatapointStatus::ok);
	REQUIRE(p.model.command(0, Value::of_bool(false)) == DatapointStatus::ok);
	p.run_for(0);
	CHECK(p.socket.count(ip::Service::tunneling_request) == sent_before + 1); // one in flight, two queued
	CHECK(p.tunnel.queued() == 3); // the queue counts the frame in flight as well
	// The gateway drops the connection before anything is acknowledged.
	p.socket.inject(hc_test::disconnect_request(7));
	CHECK_FALSE(p.tunnel.is_up());
	CHECK(p.tunnel.queued() == 0);
	size_t lost = 0;
	for (const auto& s : p.recorder.sent) {
		lost += s.outcome == SendOutcome::lost ? 1 : 0;
	}
	CHECK(lost == 3);
	CHECK(p.dp(0).quality() == Quality::stale);
	// Reconnected: only the read-on-start read goes out, none of the lost writes.
	p.run_for(1000);
	p.socket.inject(hc_test::connect_response(8));
	p.gateway_sequence = 0;
	REQUIRE(p.tunnel.is_up());
	p.run_for(200);
	size_t after = p.socket.count(ip::Service::tunneling_request);
	CHECK(after == sent_before + 1 + 1); // the one that was in flight (lost) plus exactly one read
	CHECK(p.last_telegram() == "0A 03 01 00 00"); // GroupValueRead 1/2/3, none of the lost writes
}

TEST_CASE("KnxProbe.ARepeatedGatewayTelegramUpdatesTheDatapointOnce") {
	Probe p;
	p.connect(7);
	p.settle_reads();
	size_t before = p.listener.updates.size();
	p.receive(0x0A03, "00 80 0C 1A"); // temperature 21.0 on 1/2/3, sequence n
	REQUIRE(p.listener.updates.size() == before + 1);
	// The gateway repeats the same sequence: acknowledged again, not delivered again.
	p.gateway_sequence--;
	size_t acks = p.socket.count(ip::Service::tunneling_ack);
	p.receive(0x0A03, "00 80 0C 1A");
	CHECK(p.socket.count(ip::Service::tunneling_ack) == acks + 1);
	CHECK(p.listener.updates.size() == before + 1);
	// A wrong length for DPT 9 is refused: value and quality untouched, a log line names it.
	size_t errors = p.capture.sink.count("W hc.knx") + p.capture.sink.count("E hc.knx");
	p.receive(0x0A03, "00 80 0C 1A 00");
	CHECK(p.dp(3).value().as_real() == doctest::Approx(21.0));
	CHECK(p.capture.sink.count("W hc.knx") + p.capture.sink.count("E hc.knx") > errors);
	CHECK(p.log.size() >= 1);
}
