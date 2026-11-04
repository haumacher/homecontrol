/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The KNX adapter between the tunnel and the datapoint model, under the fake clock.

#include <string>
#include <vector>

#include "doctest.h"
#include "hc/datapoint.h"
#include "hc/knx/adapter.h"
#include "hc/knx/telegram_log.h"
#include "knx_test_support.h"

using hc::DatapointFlags;
using hc::DatapointSpec;
using hc::DatapointStatus;
using hc::Dpt;
using hc::Quality;
using hc::Value;
using hc::ValueType;
using hc::knx::GroupAddress;
using hc::knx::KnxAdapter;
using hc::knx::KnxMapping;
using hc::knx::TelegramLog;
using hc::knx::TelegramRecord;
using hc_test::Bytes;
using hc_test::bytes;
using hc_test::hex;
namespace ip = hc::knx::ip;

namespace {

constexpr uint8_t kReadWrite = DatapointFlags::kWritable | DatapointFlags::kTransmit | DatapointFlags::kReadable;

/// Ids: 0 light (1.001, sends 1/0/1, listens 1/0/2 for the status), 1 temperature (9.001, listens
/// 1/2/3, read on start), 2 dimmer (5.001, sends 1/1/1, readable, read on start), 3 other light
/// (1.001, listens 1/0/1), 4 setpoint (9.001, read on start via a listening address).
struct AdapterFixture : hc_test::TunnelFixture {
	hc::DatapointModel model{clock};
	TelegramLog log{clock};
	KnxAdapter adapter{loop, clock, model, tunnel, &log};
	GroupAddress light_status[1] = {GroupAddress::of(1, 0, 2)};
	GroupAddress temperature_listen[1] = {GroupAddress::of(1, 2, 3)};
	GroupAddress other_light_listen[1] = {GroupAddress::of(1, 0, 1)};
	GroupAddress setpoint_listen[2] = {GroupAddress::of(2, 0, 1), GroupAddress::of(2, 0, 2)};

