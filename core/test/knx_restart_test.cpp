/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// homecontrol #103: the qualities of the KNX datapoints across a restart and a lost tunnel, and the
// planned restart that closes the tunnel first (hc::Shutdown).

#include <string>
#include <vector>

#include "doctest.h"
#include "hc/datapoint.h"
#include "hc/knx/adapter.h"
#include "hc/shutdown.h"
#include "knx_test_support.h"

using hc::DatapointFlags;
using hc::DatapointSpec;
using hc::DatapointStatus;
using hc::Dpt;
using hc::Quality;
using hc::Shutdown;
using hc::ShutdownParticipant;
using hc::Value;
using hc::ValueType;
using hc::knx::GroupAddress;
using hc::knx::KnxAdapter;
using hc::knx::KnxMapping;
using hc::knx::KnxTunnel;
using hc::knx::TunnelState;
using hc_test::Bytes;
using hc_test::bytes;
namespace ip = hc::knx::ip;

namespace {

constexpr uint8_t kWritten = DatapointFlags::kWritable | DatapointFlags::kTransmit | DatapointFlags::kPersist;
constexpr uint8_t kRefused = 0x24; // E_NO_MORE_CONNECTIONS

struct Updates : hc::DatapointListener {
	void datapoint_updated(const hc::DatapointUpdate& update) override {
		names.push_back(update.datapoint.name());
	}
	void datapoint_quality(const hc::Datapoint& datapoint) override {
		qualities.push_back(std::string(datapoint.name()) + " " + hc::quality_name(datapoint.quality()));
	}
	std::vector<std::string> names;
	std::vector<std::string> qualities; ///< What a display is told (datapoint_quality()).
};

/// The house controller's case: 0 day_night (1.001, persisted, written only by the controller,
/// sends 3/1/1, no read on start: schlafzimmer.tag_nacht_update), 1 temperature (9.001, persisted,
/// read on start, listens 3/2/1), 2 status (1.001, listens 3/3/1, neither persisted nor read).
struct RestartFixture : hc_test::TunnelFixture {
	hc::DatapointModel model{clock};
	KnxAdapter adapter{loop, clock, model, tunnel};
	Updates updates;
	GroupAddress temperature_listen[1] = {GroupAddress::of(3, 2, 1)};
	GroupAddress status_listen[1] = {GroupAddress::of(3, 3, 1)};
	uint8_t gateway_sequence = 0;

