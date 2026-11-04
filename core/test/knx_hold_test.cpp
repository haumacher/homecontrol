/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Commands while the KNX tunnel is not up are held and sent once it connects (homecontrol #72):
// the boot with the tunnel down (a block's output and a rule's start write), the last value of
// several commands, the image swap that drops or keeps a held datapoint, the pending mark in the
// API and the live feed.

#include <algorithm>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/image/image_service.h"
#include "hc/knx/adapter.h"
#include "hc/knx/tunnel.h"
#include "knx_test_support.h"
#include "web_test_support.h"

using hc::DatapointStatus;
using hc::Quality;
using hc::Value;
using hc_test::Bytes;
using hc_test::Json;

namespace {

/// The issue's case: a room controller whose `switch` sends on 0/0/13, a heat request on 0/0/14
/// that a rule writes from the block's demand, a frost output on 0/0/46 that a rule writes at start,
/// and a lamp on 1/0/1 that only the test commands.
std::string image(unsigned revision, bool with_lamp = true) {
	std::string text = R"({"image": {"schemaVersion": "1.3", "projectId": "hold", "revision": )" +
		std::to_string(revision) + R"(},
 "datapoints": [
  {"id": "room.temperature", "type": "real", "knx": {"dpt": "9.001", "listen": ["0/0/20"]}},
  {"id": "room.heat", "type": "boolean", "writable": true, "transmit": true, "knx": {"dpt": "1.001", "send": "0/0/13"}},
  {"id": "house.request", "type": "boolean", "writable": true, "transmit": true, "knx": {"dpt": "1.001", "send": "0/0/14"}},
  {"id": "frost", "type": "boolean", "writable": true, "transmit": true, "knx": {"dpt": "1.001", "send": "0/0/46"}})";
	if (with_lamp) {
		text += R"(,
  {"id": "lamp", "type": "boolean", "writable": true, "transmit": true, "knx": {"dpt": "1.001", "send": "1/0/1"}})";
	}
	text += R"(],
 "blocks": [{"id": "rr", "type": "org.homecontrol.room-controller", "version": "1.0", "label": "Room",
   "inputs": {"temperature": "room.temperature"}, "outputs": {"switch": "room.heat"}, "parameters": {"system": "convector"}}],
 "rules": [{"id": "frost", "name": "Frost", "reactions": [{"triggers": [{"kind": "start"}],
    "actions": [{"kind": "write", "datapoint": "frost", "value": {"kind": "literal", "type": "boolean", "value": true}}]}]},
   {"id": "request", "name": "Request", "reactions": [{"triggers": [{"kind": "start"}, {"kind": "datapoint", "datapoint": "block/rr/demand"}],
    "actions": [{"kind": "write", "datapoint": "house.request", "value": {"kind": "datapoint", "name": "block/rr/demand"}}]}]}]})";
	return text;
}

/// The web API over an image service with a KNX tunnel to the test gateway, which stays down until
/// connect().
struct HoldFixture : hc_test::WebFixture {
	hc_test::FakeUdpSocket socket{fake.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), fake.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{controller.loop(), fake.clock, model, tunnel};
	hc_test::FakeBlobStore blobs;
	hc::image::ImageService service{controller, model, blobs, adapter, &tunnel};
	std::string cookie;
	size_t handled = 0;
	uint8_t gateway_sequence = 0;

	HoldFixture() {
		controller.start();
		controller.connectivity().link_up("192.0.2.10");
		adapter.start();
		service.start();
		api.set_image(&service);
		cookie = onboard();
	}

	void load(const std::string& text) {
		bool loaded = service.load(text.data(), text.size(), hc::image::Source::api, true);
		std::string why;
		for (size_t i = 0; i < service.report().count; ++i) {
			why += std::string(service.report().refusals[i].path) + ": " + service.report().refusals[i].reason + "\n";
		}
		REQUIRE_MESSAGE(loaded, why);
		run();
	}

	void run() {
		for (int i = 0; i < 8; ++i) {
			controller.run_once();
		}
	}