	AdapterFixture() {
		DatapointSpec specs[] = {
			{"light", ValueType::boolean, kReadWrite},
			{"temperature", ValueType::real, DatapointFlags::kReadOnStart},
			{"dimmer", ValueType::real, kReadWrite | DatapointFlags::kReadOnStart},
			{"other.light", ValueType::boolean, 0},
			{"setpoint", ValueType::real, DatapointFlags::kReadOnStart},
		};
		REQUIRE(model.load(specs, 5) == DatapointStatus::ok);
		adapter.start();
		KnxMapping mappings[5];
		mappings[0] = mapping(0, Dpt{1, 1}, GroupAddress::of(1, 0, 1), light_status, 1);
		mappings[1] = mapping(1, Dpt{9, 1}, std::nullopt, temperature_listen, 1);
		mappings[2] = mapping(2, Dpt{5, 1}, GroupAddress::of(1, 1, 1), nullptr, 0);
		mappings[3] = mapping(3, Dpt{1, 1}, std::nullopt, other_light_listen, 1);
		mappings[4] = mapping(4, Dpt{9, 1}, std::nullopt, setpoint_listen, 2);
		REQUIRE(adapter.configure(mappings, 5));
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

	/// An L_Data.ind from 1.1.10 with the TPDU `tpdu` to `destination`.
	void receive(uint16_t destination, const char* tpdu) {
		Bytes cemi = bytes("29 00 BC D0 11 0A");
		cemi.push_back(static_cast<uint8_t>(destination >> 8));
		cemi.push_back(static_cast<uint8_t>(destination));
		Bytes t = bytes(tpdu);
		cemi.push_back(static_cast<uint8_t>(t.size() - 1));
		cemi.insert(cemi.end(), t.begin(), t.end());
		socket.inject(hc_test::tunneling_request(7, gateway_sequence++, cemi));
	}

	/// Acknowledges the last TUNNELING_REQUEST and confirms it.
	void confirm_last() {
		const Bytes& request = socket.last().data;
		REQUIRE(request[3] == 0x20);
		socket.inject(hc_test::tunneling_ack(7, request[8]));
		Bytes cemi(request.begin() + 10, request.end());
		cemi[0] = 0x2E;
		cemi[4] = 0x11; // source 1.1.250
		cemi[5] = 0xFA;
		socket.inject(hc_test::tunneling_request(7, gateway_sequence++, cemi));
	}

	/// The cEMI TPDU part "dest len tpdu" of the last TUNNELING_REQUEST, as hex from the destination.
	std::string last_telegram() const {
		const Bytes& request = socket.last().data;
		return hex(Bytes(request.begin() + 16, request.end()));
	}

	const hc::Datapoint& dp(uint16_t id) const { return *model.get(id); }

	uint8_t gateway_sequence = 0;
};

} // namespace

TEST_CASE("KnxAdapter.ConfigureRefusals") {
	AdapterFixture f;
	GroupAddress nine[9] = {};
	KnxMapping m = AdapterFixture::mapping(9, Dpt{1, 1}, GroupAddress{1}, nullptr, 0);
	const char* error = nullptr;
	CHECK_FALSE(f.adapter.configure(&m, 1, &error));
	CHECK(f.capture.sink.contains("E hc.knx: refusing the KNX mapping: mapping 0: no datapoint with this id"));
	m = AdapterFixture::mapping(0, Dpt{999, 1}, GroupAddress{1}, nullptr, 0);
	CHECK_FALSE(f.adapter.configure(&m, 1, &error));
	m = AdapterFixture::mapping(0, Dpt{9, 1}, GroupAddress{1}, nullptr, 0);
	CHECK_FALSE(f.adapter.configure(&m, 1, &error));
	CHECK(f.capture.sink.contains("the DPT's value type does not fit the datapoint's"));
	m = AdapterFixture::mapping(0, Dpt{1, 1}, std::nullopt, nine, 9);
	CHECK_FALSE(f.adapter.configure(&m, 1, &error));
	m = AdapterFixture::mapping(0, Dpt{1, 1}, std::nullopt, nine, 1);
	CHECK_FALSE(f.adapter.configure(&m, 1, &error));
	CHECK(f.capture.sink.contains("a transmitting or readable datapoint needs a sending address"));
	m = AdapterFixture::mapping(3, Dpt{1, 1}, std::nullopt, nullptr, 0);
	CHECK_FALSE(f.adapter.configure(&m, 1, &error));
	CHECK(f.capture.sink.contains("mapping 0: no group address"));
	KnxMapping twice[2] = {AdapterFixture::mapping(3, Dpt{1, 1}, GroupAddress{1}, nullptr, 0),
		AdapterFixture::mapping(3, Dpt{1, 1}, GroupAddress{2}, nullptr, 0)};
	CHECK_FALSE(f.adapter.configure(twice, 2, &error));
	CHECK(f.capture.sink.contains("mapping 1: the datapoint is mapped twice"));
	// The previous mapping stays.
	CHECK(f.adapter.size() == 5);

	// A datapoint owned by another technology (a model reload unbinds everything).
	struct Other : hc::Technology {
		const char* name() const override { return "other"; }
		bool command(const hc::Datapoint&, const Value&) override { return true; }
	} other;
	DatapointSpec specs[] = {{"light", ValueType::boolean, 0}};
	REQUIRE(f.model.load(specs, 1) == DatapointStatus::ok);
	REQUIRE(f.model.bind(0, other) == DatapointStatus::ok);
	m = AdapterFixture::mapping(0, Dpt{1, 1}, GroupAddress{1}, nullptr, 0);
	CHECK_FALSE(f.adapter.configure(&m, 1, &error));
	CHECK(f.capture.sink.contains("mapping 0: the datapoint belongs to another technology"));
	f.adapter.clear();
	CHECK(f.adapter.size() == 0);
}

TEST_CASE("KnxAdapter.WritesAndResponsesReport") {
	AdapterFixture f;
	f.connect(7);
	f.receive(0x0A03, "00 80 0C 1A"); // 1/2/3 write 21.0
	CHECK(f.dp(1).quality() == Quality::good);
	CHECK(f.dp(1).value().as_real() == doctest::Approx(21.0));
	f.receive(0x0802, "00 41"); // 1/0/2 response on (the light's status address)
	CHECK(f.dp(0).value().as_bool());
	f.receive(0x1002, "00 40 0C 1A"); // 2/0/2: the setpoint's second listening address
	CHECK(f.dp(4).value().as_real() == doctest::Approx(21.0));
	f.receive(0x0801, "00 80"); // 1/0/1 write off: the light (sending address) and the other light
	CHECK_FALSE(f.dp(0).value().as_bool());
	CHECK(f.dp(3).quality() == Quality::good);
	CHECK_FALSE(f.dp(3).value().as_bool());
	f.receive(0x0F0F, "00 81"); // unmapped: logged only
	CHECK(f.log.size() == 5);
	CHECK(f.log.at(4)->datapoint == TelegramRecord::kNoDatapoint);
	CHECK(f.log.at(0)->datapoint == 1);
	CHECK(f.log.at(0)->direction == TelegramRecord::Direction::received);
	CHECK(f.log.at(0)->source.raw == 0x110A);
}

TEST_CASE("KnxAdapter.DecodingRefusalKeepsTheValue") {
	AdapterFixture f;
	f.connect(7);
	f.receive(0x0A03, "00 80 0C 1A");
	f.receive(0x0A03, "00 80 7F FF");
	CHECK(f.dp(1).value().as_real() == doctest::Approx(21.0));
	CHECK(f.capture.sink.contains(
		"W hc.knx: 1/2/3 from 1.1.10: 9.001 refuses 7F FF (invalid: 0x7FFF is the invalid value); 'temperature' unchanged"));
	f.receive(0x0A03, "00 81"); // a short telegram for a 2-octet type
	CHECK(f.capture.sink.contains("9.001 refuses 01 (short) (wrong_length"));
	f.receive(0x0801, "00 80 01"); // a 1-bit type in the long form
	CHECK(f.capture.sink.contains("1/0/1 from 1.1.10: 1.001 refuses 01 (wrong_length"));
	CHECK(f.dp(1).value().as_real() == doctest::Approx(21.0));
	CHECK(f.dp(0).quality() == Quality::unknown);
}

TEST_CASE("KnxAdapter.CommandSendsAGroupWrite") {
	AdapterFixture f;
	// While the tunnel is down a command is held (homecontrol #72, knx_hold_test.cpp); a value the DPT
	// cannot encode is refused all the same.
	CHECK(f.model.command(2, Value::of_real(150.0)) == DatapointStatus::refused);
	CHECK(f.capture.sink.contains("W hc.knx: refusing command for 'dimmer': 5.001 cannot encode it"));
	CHECK(f.dp(2).quality() == Quality::unknown);
	CHECK_FALSE(f.dp(2).pending());

	f.connect(7);
	f.run_for(1000); // let the reads go out
	while (f.adapter.reads_pending() > 0 || f.tunnel.queued() > 0) {
		f.confirm_last();
		f.run_for(100);
	}
	CHECK(f.model.command(0, Value::of_bool(true)) == DatapointStatus::ok);
	CHECK(f.last_telegram() == "08 01 01 00 81");
	CHECK(f.dp(0).value().as_bool());
	CHECK(f.dp(3).quality() == Quality::unknown); // not before the confirmation
	f.confirm_last();
	// The other datapoint on the address takes the value; the sender is not reported again.
	CHECK(f.dp(3).value().as_bool());
	CHECK(f.log.at(f.log.size() - 1)->direction == TelegramRecord::Direction::sent);
	CHECK(f.log.at(f.log.size() - 1)->source.raw == 0x11FA);

	CHECK(f.model.command(2, Value::of_real(50.0)) == DatapointStatus::ok);
	CHECK(f.last_telegram() == "09 01 02 00 80 80");
	CHECK(f.model.command(2, Value::of_real(150.0)) == DatapointStatus::refused);
	CHECK(f.capture.sink.contains("W hc.knx: refusing command for 'dimmer': 5.001 cannot encode it"));
}

TEST_CASE("KnxAdapter.AnswersReads") {
	AdapterFixture f;
	f.connect(7);
	f.run_for(1000);
	while (f.adapter.reads_pending() > 0 || f.tunnel.queued() > 0) {
		f.confirm_last();
		f.run_for(100);
	}
	size_t base = f.socket.count(ip::Service::tunneling_request);
	f.receive(0x0801, "00 00"); // read of the light: unknown, not answered
	CHECK(f.socket.count(ip::Service::tunneling_request) == base);
	CHECK(f.capture.sink.contains("I hc.knx: not answering the read of 1/0/1: 'light' has no good value (unknown)"));
	f.receive(0x0801, "00 81");
	f.receive(0x0801, "00 00");
	REQUIRE(f.socket.count(ip::Service::tunneling_request) == base + 1);
	CHECK(f.last_telegram() == "08 01 01 00 41"); // response on
	f.confirm_last();
	f.receive(0x0802, "00 00"); // the status address is no sending address: not answered
	f.receive(0x0A03, "00 00"); // the temperature is not readable
	CHECK(f.socket.count(ip::Service::tunneling_request) == base + 1);
}

TEST_CASE("KnxAdapter.ReadOnStartIsSpread") {
	AdapterFixture f;
	f.connect(7);
	uint64_t up_at = f.clock.now_ms;
	CHECK(f.adapter.reads_pending() == 3);
	f.run_until(up_at + 99);
	CHECK(f.socket.count(ip::Service::tunneling_request) == 0);
	f.run_until(up_at + 100);
	REQUIRE(f.socket.count(ip::Service::tunneling_request) == 1);
	CHECK(f.last_telegram() == "0A 03 01 00 00"); // temperature: its listening address
	f.run_until(up_at + 200);
	CHECK(f.tunnel.queued() == 2); // the dimmer's read waits for the first confirmation
	f.confirm_last();
	CHECK(f.last_telegram() == "09 01 01 00 00"); // dimmer: its sending address
	f.run_until(up_at + 300);
	CHECK(f.adapter.reads_pending() == 0);
	f.confirm_last();
	CHECK(f.last_telegram() == "10 01 01 00 00"); // setpoint: its first listening address
	f.confirm_last();
	f.run_for(10000);
	CHECK(f.socket.count(ip::Service::tunneling_request) == 3);
	// The answers report.
	f.receive(0x0A03, "00 40 0C 1A");
	CHECK(f.dp(1).value().as_real() == doctest::Approx(21.0));
}

TEST_CASE("KnxAdapter.StaleWhenTheTunnelGoesDown") {
	AdapterFixture f;
	f.connect(7);
	f.receive(0x0A03, "00 80 0C 1A");
	f.receive(0x0802, "00 41");
	CHECK(f.dp(1).quality() == Quality::good);
	f.socket.inject(hc_test::disconnect_request(7));
	CHECK(f.dp(1).quality() == Quality::stale);
	CHECK(f.dp(0).quality() == Quality::stale);
	CHECK(f.dp(1).value().as_real() == doctest::Approx(21.0));
	CHECK(f.dp(2).quality() == Quality::unknown);
	CHECK(f.adapter.reads_pending() == 0);
	// Up again: the read-on-start datapoints are read again.
	f.run_for(1000);
	f.socket.inject(hc_test::connect_response(8));
	f.gateway_sequence = 0;
	CHECK(f.adapter.reads_pending() == 3);
	f.run_for(100);
	CHECK(f.last_telegram() == "0A 03 01 00 00");
	f.socket.inject(hc_test::tunneling_ack(8, 0));
}

namespace {

/// An application component owning datapoints the adapter transmits (the time master's role).
struct Owner : hc::Technology {
	const char* name() const override { return "owner"; }
	bool command(const hc::Datapoint&, const Value&) override { return false; }
};

/// Ids: 0 clock (10.001, owned by `owner`, transmit-only on 0/0/1, readable, read-on-start which the
/// adapter ignores), 1 display (10.001, listens 0/0/1: a device showing the time), 2 light (1.001,
/// the adapter's own, sends 1/0/1).
struct TransmitFixture : AdapterFixture {
	Owner owner;
	GroupAddress display_listen[1] = {GroupAddress::of(0, 0, 1)};

