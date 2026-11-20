/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// A persisted value counts after a reboot (homecontrol #100): the value store restores the last value
// of every kPersist datapoint with quality good, whatever its technology (an application value, a
// KNX group address only the controller writes, a Zigbee attribute); where the bus knows better,
// kReadOnStart reads it and the answer replaces the restored value. The restart is the device's own:
// the image service boots a second time on the same blob store, image slot and Zigbee database.

#include <algorithm>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/controller.h"
#include "hc/datapoint.h"
#include "hc/image/image_service.h"
#include "hc/image/memory_image_slot.h"
#include "hc/knx/adapter.h"
#include "hc/knx/tunnel.h"
#include "hc/rules/engine.h"
#include "hc/value_store.h"
#include "hc/zigbee/adapter.h"
#include "image_support.h"
#include "knx_test_support.h"
#include "test_support.h"
#include "zigbee_test_support.h"

using hc::DatapointFlags;
using hc::DatapointModel;
using hc::DatapointSpec;
using hc::DatapointStatus;
using hc::Quality;
using hc::Value;
using hc::ValueType;
using hc::image::ImageService;
using hc::image::MemoryImageSlot;
using hc_test::Bytes;
using hc_test::describe;

namespace {

/// What survives the restart: the value store's blobs, the image slot, the Zigbee database.
struct Storage {
	hc_test::FakeBlobStore blobs;
	MemoryImageSlot slot;
	hc_test::FakeBlobStore zigbee;
};

/// One boot of the controller: the image service with the KNX tunnel and adapter and the Zigbee
/// adapter as its provider (main.cpp's wiring), on `storage`.
struct Boot {
	hc_test::FakePlatform p;
	hc::Controller controller{p.platform()};
	DatapointModel model{p.clock};
	hc_test::FakeUdpSocket socket{p.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), p.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter knx{controller.loop(), p.clock, model, tunnel};
	hc_test::FakeZigbeeDriver driver;
	hc::zigbee::ZigbeeAdapter zigbee;
	ImageService service;
	hc_test::ZigbeeActions device{zigbee};
	size_t handled = 0;
	uint8_t gateway_sequence = 0;
	std::vector<std::string> telegrams; ///< "dest len tpdu" of each group telegram the device sent.

	explicit Boot(Storage& storage)
		: zigbee(controller.loop(), p.clock, model, driver, &storage.zigbee),
		  service(controller, model, storage.blobs, storage.slot, knx, &tunnel) {
		controller.start();
		controller.connectivity().link_up("192.0.2.10");
		knx.start();
		REQUIRE(service.add_provider(zigbee));
		REQUIRE(zigbee.start());
		zigbee.network_state(true, 0x1a62, 15, 0xdddddddddddddddd);
		service.start();
	}

	bool load(const std::string& json) { return hc_test::deploy(service, json); }

	/// The tunnel to the test gateway is up on channel 7.
	void connect() {
		std::optional<hc::knx::GatewayAddress> gateway = hc::knx::GatewayAddress::parse("192.0.2.1");
		REQUIRE(gateway);
		tunnel.set_gateway(&*gateway, false);
		tunnel.start();
		run_for(0);
		socket.inject(hc_test::connect_response(7));
		REQUIRE(tunnel.is_up());
		pump();
	}

	/// Acknowledges and confirms every TUNNELING_REQUEST sent, recording its telegram.
	void pump() {
		for (int round = 0; round < 100; ++round) {
			controller.run_once();
			if (handled == socket.sent.size()) {
				return;
			}
			while (handled < socket.sent.size()) {
				Bytes request = socket.sent[handled++].data;
				if (request.size() >= 8 && request[2] == 0x02 && request[3] == 0x07) {
					socket.inject(hc_test::connectionstate_response(7));
					continue;
				}
				if (request.size() < 10 || request[2] != 0x04 || request[3] != 0x20) {
					continue;
				}
				telegrams.push_back(hc_test::hex(Bytes(request.begin() + 16, request.end())));
				socket.inject(hc_test::tunneling_ack(7, request[8]));
				Bytes cemi(request.begin() + 10, request.end());
				cemi[0] = 0x2E;
				socket.inject(hc_test::tunneling_request(7, gateway_sequence++, cemi));
			}
		}
	}

	/// A group telegram from 1.1.10 to `destination` (`tpdu`: "00 80 ..." a write, "00 40 ..." a response).
	void receive(uint16_t destination, const char* tpdu) {
		Bytes cemi = hc_test::bytes("29 00 BC D0 11 0A");
		cemi.push_back(static_cast<uint8_t>(destination >> 8));
		cemi.push_back(static_cast<uint8_t>(destination));
		Bytes t = hc_test::bytes(tpdu);
		cemi.push_back(static_cast<uint8_t>(t.size() - 1));
		cemi.insert(cemi.end(), t.begin(), t.end());
		socket.inject(hc_test::tunneling_request(7, gateway_sequence++, cemi));
		pump();
	}