	RestartFixture() {
		DatapointSpec specs[] = {
			{"day_night", ValueType::boolean, kWritten},
			{"temperature", ValueType::real, DatapointFlags::kPersist | DatapointFlags::kReadOnStart},
			{"status", ValueType::boolean, 0},
		};
		REQUIRE(model.load(specs, 3) == DatapointStatus::ok);
		// What hc::ValueStore does right after the load (homecontrol #100).
		REQUIRE(model.restore(0, Value::of_bool(true)) == DatapointStatus::ok);
		REQUIRE(model.restore(1, Value::of_real(19.5)) == DatapointStatus::ok);
		adapter.start();
		KnxMapping mappings[3];
		mappings[0] = mapping(0, Dpt{1, 1}, GroupAddress::of(3, 1, 1), nullptr, 0);
		mappings[1] = mapping(1, Dpt{9, 1}, std::nullopt, temperature_listen, 1);
		mappings[2] = mapping(2, Dpt{1, 1}, std::nullopt, status_listen, 1);
		REQUIRE(adapter.configure(mappings, 3));
		REQUIRE(model.add_listener(updates) == DatapointStatus::ok);
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

	/// The gateway refuses every attempt for `ms` (its slot is still taken by the previous run).
	void refuse_for(uint64_t ms) {
		uint64_t until = clock.now_ms + ms;
		size_t answered = 0;
		while (clock.now_ms < until) {
			size_t requests = socket.count(ip::Service::connect_request);
			if (requests > answered && socket.opened) {
				socket.inject(hc_test::connect_response(0, kRefused));
				answered = requests;
			}
			run_for(100);
		}
	}

	/// An L_Data.ind from 1.1.10 with the TPDU `tpdu` to `destination`.
	void receive(uint16_t destination, const char* tpdu, uint8_t channel) {
		Bytes cemi = bytes("29 00 BC D0 11 0A");
		cemi.push_back(static_cast<uint8_t>(destination >> 8));
		cemi.push_back(static_cast<uint8_t>(destination));
		Bytes t = bytes(tpdu);
		cemi.push_back(static_cast<uint8_t>(t.size() - 1));
		cemi.insert(cemi.end(), t.begin(), t.end());
		socket.inject(hc_test::tunneling_request(channel, gateway_sequence++, cemi));
	}

	/// Acknowledges and confirms the last TUNNELING_REQUEST on `channel`.
	void confirm_last(uint8_t channel) {
		const Bytes& request = socket.last().data;
		REQUIRE(request[3] == 0x20);
		socket.inject(hc_test::tunneling_ack(channel, request[8]));
		Bytes cemi(request.begin() + 10, request.end());
		cemi[0] = 0x2E;
		cemi[4] = 0x11;
		cemi[5] = 0xFA;
		socket.inject(hc_test::tunneling_request(channel, gateway_sequence++, cemi));
	}

	const hc::Datapoint& dp(uint16_t id) const { return *model.get(id); }
};

} // namespace

TEST_CASE("KnxRestart.RestoredValuesStayGoodThroughFailedFirstAttempts") {
	RestartFixture f;
	f.set_gateway("192.0.2.1");
	f.tunnel.start();
	f.run_for(0);
	// The boot log of the house controller: E_NO_MORE_CONNECTIONS for the first attempts.
	f.refuse_for(30000);
	CHECK(f.tunnel.attempts() >= 5);
	CHECK(f.tunnel.connections() == 0);
	CHECK(f.dp(0).quality() == Quality::good);
	CHECK(f.dp(0).value().as_bool() == true);
	CHECK(f.dp(1).quality() == Quality::good);
	CHECK(f.dp(2).quality() == Quality::unknown);
	CHECK(f.updates.names.empty()); // no stale report, nothing for the rules
	CHECK(f.updates.qualities.empty());
	// Up at last: the controller-written datapoint keeps its restored value as good, the one read on
	// start is read and good on the answer, which replaces the restored value.
	f.run_until(f.tunnel.next_attempt_at());
	f.socket.inject(hc_test::connect_response(7));
	REQUIRE(f.tunnel.is_up());
	CHECK(f.dp(0).quality() == Quality::good);
	CHECK(f.dp(0).value().as_bool() == true);
	CHECK(f.dp(1).quality() == Quality::good);
	CHECK(f.adapter.reads_pending() == 1);
	f.run_for(KnxAdapter::kReadSpacingMs);
	CHECK(hc_test::hex(Bytes(f.socket.last().data.begin() + 16, f.socket.last().data.end())) == "1A 01 01 00 00");
	f.confirm_last(7);
	f.receive(0x1A01, "00 40 0C 1A", 7); // a group response: 21.0 °C
	CHECK(f.dp(1).quality() == Quality::good);
	CHECK(f.dp(1).value().as_real() == doctest::Approx(21.0));
}

TEST_CASE("KnxRestart.AfterALossTheLastValueIsGoodAgain") {
	RestartFixture f;
	f.connect(7);
	f.run_for(KnxAdapter::kReadSpacingMs);
	f.confirm_last(7); // the read of the temperature
	f.receive(0x1A01, "00 40 0C 1A", 7);
	f.receive(0x1B01, "00 81", 7); // status on
	// The controller writes day_night.
	REQUIRE(f.model.command(0, Value::of_bool(false)) == DatapointStatus::ok);
	f.confirm_last(7);
	CHECK(f.dp(0).quality() == Quality::good);
	uint64_t written_at = f.dp(0).updated_ms();
	// Contact lost: every datapoint is stale, the rules see no value meanwhile.
	f.socket.inject(hc_test::disconnect_request(7));
	CHECK(f.dp(0).quality() == Quality::stale);
	CHECK(f.dp(1).quality() == Quality::stale);
	CHECK(f.dp(2).quality() == Quality::stale);
	// Up again, after a refused attempt: day_night has nothing on the bus to ask and is good again with
	// the controller's own write, silently (no listener is told, updated_ms() stays). The temperature
	// is read and good on the answer. The status (no read on start, a datapoint the bus writes) is
	// good again too: the last value counts.
	f.updates.names.clear();
	f.updates.qualities.clear();
	f.run_until(f.tunnel.next_attempt_at());
	f.socket.inject(hc_test::connect_response(0, kRefused));
	CHECK(f.dp(0).quality() == Quality::stale); // still down: still no contact
	f.run_until(f.tunnel.next_attempt_at());
	f.socket.inject(hc_test::connect_response(8));
	f.gateway_sequence = 0;
	REQUIRE(f.tunnel.is_up());
	CHECK(f.dp(0).quality() == Quality::good);
	CHECK(f.dp(0).value().as_bool() == false);
	CHECK(f.dp(0).updated_ms() == written_at);
	CHECK(f.dp(2).quality() == Quality::good);
	CHECK(f.dp(2).value().as_bool() == true);
	CHECK(f.dp(1).quality() == Quality::stale);
	CHECK(f.updates.names.empty()); // no update: no rule trigger, no change event, no history entry
	// The displays (live feed, MQTT) are told the new quality.
	CHECK(f.updates.qualities == std::vector<std::string>{"day_night good", "status good"});
	f.run_for(KnxAdapter::kReadSpacingMs);
	f.confirm_last(8);
	f.receive(0x1A01, "00 40 0C 1C", 8); // 21.04 °C... a new answer
	CHECK(f.dp(1).quality() == Quality::good);
	CHECK(f.updates.names == std::vector<std::string>{"temperature"});
}

TEST_CASE("KnxRestart.CommandWhileDownIsHeldAndSentNotResumed") {
	RestartFixture f;
	f.connect(7);
	f.run_for(KnxAdapter::kReadSpacingMs);
	f.confirm_last(7);
	f.socket.inject(hc_test::disconnect_request(7));
	REQUIRE(f.dp(0).quality() == Quality::stale);
	// Commanded while down: the model takes the value (good), the adapter holds it.
	REQUIRE(f.model.command(0, Value::of_bool(false)) == DatapointStatus::ok);
	CHECK(f.dp(0).quality() == Quality::good);
	CHECK(f.dp(0).pending());
	uint64_t commanded_at = f.dp(0).updated_ms();
	f.run_until(f.tunnel.next_attempt_at());
	f.socket.inject(hc_test::connect_response(8));
	REQUIRE(f.tunnel.is_up());
	CHECK(f.dp(0).quality() == Quality::good);
	CHECK(f.dp(0).value().as_bool() == false);
	CHECK(f.dp(0).updated_ms() == commanded_at);
	CHECK_FALSE(f.dp(0).pending()); // sent
	CHECK(f.socket.count(ip::Service::tunneling_request) >= 2);
}

TEST_CASE("KnxRestart.ResumeIsTheOwnersAndOnlyForStale") {
	RestartFixture f;
	struct Other : hc::Technology {
		const char* name() const override { return "other"; }
		bool command(const hc::Datapoint&, const Value&) override { return false; }
	} other;
	CHECK(f.model.resume(other, 0) == DatapointStatus::not_owner);
	CHECK(f.model.resume(f.adapter, 2) == DatapointStatus::ok);
	CHECK(f.dp(2).quality() == Quality::unknown); // unknown stays unknown
	CHECK(f.model.resume(f.adapter, 0) == DatapointStatus::ok);
	CHECK(f.dp(0).quality() == Quality::good);
	CHECK(f.model.resume(f.adapter, 99) == DatapointStatus::unknown_id);
}

// ---- The planned restart ---------------------------------------------------------------------

namespace {

struct Restarted {
	bool done = false;
	size_t datagrams_before = 0;
	const hc_test::FakeUdpSocket* socket = nullptr;
	void restart() {
		done = true;
		datagrams_before = socket->sent.size();
	}
};

struct ShutdownFixture : RestartFixture {
	Shutdown shutdown{loop, clock};
	Restarted restarted;