	void connect() {
		std::optional<hc::knx::GatewayAddress> gateway = hc::knx::GatewayAddress::parse("192.0.2.1");
		REQUIRE(gateway);
		tunnel.set_gateway(&*gateway, false);
		tunnel.start();
		run();
		socket.inject(hc_test::connect_response(7));
		REQUIRE(tunnel.is_up());
		pump();
	}

	/// Acknowledges and confirms every TUNNELING_REQUEST sent; advances the clock so the reads go.
	void pump() {
		for (int round = 0; round < 200; ++round) {
			controller.run_once();
			if (handled == socket.sent.size()) {
				uint64_t deadline = controller.next_deadline();
				if (deadline > fake.clock.now_ms + 2000) {
					return;
				}
				fake.clock.now_ms = std::max(fake.clock.now_ms, deadline);
				continue;
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
				socket.inject(hc_test::tunneling_ack(7, request[8]));
				Bytes cemi(request.begin() + 10, request.end());
				cemi[0] = 0x2E;
				cemi[4] = 0x11; // source 1.1.250
				cemi[5] = 0xFA;
				socket.inject(hc_test::tunneling_request(7, gateway_sequence++, cemi));
			}
		}
	}

	/// The group telegrams the device sent: "dest len tpdu".
	std::vector<std::string> telegrams() const {
		std::vector<std::string> out;
		for (const auto& d : socket.sent) {
			if (d.data.size() >= 16 && d.data[2] == 0x04 && d.data[3] == 0x20) {
				out.push_back(hc_test::hex(Bytes(d.data.begin() + 16, d.data.end())));
			}
		}
		return out;
	}

	size_t count(const std::string& telegram) const {
		std::vector<std::string> all = telegrams();
		return static_cast<size_t>(std::count(all.begin(), all.end(), telegram));
	}

	const hc::Datapoint& dp(const char* name) const {
		const hc::Datapoint* d = model.find(name);
		REQUIRE_MESSAGE(d != nullptr, "no datapoint " << name);
		return *d;
	}

	/// The datapoint's object in GET /api/datapoints.
	Json api_datapoint(const char* name) {
		Result r = get("/api/datapoints", cookie);
		REQUIRE(r.status == 200);
		for (const Json& item : r.json["datapoints"].items) {
			if (item["name"].text == name) {
				return item;
			}
		}
		FAIL("no datapoint " << name << " in /api/datapoints");
		return Json();
	}
};

// Group writes on: 0/0/13, 0/0/14, 0/0/46, 1/0/1; off on 1/0/1.
const char* const kHeatOn = "00 0D 01 00 81";
const char* const kRequestOn = "00 0E 01 00 81";
const char* const kFrostOn = "00 2E 01 00 81";
const char* const kLampOn = "08 01 01 00 81";
const char* const kLampOff = "08 01 01 00 80";

} // namespace

TEST_CASE("KnxHold: a boot with the tunnel down sends the start cascade's writes once the tunnel is up") {
	HoldFixture f;
	hc_test::ScopedCapture capture(hc::LogLevel::info);
	f.load(image(1));
	// The cascade ran: the block's switch (no temperature yet: the fallback valve, so on), the rules'
	// start writes. Accepted, stored, held.
	CHECK(f.dp("block/rr/switch").value() == Value::of_bool(true));
	CHECK(f.dp("room.heat").value() == Value::of_bool(true));
	CHECK(f.dp("room.heat").quality() == Quality::good);
	CHECK(f.dp("room.heat").pending());
	CHECK(f.dp("house.request").value() == Value::of_bool(true));
	CHECK(f.dp("house.request").pending());
	CHECK(f.dp("frost").pending());
	CHECK_FALSE(f.dp("lamp").pending());
	CHECK_FALSE(capture.sink.contains("refused"));
	CHECK(capture.sink.contains("I hc.knx: holding commands until the KNX tunnel is up (down"));
	CHECK(f.adapter.commands_held() == 3);
	CHECK(f.telegrams().empty());
	CHECK(f.api_datapoint("room.heat")["pending"].boolean == true);
	CHECK(f.api_datapoint("lamp")["pending"].boolean == false);

	f.connect();
	CHECK(capture.sink.contains("I hc.knx: knx tunnel up: sending 3 held command(s)"));
	CHECK(capture.sink.contains("I hc.knx: sending the held command on for 'room.heat' to 0/0/13"));
	// Once each, in datapoint order, before the read of anything else.
	std::vector<std::string> sent = f.telegrams();
	REQUIRE(sent.size() >= 3);
	CHECK(sent[0] == kHeatOn);
	CHECK(sent[1] == kRequestOn);
	CHECK(sent[2] == kFrostOn);
	CHECK(f.count(kHeatOn) == 1);
	CHECK(f.count(kRequestOn) == 1);
	CHECK(f.count(kFrostOn) == 1);
	CHECK(f.adapter.commands_held() == 0);
	CHECK_FALSE(f.dp("room.heat").pending());
	CHECK(f.api_datapoint("room.heat")["pending"].boolean == false);
	// Nothing is sent again later.
	f.pump();
	CHECK(f.count(kHeatOn) == 1);
	CHECK(f.count(kFrostOn) == 1);
}