	/// Runs the loop, jumping from deadline to deadline, for `ms`, answering the tunnel's requests.
	void run_for(uint64_t ms) {
		uint64_t until = p.clock.now_ms + ms;
		for (;;) {
			pump();
			uint64_t deadline = controller.next_deadline();
			if (deadline > until) {
				p.clock.now_ms = until;
				pump();
				return;
			}
			if (deadline > p.clock.now_ms) {
				p.clock.now_ms = deadline;
			}
		}
	}

	bool sent(const std::string& telegram) const {
		return std::find(telegrams.begin(), telegrams.end(), telegram) != telegrams.end();
	}

	const hc::Datapoint& dp(const char* name) const {
		const hc::Datapoint* d = model.find(name);
		REQUIRE_MESSAGE(d != nullptr, "no datapoint " << name);
		return *d;
	}
};

// 1/1/1 (a group address only the controller writes), 1/1/2 (a temperature read on start).
constexpr uint16_t kCommandAddress = (1 << 11) | (1 << 8) | 1;
constexpr uint16_t kTemperatureAddress = (1 << 11) | (1 << 8) | 2;

/// The house's case (homecontrol #100): a persisted application setting chooses the blinds'
/// position at the start, `write x = if(@setting, 100, 80)`; `other` is the same without persist.
/// Beside it a persisted KNX datapoint without and one with readOnStart, and a persisted Zigbee one.
const char* const kImage = R"({"image": {"schemaVersion": "1.7", "projectId": "restore", "revision": 1},
"datapoints": [
 {"id": "setting", "type": "boolean", "writable": true, "persist": true, "virtual": {}},
 {"id": "other", "type": "boolean", "writable": true, "virtual": {}},
 {"id": "x", "type": "unsigned", "writable": true, "virtual": {}},
 {"id": "y", "type": "unsigned", "writable": true, "virtual": {}},
 {"id": "command", "type": "boolean", "writable": true, "transmit": true, "persist": true,
  "knx": {"dpt": "1.001", "send": "1/1/1"}},
 {"id": "temperature", "type": "real", "readOnStart": true, "persist": true,
  "knx": {"dpt": "9.001", "listen": ["1/1/2"]}},
 {"id": "ceiling", "type": "boolean", "writable": true, "transmit": true, "persist": true,
  "zigbee": {"ieee": "00124b0001020304", "endpoint": 11, "cluster": 6, "attribute": 0, "command": 8}}
],
"rules": [
 {"id": "blinds", "name": "Blinds", "reactions": [{"triggers": [{"kind": "start"}], "actions": [
  {"kind": "write", "datapoint": "x", "value": {"kind": "call", "function": "if", "args": [
   {"kind": "datapoint", "name": "setting"},
   {"kind": "literal", "type": "integer", "value": 100}, {"kind": "literal", "type": "integer", "value": 80}]}},
  {"kind": "write", "datapoint": "y", "value": {"kind": "call", "function": "if", "args": [
   {"kind": "datapoint", "name": "other"},
   {"kind": "literal", "type": "integer", "value": 100}, {"kind": "literal", "type": "integer", "value": 80}]}}]}]}
]})";

/// The first boot: the image, a value for each datapoint, an orderly shutdown.
void first_boot(Storage& storage) {
	Boot first(storage);
	first.connect();
	first.device.join_bulb();
	first.run_for(2000);
	REQUIRE_MESSAGE(first.load(kImage), describe(first.service.report()));
	first.run_for(10);
	// Nothing had a value at this start: the rule wrote nothing.
	CHECK(first.dp("x").quality() == Quality::unknown);
	REQUIRE(first.model.command(first.dp("setting").id(), Value::of_bool(true)) == DatapointStatus::ok);
	REQUIRE(first.model.command(first.dp("other").id(), Value::of_bool(true)) == DatapointStatus::ok);
	REQUIRE(first.model.command(first.dp("command").id(), Value::of_bool(true)) == DatapointStatus::ok);
	first.run_for(10);
	CHECK(first.sent("09 01 01 00 81"));
	first.receive(kTemperatureAddress, "00 80 0C 1A"); // 21.0 °C
	CHECK(first.dp("temperature").value().as_real() == doctest::Approx(21.0));
	first.device.report(hc_test::kBulbShort, 11, hc::zigbee::cluster::kOnOff, hc::zigbee::attribute::kOnOff,
		hc::zigbee::zcl_type::kBool, 1);
	CHECK(first.dp("ceiling").value() == Value::of_bool(true));
	first.service.flush();
	CHECK(first.service.value_store().pending() == 0);
}

} // namespace