	ShutdownFixture() {
		restarted.socket = &socket;
		REQUIRE(shutdown.add(tunnel));
	}

	bool begin() {
		return shutdown.begin("restart over the API", hc::Callback::member<Restarted, &Restarted::restart>(&restarted));
	}
};

} // namespace

TEST_CASE("KnxRestart.PlannedRestartSendsDisconnectFirst") {
	ShutdownFixture f;
	f.connect(7);
	uint64_t begun = f.clock.now_ms;
	REQUIRE(f.begin());
	CHECK(f.socket.service(f.socket.sent.size() - 1) == static_cast<uint16_t>(ip::Service::disconnect_request));
	CHECK(f.socket.last().data[6] == 7); // the tunnel's channel
	CHECK(f.tunnel.state() == TunnelState::down);
	CHECK_FALSE(f.restarted.done); // waits for the gateway's answer
	CHECK(f.shutdown.running());
	f.run_for(20);
	f.socket.inject(hc_test::disconnect_response(7));
	CHECK(f.restarted.done);
	CHECK(f.clock.now_ms - begun == 20);
	CHECK_FALSE(f.socket.opened);
	CHECK(f.shutdown.finished());
	// No reconnect afterwards (the restart follows); a second restart is refused.
	f.run_for(120000);
	CHECK(f.socket.count(ip::Service::connect_request) == 1);
	CHECK_FALSE(f.begin());
}

TEST_CASE("KnxRestart.PlannedRestartWaitsAtMostTheTimeout") {
	ShutdownFixture f;
	f.connect(7);
	uint64_t begun = f.clock.now_ms;
	REQUIRE(f.begin());
	f.run_until(begun + KnxTunnel::kDisconnectTimeoutMs - 1);
	CHECK_FALSE(f.restarted.done);
	f.run_until(begun + KnxTunnel::kDisconnectTimeoutMs);
	CHECK(f.restarted.done);
	CHECK(f.socket.count(ip::Service::disconnect_request) == 1);
	CHECK(f.capture.sink.contains("no DISCONNECT_RESPONSE within 500 ms"));
}

TEST_CASE("KnxRestart.PlannedRestartClosesTheConnectionOfAnAttemptUnderWay") {
	ShutdownFixture f;
	f.set_gateway("192.0.2.1");
	f.tunnel.start();
	f.run_for(0);
	REQUIRE(f.socket.count(ip::Service::connect_request) == 1);
	REQUIRE(f.begin());
	CHECK(f.socket.count(ip::Service::disconnect_request) == 0);
	CHECK_FALSE(f.restarted.done);
	// The gateway accepts the attempt after the restart began: its channel is closed at once.
	f.socket.inject(hc_test::connect_response(9));
	CHECK_FALSE(f.tunnel.is_up());
	REQUIRE(f.socket.count(ip::Service::disconnect_request) == 1);
	CHECK(f.socket.last().data[6] == 9);
	CHECK_FALSE(f.restarted.done);
	f.socket.inject(hc_test::disconnect_response(9));
	CHECK(f.restarted.done);
}

TEST_CASE("KnxRestart.PlannedRestartWithoutATunnelRestartsAtOnce") {
	ShutdownFixture f;
	f.set_gateway("192.0.2.1");
	f.tunnel.start();
	f.run_for(0);
	f.socket.inject(hc_test::connect_response(0, kRefused)); // down, a retry scheduled
	REQUIRE(f.tunnel.next_attempt_at() != hc::EventLoop::kNever);
	REQUIRE(f.begin());
	CHECK(f.restarted.done);
	CHECK(f.socket.count(ip::Service::disconnect_request) == 0);
	CHECK(f.tunnel.next_attempt_at() == hc::EventLoop::kNever);
	f.run_for(120000);
	CHECK(f.socket.count(ip::Service::connect_request) == 1);
}

TEST_CASE("KnxRestart.TheGatewaysDisconnectWhileClosingEndsIt") {
	ShutdownFixture f;
	f.connect(7);
	REQUIRE(f.begin());
	f.socket.inject(hc_test::disconnect_request(7));
	CHECK(f.restarted.done);
	CHECK(f.socket.service(f.socket.sent.size() - 1) == static_cast<uint16_t>(ip::Service::disconnect_response));
}

TEST_CASE("KnxRestart.NetworkDownWhileClosingEndsIt") {
	ShutdownFixture f;
	f.connect(7);
	REQUIRE(f.begin());
	f.connectivity.link_down("beacon timeout");
	f.run_for(0);
	CHECK(f.restarted.done);
}

namespace {

struct Silent : ShutdownParticipant {
	const char* shutdown_name() const override { return "silent"; }
	void shutdown_begin(Shutdown&) override { ++begun; }
	int begun = 0;
};

struct Immediate : ShutdownParticipant {
	const char* shutdown_name() const override { return "immediate"; }
	void shutdown_begin(Shutdown& shutdown) override { shutdown.done(*this); }
};

} // namespace

TEST_CASE("Shutdown.WaitsForEveryParticipantAtMostTheLimit") {
	hc_test::FakeClock clock;
	hc::EventLoop loop;
	Shutdown shutdown{loop, clock};
	Silent silent;
	Immediate immediate;
	REQUIRE(shutdown.add(silent));
	REQUIRE(shutdown.add(immediate));
	CHECK_FALSE(shutdown.add(silent));
	hc_test::FakeUdpSocket socket{clock};
	Restarted restarted;
	restarted.socket = &socket;
	REQUIRE(shutdown.begin("test", hc::Callback::member<Restarted, &Restarted::restart>(&restarted)));
	CHECK(silent.begun == 1);
	uint64_t begun = clock.now_ms;
	loop.run_once(clock.now_ms = begun + Shutdown::kMaxWaitMs - 1);
	CHECK_FALSE(restarted.done);
	loop.run_once(clock.now_ms = begun + Shutdown::kMaxWaitMs);
	CHECK(restarted.done);
	shutdown.done(silent); // too late: ignored
	CHECK(shutdown.finished());
}

TEST_CASE("Shutdown.WithoutParticipantsRunsAtOnce") {
	hc_test::FakeClock clock;
	hc::EventLoop loop;
	Shutdown shutdown{loop, clock};
	hc_test::FakeUdpSocket socket{clock};
	Restarted restarted;
	restarted.socket = &socket;
	REQUIRE(shutdown.begin("test", hc::Callback::member<Restarted, &Restarted::restart>(&restarted)));
	CHECK(restarted.done);
}