TEST_CASE("KnxHold: two commands while down send only the last; a command while up goes out at once") {
	HoldFixture f;
	f.load(image(1));
	REQUIRE(f.model.command(f.dp("lamp").id(), Value::of_bool(true)) == DatapointStatus::ok);
	REQUIRE(f.model.command(f.dp("lamp").id(), Value::of_bool(false)) == DatapointStatus::ok);
	CHECK(f.dp("lamp").value() == Value::of_bool(false));
	CHECK(f.dp("lamp").pending());
	CHECK(f.adapter.commands_held() == 4);
	f.connect();
	CHECK(f.count(kLampOff) == 1);
	CHECK(f.count(kLampOn) == 0);
	// Connected: the command is sent as before, not held.
	REQUIRE(f.model.command(f.dp("lamp").id(), Value::of_bool(true)) == DatapointStatus::ok);
	CHECK_FALSE(f.dp("lamp").pending());
	CHECK(f.telegrams().back() == kLampOn);
	f.pump();
	CHECK(f.count(kLampOn) == 1);
}

TEST_CASE("KnxHold: a swap that removes a held datapoint sends nothing for it") {
	HoldFixture f;
	hc_test::ScopedCapture capture(hc::LogLevel::info);
	f.load(image(1));
	REQUIRE(f.model.command(f.dp("lamp").id(), Value::of_bool(true)) == DatapointStatus::ok);
	REQUIRE(f.dp("lamp").pending());
	f.load(image(2, false));
	CHECK(f.model.find("lamp") == nullptr);
	CHECK(f.adapter.commands_held() == 3); // the kept ones stay held (the next case)
	f.connect();
	CHECK(f.count(kLampOn) == 0);
	for (const std::string& t : f.telegrams()) {
		CHECK(t.rfind("08 01", 0) != 0); // nothing to 1/0/1 at all
	}
	CHECK(f.count(kHeatOn) == 1);
}