TEST_CASE("PersistedRestore.EveryTechnologyRestoresGoodAndTheRulesSeeIt") {
	Storage storage;
	first_boot(storage);

	// The restart, with the KNX tunnel still down.
	Boot second(storage);
	REQUIRE(second.service.loaded());
	second.run_for(10);
	struct Expected {
		const char* name;
		Value value;
	};
	const Expected restored[] = {{"setting", Value::of_bool(true)}, {"command", Value::of_bool(true)},
		{"temperature", Value::of_real(21.0)}, {"ceiling", Value::of_bool(true)}};
	for (const Expected& e : restored) {
		CAPTURE(e.name);
		const hc::Datapoint& dp = second.dp(e.name);
		CHECK(dp.persists());
		CHECK(dp.quality() == Quality::good);
		CHECK(dp.value() == e.value);
		CHECK(dp.updated_ms() == 0); // nothing was updated in this boot
	}
	CHECK(second.dp("command").technology() == &second.knx);
	CHECK_FALSE(second.dp("command").read_on_start());
	CHECK(second.dp("temperature").read_on_start());
	CHECK(second.dp("ceiling").technology() == &second.zigbee);
	// The rule's start: the restored setting is a value, `x` is 100; `other` was not persisted.
	CHECK(second.dp("x").value() == Value::of_unsigned(100));
	CHECK(second.dp("y").quality() == Quality::unknown);
	hc::rules::RuleDiagnostics diagnostics;
	REQUIRE(second.service.engine().diagnostics(0, diagnostics));
	CHECK(diagnostics.last_error_code == hc::rules::LogCode::value_no_value);
	CHECK(diagnostics.last_error_datapoint == second.dp("other").id());

	// The tunnel comes up: the read-on-start datapoint is read, the controller-written one is not.
	second.connect();
	second.run_for(1000);
	CHECK(second.sent("09 02 01 00 00"));
	CHECK_FALSE(second.sent("09 01 01 00 00"));
	CHECK(second.dp("temperature").value() == Value::of_real(21.0));
	CHECK(second.dp("temperature").quality() == Quality::good);
	// The answer replaces the restored value.
	second.receive(kTemperatureAddress, "00 40 0C 4C"); // 22.0 °C
	CHECK(second.dp("temperature").value().as_real() == doctest::Approx(22.0));
	CHECK(second.dp("temperature").updated_ms() != 0);
	// The group address only the controller writes keeps its restored value as good.
	CHECK(second.dp("command").quality() == Quality::good);
	CHECK(second.dp("command").value() == Value::of_bool(true));
	// Stale keeps its meaning: the tunnel goes down.
	second.tunnel.stop();
	second.run_for(10);
	CHECK(second.dp("command").quality() == Quality::stale);
	CHECK(second.dp("temperature").quality() == Quality::stale);
}

TEST_CASE("PersistedRestore.AReportBeforeOrAfterTheRestoreWins") {
	hc_test::ScopedCapture capture;
	hc_test::FakeBlobStore blobs;
	struct Bus : hc::Technology {
		const char* name() const override { return "bus"; }
		bool command(const hc::Datapoint&, const Value&) override { return true; }
	} bus;
	const DatapointSpec specs[] = {{"a", ValueType::real, DatapointFlags::kPersist | DatapointFlags::kReadOnStart},
		{"b", ValueType::real, DatapointFlags::kPersist | DatapointFlags::kReadOnStart}};
	{
		hc_test::FakeClock clock;
		hc::EventLoop loop;
		DatapointModel model(clock);
		hc::ValueStore store(loop, clock, model, blobs);
		REQUIRE(model.load(specs, 2) == DatapointStatus::ok);
		REQUIRE(store.load() == DatapointStatus::ok);
		REQUIRE(model.bind(0, bus) == DatapointStatus::ok);
		REQUIRE(model.bind(1, bus) == DatapointStatus::ok);
		REQUIRE(model.report(bus, 0, Value::of_real(1.0)) == DatapointStatus::ok);
		REQUIRE(model.report(bus, 1, Value::of_real(2.0)) == DatapointStatus::ok);
		store.flush();
	}
	hc_test::FakeClock clock;
	clock.advance(5000);
	hc::EventLoop loop;
	DatapointModel model(clock);
	hc::ValueStore store(loop, clock, model, blobs);
	REQUIRE(model.load(specs, 2) == DatapointStatus::ok);
	REQUIRE(model.bind(0, bus) == DatapointStatus::ok);
	REQUIRE(model.bind(1, bus) == DatapointStatus::ok);
	// The answer of `a` comes before the restore: the restore leaves it alone.
	REQUIRE(model.report(bus, 0, Value::of_real(10.0)) == DatapointStatus::ok);
	REQUIRE(store.load() == DatapointStatus::ok);
	CHECK(model.get(0)->value() == Value::of_real(10.0));
	CHECK(model.get(0)->quality() == Quality::good);
	CHECK(model.get(0)->updated_ms() == clock.now_ms);
	CHECK(model.get(1)->value() == Value::of_real(2.0));
	CHECK(model.get(1)->quality() == Quality::good);
	CHECK(model.get(1)->updated_ms() == 0);
	// The answer of `b` comes after it: it replaces the restored value.
	clock.advance(100);
	REQUIRE(model.report(bus, 1, Value::of_real(20.0)) == DatapointStatus::ok);
	CHECK(model.get(1)->value() == Value::of_real(20.0));
	CHECK(model.get(1)->updated_ms() == clock.now_ms);
	// A second restore (another load of the store, an image swap) never overwrites either.
	REQUIRE(store.load() == DatapointStatus::ok);
	CHECK(model.get(0)->value() == Value::of_real(10.0));
	CHECK(model.get(1)->value() == Value::of_real(20.0));
	CHECK_FALSE(capture.sink.contains("refusing restore"));
}