	TransmitFixture() {
		DatapointSpec specs[] = {
			{"clock", ValueType::time_of_day,
				DatapointFlags::kReadable | DatapointFlags::kTransmit | DatapointFlags::kReadOnStart},
			{"display", ValueType::time_of_day, 0},
			{"light", ValueType::boolean, kReadWrite},
		};
		REQUIRE(model.load(specs, 3) == DatapointStatus::ok);
		REQUIRE(model.bind(0, owner) == DatapointStatus::ok);
		KnxMapping mappings[3] = {
			clock_mapping(),
			mapping(1, Dpt{10, 1}, std::nullopt, display_listen, 1),
			mapping(2, Dpt{1, 1}, GroupAddress::of(1, 0, 1), nullptr, 0),
		};
		REQUIRE(adapter.configure(mappings, 3));
	}

	static KnxMapping clock_mapping() {
		KnxMapping m = mapping(0, Dpt{10, 1}, GroupAddress::of(0, 0, 1), nullptr, 0);
		m.transmit_only = true;
		return m;
	}

	/// Up; nothing to read on start (only the clock has the flag, which the adapter ignores).
	void up() {
		connect(7);
		run_for(1000);
	}
};

const hc::TimeOfDay kNoon{3, 12, 10, 0}; // Wednesday 12:10:00: 6C 0A 00

} // namespace

TEST_CASE("KnxAdapter.TransmitOnlyBindsAsTransmitter") {
	TransmitFixture f;
	CHECK(f.dp(0).technology() == &f.owner);
	CHECK(f.dp(0).transmitter() == &f.adapter);
	CHECK(f.dp(1).technology() == &f.adapter);
	CHECK(f.adapter.sender_of(GroupAddress::of(0, 0, 1)) == std::optional<uint16_t>(0));
	CHECK(f.adapter.sender_of(GroupAddress::of(1, 0, 1)) == std::optional<uint16_t>(2));
	CHECK_FALSE(f.adapter.sender_of(GroupAddress::of(0, 0, 2)));
	// The clock's read-on-start flag is not the adapter's business: nothing is read.
	f.connect(7);
	CHECK(f.adapter.reads_pending() == 0);
	f.run_for(1000);
	CHECK(f.socket.count(ip::Service::tunneling_request) == 0);
}

TEST_CASE("KnxAdapter.TransmitOnlyPublishSendsAndAnswersReads") {
	TransmitFixture f;
	// Down: the publish is refused, nothing stored.
	CHECK(f.model.publish(f.owner, 0, Value::of_time(kNoon)) == DatapointStatus::refused);
	CHECK(f.capture.sink.contains("W hc.knx: refusing command for 'clock': the KNX tunnel is down"));
	CHECK(f.dp(0).quality() == Quality::unknown);

	f.up();
	CHECK(f.model.publish(f.owner, 0, Value::of_time(kNoon)) == DatapointStatus::ok);
	CHECK(f.last_telegram() == "00 01 04 00 80 6C 0A 00");
	CHECK(f.dp(0).value().as_time() == kNoon);
	CHECK(f.dp(1).quality() == Quality::unknown);
	f.confirm_last();
	CHECK(f.dp(1).value().as_time() == kNoon); // the display on the address takes it, like the bus devices
	CHECK(f.log.at(f.log.size() - 1)->datapoint == 0);

	// A read of the address is answered from the published value.
	size_t base = f.socket.count(ip::Service::tunneling_request);
	f.receive(0x0001, "00 00");
	REQUIRE(f.socket.count(ip::Service::tunneling_request) == base + 1);
	CHECK(f.last_telegram() == "00 01 04 00 40 6C 0A 00");
	f.confirm_last();

	// Another device writes the address: the display follows, the owner's value does not.
	f.receive(0x0001, "00 80 6D 00 00");
	CHECK(f.dp(1).value().as_time() == hc::TimeOfDay{3, 13, 0, 0});
	CHECK(f.dp(0).value().as_time() == kNoon);
	CHECK(f.capture.sink.contains("D hc.knx: 0/0/1 from 1.1.10: 'clock' is transmit-only, not updated"));

	// The clock is not writable: the model refuses the command before any technology sees it.
	base = f.socket.count(ip::Service::tunneling_request);
	CHECK(f.model.command(0, Value::of_time(kNoon)) == DatapointStatus::not_writable);
	CHECK(f.socket.count(ip::Service::tunneling_request) == base);
}

TEST_CASE("KnxAdapter.TransmitOnlyIsNotMarkedStale") {
	TransmitFixture f;
	f.up();
	REQUIRE(f.model.publish(f.owner, 0, Value::of_time(kNoon)) == DatapointStatus::ok);
	f.confirm_last();
	f.socket.inject(hc_test::disconnect_request(7));
	CHECK(f.dp(1).quality() == Quality::stale); // the adapter's own
	CHECK(f.dp(0).quality() == Quality::good);  // the owner decides
	// The owner's stale report is its own.
	CHECK(f.model.report_stale(f.owner, 0) == DatapointStatus::ok);
	CHECK(f.dp(0).quality() == Quality::stale);
}

TEST_CASE("KnxAdapter.TransmitOnlyRefusals") {
	TransmitFixture f;
	const char* error = nullptr;
	auto refused = [&](KnxMapping* mappings, size_t count, const char* text) {
		CHECK_FALSE(f.adapter.configure(mappings, count, &error));
		CHECK(f.capture.sink.contains(text));
		CHECK(f.adapter.size() == 3);
	};
	// The address another datapoint sends on.
	KnxMapping two[2] = {TransmitFixture::clock_mapping(),
		AdapterFixture::mapping(2, Dpt{1, 1}, GroupAddress::of(0, 0, 1), nullptr, 0)};
	refused(two, 2, "mapping 0: mapping 1 sends on the same address");
	// Listening addresses.
	KnxMapping m = TransmitFixture::clock_mapping();
	m.listen = f.display_listen;
	m.listen_count = 1;
	refused(&m, 1, "mapping 0: a transmit-only datapoint listens to no address");
	// No sending address.
	m = TransmitFixture::clock_mapping();
	m.has_send = false;
	m.listen = f.display_listen;
	m.listen_count = 1;
	refused(&m, 1, "a transmit-only datapoint needs a sending address");
	// No owner, the adapter as owner, no transmit flag.
	m = AdapterFixture::mapping(1, Dpt{10, 1}, GroupAddress::of(0, 0, 2), nullptr, 0);
	m.transmit_only = true;
	refused(&m, 1, "a transmit-only datapoint must be owned by another component, not the adapter");
	DatapointSpec specs[] = {
		{"clock", ValueType::time_of_day, DatapointFlags::kTransmit},
		{"plain", ValueType::time_of_day, DatapointFlags::kReadable},
	};
	REQUIRE(f.model.load(specs, 2) == DatapointStatus::ok);
	m = TransmitFixture::clock_mapping();
	refused(&m, 1, "a transmit-only datapoint needs its owner bound first");
	REQUIRE(f.model.bind(1, f.owner) == DatapointStatus::ok);
	m.datapoint = 1;
	refused(&m, 1, "a transmit-only datapoint needs the transmit flag");
	// Another transmitter.
	Owner other;
	REQUIRE(f.model.bind(0, f.owner) == DatapointStatus::ok);
	REQUIRE(f.model.set_transmitter(0, other) == DatapointStatus::ok);
	m.datapoint = 0;
	refused(&m, 1, "mapping 0: the datapoint has another transmitter");
	// Without transmit_only a foreign datapoint stays refused.
	m.transmit_only = false;
	refused(&m, 1, "the datapoint belongs to another technology");
}

TEST_CASE("KnxAdapter.ReadOnRequest") {
	AdapterFixture f;
	// The tunnel is down: refused.
	CHECK(f.model.read(0) == DatapointStatus::refused);
	CHECK(f.capture.sink.contains("refusing read of 'light': the KNX tunnel is"));
	f.connect(7);
	f.run_for(1000);
	while (f.adapter.reads_pending() > 0 || f.tunnel.queued() > 0) {
		f.confirm_last();
		f.run_for(100);
	}
	size_t base = f.socket.count(ip::Service::tunneling_request);
	// A read of the light goes to its sending address after the read spacing; a second request
	// while it waits is the same read.
	uint64_t asked = f.clock.now_ms;
	CHECK(f.model.read(0) == DatapointStatus::ok);
	CHECK(f.model.read(0) == DatapointStatus::ok);
	CHECK(f.adapter.reads_pending() == 1);
	f.run_until(asked + 99);
	CHECK(f.socket.count(ip::Service::tunneling_request) == base);
	f.run_until(asked + 100);
	REQUIRE(f.socket.count(ip::Service::tunneling_request) == base + 1);
	CHECK(f.last_telegram() == "08 01 01 00 00");
	CHECK(f.adapter.reads_pending() == 0);
	f.confirm_last();
	// Two datapoints, the later one asked for first: both are read, one spacing apart.
	CHECK(f.model.read(4) == DatapointStatus::ok);
	CHECK(f.model.read(1) == DatapointStatus::ok);
	f.run_for(100);
	CHECK(f.last_telegram() == "10 01 01 00 00"); // setpoint: its first listening address
	f.confirm_last();
	f.run_for(100);
	CHECK(f.last_telegram() == "0A 03 01 00 00"); // temperature
	f.confirm_last();
	f.run_for(1000);
	CHECK(f.socket.count(ip::Service::tunneling_request) == base + 3);
	// The answer reports.
	f.receive(0x0A03, "00 40 0C 1A");
	CHECK(f.dp(1).value().as_real() == doctest::Approx(21.0));
	// The tunnel goes down: the waiting reads go with it.
	CHECK(f.model.read(2) == DatapointStatus::ok);
	f.socket.inject(hc_test::disconnect_request(7));
	CHECK(f.adapter.reads_pending() == 0);
}

TEST_CASE("KnxAdapter.ReadRefusals") {
	TransmitFixture f;
	f.up();
	// The transmit-only clock belongs to its owner, which cannot be read: the model asks the owner.
	CHECK(f.model.read(0) == DatapointStatus::refused);
	CHECK(f.capture.sink.contains("read of 'clock' refused by owner"));
	// A datapoint of the adapter's that is not mapped.
	DatapointSpec specs[] = {{"a", ValueType::boolean, kReadWrite}, {"b", ValueType::boolean, kReadWrite}};
	REQUIRE(f.model.load(specs, 2) == DatapointStatus::ok);
	KnxMapping m = AdapterFixture::mapping(0, Dpt{1, 1}, GroupAddress::of(3, 0, 1), nullptr, 0);
	REQUIRE(f.adapter.configure(&m, 1));
	REQUIRE(f.model.bind(1, f.adapter) == DatapointStatus::ok);
	CHECK(f.model.read(1) == DatapointStatus::refused);
	CHECK(f.capture.sink.contains("refusing read of 'b': not mapped to a group address"));
	CHECK(f.model.read(0) == DatapointStatus::ok);
}