TEST_CASE("KnxHold: a swap that keeps a held datapoint still sends it, once") {
	HoldFixture f;
	f.load(image(1));
	REQUIRE(f.model.command(f.dp("lamp").id(), Value::of_bool(true)) == DatapointStatus::ok);
	uint16_t before = f.dp("lamp").id();
	// The new revision drops the frost output: the lamp keeps its name, not its id.
	std::string next = image(2);
	size_t at = next.find(R"(,
  {"id": "frost")");
	REQUIRE(at != std::string::npos);
	size_t end = next.find("}}", at) + 2;
	next.erase(at, end - at);
	size_t rule = next.find(R"({"id": "frost", "name")");
	REQUIRE(rule != std::string::npos);
	size_t rule_end = next.find(R"(   {"id": "request")");
	next.erase(rule, rule_end - rule);
	f.load(next);
	REQUIRE(f.model.find("frost") == nullptr);
	CHECK(f.dp("lamp").id() != before);
	CHECK(f.dp("lamp").pending());
	CHECK(f.dp("lamp").value() == Value::of_bool(true));
	f.connect();
	CHECK(f.count(kLampOn) == 1);
	CHECK(f.count(kHeatOn) == 1);
	CHECK(f.count(kFrostOn) == 0);
	CHECK_FALSE(f.dp("lamp").pending());
}

TEST_CASE("KnxHold: refusals for other reasons stay; a report drops the held value; a transmit-only one is not held") {
	HoldFixture f;
	hc_test::ScopedCapture capture;
	f.load(image(1));
	// Not writable: refused by the model, nothing held.
	CHECK(f.model.command(f.dp("room.temperature").id(), Value::of_real(20.0)) == DatapointStatus::not_writable);
	CHECK_FALSE(f.dp("room.temperature").pending());
	// No conversion: refused.
	CHECK(f.model.command(f.dp("lamp").id(), Value::of_real(0.5)) == DatapointStatus::wrong_type);
	CHECK_FALSE(f.dp("lamp").pending());
	// A refused command keeps a held one.
	REQUIRE(f.model.command(f.dp("lamp").id(), Value::of_bool(true)) == DatapointStatus::ok);
	CHECK(f.model.command(f.dp("lamp").id(), Value::of_real(0.5)) == DatapointStatus::wrong_type);
	CHECK(f.dp("lamp").pending());
	CHECK(f.dp("lamp").value() == Value::of_bool(true));
}

TEST_CASE("KnxHold: the held values go first, then a report on the bus wins as an update") {
	// The adapter alone: a read-on-start datapoint with a held command. The write goes out first, the
	// read after it; the bus's answer is an ordinary update and replaces the commanded value.
	hc_test::TunnelFixture t;
	hc::DatapointModel model{t.clock};
	hc::knx::KnxAdapter adapter{t.loop, t.clock, model, t.tunnel};
	hc::DatapointSpec specs[] = {
		{"valve", hc::ValueType::real,
			hc::DatapointFlags::kWritable | hc::DatapointFlags::kTransmit | hc::DatapointFlags::kReadOnStart},
	};
	REQUIRE(model.load(specs, 1) == DatapointStatus::ok);
	adapter.start();
	hc::knx::KnxMapping m;
	m.datapoint = 0;
	m.dpt = hc::Dpt{5, 1};
	m.has_send = true;
	m.send = hc::knx::GroupAddress::of(1, 1, 1);
	REQUIRE(adapter.configure(&m, 1));
	REQUIRE(model.command(0, Value::of_real(40.0)) == DatapointStatus::ok);
	REQUIRE(model.get(0)->pending());
	t.connect(7);
	REQUIRE(t.socket.count(hc::knx::ip::Service::tunneling_request) == 1);
	auto last_telegram = [&t]() {
		const Bytes& request = t.socket.last().data;
		return hc_test::hex(Bytes(request.begin() + 16, request.end()));
	};
	CHECK(last_telegram() == "09 01 02 00 80 66"); // the write of 40 % to 1/1/1
	CHECK_FALSE(model.get(0)->pending());
	// The gateway confirms it; then the read on start goes out.
	uint8_t gateway_sequence = 0;
	auto confirm_last = [&]() {
		const Bytes& request = t.socket.last().data;
		t.socket.inject(hc_test::tunneling_ack(7, request[8]));
		Bytes cemi(request.begin() + 10, request.end());
		cemi[0] = 0x2E;
		t.socket.inject(hc_test::tunneling_request(7, gateway_sequence++, cemi));
	};
	confirm_last();
	t.run_for(100);
	REQUIRE(t.socket.count(hc::knx::ip::Service::tunneling_request) == 2);
	CHECK(last_telegram() == "09 01 01 00 00"); // GroupValueRead 1/1/1
	confirm_last();
	// The actuator answers with what it actually does: the bus wins, as an ordinary report.
	Bytes cemi = hc_test::bytes("29 00 BC D0 11 0A 09 01 02 00 40 80");
	t.socket.inject(hc_test::tunneling_request(7, gateway_sequence++, cemi));
	CHECK(model.get(0)->value().as_real() == doctest::Approx(50.2).epsilon(0.01));
	CHECK(model.get(0)->quality() == Quality::good);
	CHECK_FALSE(model.get(0)->pending());
	CHECK(t.socket.count(hc::knx::ip::Service::tunneling_request) == 2); // nothing sent again
}

namespace {

/// A technology that holds every command (the model's side of the pending mark).
struct Holder : hc::Technology {
	hc::DatapointModel* model = nullptr;
	const char* name() const override { return "holder"; }
	bool holds_commands() const override { return true; }
	bool command(const hc::Datapoint& datapoint, const Value&) override {
		return model->set_pending(*this, datapoint.id(), true) == DatapointStatus::ok;
	}
};

struct Plain : hc::Technology {
	const char* name() const override { return "plain"; }
	bool command(const hc::Datapoint&, const Value&) override { return true; }
};

struct PendingListener : hc::DatapointListener {
	int updates = 0;
	int pending = 0;
	void datapoint_updated(const hc::DatapointUpdate&) override { ++updates; }
	void datapoint_pending(const hc::Datapoint&) override { ++pending; }
};

} // namespace

TEST_CASE("KnxHold: the model's pending mark: owner only, cleared by a report, kept by name across a load") {
	hc_test::ScopedCapture capture;
	hc_test::FakeClock clock;
	hc::DatapointModel model{clock};
	Holder holder;
	holder.model = &model;
	Plain plain;
	PendingListener listener;
	REQUIRE(model.add_listener(listener) == DatapointStatus::ok);
	uint8_t flags = hc::DatapointFlags::kWritable | hc::DatapointFlags::kTransmit;
	hc::DatapointSpec specs[] = {{"a", hc::ValueType::boolean, flags}, {"b", hc::ValueType::boolean, flags}};
	REQUIRE(model.load(specs, 2) == DatapointStatus::ok);
	REQUIRE(model.bind(0, holder) == DatapointStatus::ok);
	REQUIRE(model.bind(1, holder) == DatapointStatus::ok);
	CHECK(model.set_pending(plain, 0, true) == DatapointStatus::not_owner);
	REQUIRE(model.command(0, Value::of_bool(true)) == DatapointStatus::ok);
	CHECK(model.get(0)->pending());
	CHECK(listener.updates == 1);
	CHECK(listener.pending == 0); // told with the update
	REQUIRE(model.set_pending(holder, 0, false) == DatapointStatus::ok);
	CHECK(listener.pending == 1);
	REQUIRE(model.command(0, Value::of_bool(false)) == DatapointStatus::ok);
	REQUIRE(model.report(holder, 0, Value::of_bool(true)) == DatapointStatus::ok);
	CHECK_FALSE(model.get(0)->pending()); // the bus's value is newer
	REQUIRE(model.command(0, Value::of_bool(false)) == DatapointStatus::ok);
	REQUIRE(model.command(1, Value::of_bool(true)) == DatapointStatus::ok);
	// A load keeps the mark by name; bind() drops it for an owner that does not hold commands, and a
	// datapoint left without an owner is never pending.
	hc::DatapointSpec next[] = {{"c", hc::ValueType::boolean, flags}, {"b", hc::ValueType::boolean, flags},
		{"a", hc::ValueType::boolean, flags}};
	REQUIRE(model.load(next, 3, true) == DatapointStatus::ok);
	CHECK_FALSE(model.find("a")->pending()); // no owner yet
	REQUIRE(model.bind(model.find("a")->id(), holder) == DatapointStatus::ok);
	CHECK(model.find("a")->pending());
	CHECK(model.find("a")->value() == Value::of_bool(false));
	REQUIRE(model.bind(model.find("b")->id(), plain) == DatapointStatus::ok);
	CHECK_FALSE(model.find("b")->pending());
	REQUIRE(model.bind(model.find("b")->id(), plain) == DatapointStatus::ok);
	CHECK_FALSE(model.find("c")->pending());
	REQUIRE(model.remove_listener(listener));
}
