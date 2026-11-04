/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The monitor stream (issue #19, spec/api.md "Monitor"): the ring and its sequence numbers, the JSON
// of every event kind, the WebSocket subscription with batching, gap and catch-up, the KNX sending
// from the monitor and GET /api/monitor.

#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/image/image_service.h"
#include "hc/knx/adapter.h"
#include "hc/knx/telegram_log.h"
#include "hc/knx/tunnel.h"
#include "hc/live_feed.h"
#include "hc/monitor_feed.h"
#include "knx_test_support.h"
#include "web_test_support.h"
#include "zigbee_test_support.h"

using hc::DatapointSpec;
using hc::DatapointStatus;
using hc::LiveFeed;
using hc::MonitorEvent;
using hc::MonitorFeed;
using hc::Value;
using hc::ValueType;
using hc_test::Bytes;
using hc_test::Json;

namespace {

Json parse_json(const std::string& text) {
	Json json;
	std::string error;
	REQUIRE_MESSAGE(hc_test::JsonReader(text).parse(json, error), error << ": " << text);
	return json;
}

/// The events of `feed` from `since` as JSON objects (one REST-like read into a large buffer).
std::vector<Json> events_of(const MonitorFeed& feed, uint32_t since = 0) {
	static char buffer[32768];
	hc::JsonWriter json(buffer, sizeof(buffer));
	json.begin_array();
	feed.write_events(json, since, MonitorFeed::kCapacity);
	json.end_array();
	REQUIRE_FALSE(json.overflowed());
	return parse_json(std::string(buffer, json.length())).items;
}

/// A model of plain writable datapoints (no technology) and a monitor on it.
struct RingFixture {
	hc_test::FakeClock clock;
	hc::DatapointModel model{clock};
	MonitorFeed monitor{clock, model};

	RingFixture() {
		DatapointSpec specs[] = {
			{"a", ValueType::boolean, hc::DatapointFlags::kWritable},
			{"b", ValueType::real, hc::DatapointFlags::kWritable},
		};
		REQUIRE(model.load(specs, 2) == DatapointStatus::ok);
		REQUIRE(monitor.start());
	}
};

const char* const kImage = R"({"image": {"schemaVersion": "1.0", "projectId": "monitor-test", "revision": 3},
 "datapoints": [
  {"id": "hall.presence", "type": "boolean", "knx": {"dpt": "1.001", "listen": ["1/0/5"]}},
  {"id": "hall.light", "type": "boolean", "writable": true, "transmit": true, "knx": {"dpt": "1.001", "send": "1/0/1"}},
  {"id": "hall.temperature", "type": "real", "knx": {"dpt": "9.001", "listen": ["1/2/3"]}},
  {"id": "hall.scene", "type": "real", "writable": true}
 ],
 "rules": [{"id": "presence-light", "name": "Presence light", "reactions": [{"triggers": [{"kind": "datapoint", "datapoint": "hall.presence"}],
   "actions": [{"kind": "write", "datapoint": "hall.light", "value": {"kind": "datapoint", "name": "hall.presence"}}]}]}]})";

/// The web API with the live feed over an image service with a KNX tunnel (to the test gateway's
/// documentation address) and its telegram log.
struct MonitorFixture : hc_test::WebFixture {
	hc_test::FakeUdpSocket socket{fake.clock};
	hc::knx::TelegramLog telegrams{fake.clock};
	hc::knx::KnxTunnel tunnel{controller.loop(), fake.clock, controller.connectivity(), socket};
	hc::knx::KnxAdapter adapter{controller.loop(), fake.clock, model, tunnel, &telegrams};
	hc_test::FakeBlobStore blobs;
	hc::image::ImageService image{controller, model, blobs, adapter, &tunnel};
	hc_test::FakeTransport transport;
	LiveFeed feed{api, controller.loop(), transport};
	std::string cookie;
	size_t handled = 0;
	uint8_t gateway_sequence = 0;

	MonitorFixture() {
		controller.start();
		controller.connectivity().link_up("192.0.2.10");
		adapter.start();
		image.start();
		api.set_image(&image);
		REQUIRE(feed.start());
		cookie = onboard();
		std::string text = kImage;
		bool loaded = image.load(text.data(), text.size(), hc::image::Source::api, true);
		std::string why;
		for (size_t i = 0; i < image.report().count; ++i) {
			why += std::string(image.report().refusals[i].path) + ": " + image.report().refusals[i].reason + "\n";
		}
		REQUIRE_MESSAGE(loaded, why);
		run();
	}

	void run() {
		for (int i = 0; i < 4; ++i) {
			controller.run_once();
		}
	}

	void send(uint32_t client, const std::string& text) { feed.message(client, text.data(), text.size()); }

	uint16_t id(const char* name) const { return model.find(name)->id(); }

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

	/// Acknowledges and confirms every TUNNELING_REQUEST sent.
	void pump() {
		for (int round = 0; round < 100; ++round) {
			controller.run_once();
			if (handled == socket.sent.size()) {
				return;
			}
			while (handled < socket.sent.size()) {
				Bytes request = socket.sent[handled++].data;
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

	/// The TUNNELING_REQUESTs the device sent: "dest len tpdu".
	std::vector<std::string> sent_telegrams() const {
		std::vector<std::string> out;
		for (const auto& d : socket.sent) {
			if (d.data.size() >= 16 && d.data[2] == 0x04 && d.data[3] == 0x20) {
				out.push_back(hc_test::hex(Bytes(d.data.begin() + 16, d.data.end())));
			}
		}
		return out;
	}

	/// A group telegram from 1.1.10 to `destination` with `tpdu`.
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

	/// The events of all monitor batches sent to `client`.
	std::vector<Json> streamed(uint32_t client) const {
		std::vector<Json> out;
		for (const Json& m : transport.messages(client, "monitor")) {
			for (const Json& e : m["events"].items) {
				out.push_back(e);
			}
		}
		return out;
	}

	/// The monitor batches (not the answers) sent to `client`.
	std::vector<Json> batches(uint32_t client) const {
		std::vector<Json> out;
		for (const Json& m : transport.messages(client, "monitor")) {
			if (m.has("events")) {
				out.push_back(m);
			}
		}
		return out;
	}
};

} // namespace

TEST_CASE("MonitorFeed: the ring numbers events and overwrites the oldest") {
	RingFixture f;
	CHECK(f.monitor.next() == 0);
	CHECK(f.monitor.first() == 0);
	CHECK(f.monitor.find(0) == nullptr);
	for (size_t i = 0; i < MonitorFeed::kCapacity + 5; ++i) {
		REQUIRE(f.model.command(1, Value::of_real(static_cast<double>(i))) == DatapointStatus::ok);
	}
	CHECK(f.monitor.size() == MonitorFeed::kCapacity);
	CHECK(f.monitor.next() == MonitorFeed::kCapacity + 5);
	CHECK(f.monitor.first() == 5);
	CHECK(f.monitor.find(4) == nullptr);
	REQUIRE(f.monitor.find(5) != nullptr);
	CHECK(f.monitor.find(5)->value == Value::of_real(5.0));
	CHECK(f.monitor.find(MonitorFeed::kCapacity + 4)->value == Value::of_real(MonitorFeed::kCapacity + 4.0));
	CHECK(f.monitor.find(MonitorFeed::kCapacity + 5) == nullptr);

	bool reset = false;
	CHECK(f.monitor.start_of(7, reset) == 7);
	CHECK_FALSE(reset);
	CHECK(f.monitor.start_of(f.monitor.next(), reset) == f.monitor.next());
	CHECK_FALSE(reset);
	CHECK(f.monitor.start_of(100000, reset) == 5); // a sequence of an earlier run
	CHECK(reset);

	// Reading from an overwritten sequence starts with a gap marker.
	std::vector<Json> events = events_of(f.monitor, 2);
	REQUIRE(events.size() == MonitorFeed::kCapacity + 1);
	CHECK(events[0]["kind"].text == "gap");
	CHECK(events[0]["seq"].number == 2);
	CHECK(events[0]["count"].number == 3);
	CHECK(events[1]["seq"].number == 5);
	CHECK(events[1]["value"].number == 5);
	CHECK(events.back()["seq"].number == MonitorFeed::kCapacity + 4);
	// From the present: nothing.
	CHECK(events_of(f.monitor, f.monitor.next()).empty());
}

TEST_CASE("MonitorFeed: the compact event keeps what the JSON shows (issue #45)") {
	RingFixture f;
	// Beyond 32 bits of milliseconds (50 days of uptime) and a wall time beyond int32 (after 2038).
	f.clock.now_ms = (uint64_t{1} << 40) + 12345;
	f.clock.wall_seconds = 4102444800; // 2100-01-01
	f.clock.synchronized = true;
	REQUIRE(f.model.command(1, Value::of_real(-0.5)) == DatapointStatus::ok);

	hc::zigbee::ZigbeeTraffic traffic;
	traffic.kind = hc::zigbee::ZigbeeTraffic::Kind::report;
	traffic.ieee = 0xFFEEDDCCBBAA9988ULL;
	traffic.short_addr = 0xFFF7;
	traffic.endpoint = 242;
	traffic.cluster = 0xFC00;
	traffic.code = 0xFFFE;
	Value number = Value::of_unsigned(4000000000u);
	traffic.value = &number;
	f.monitor.zigbee_traffic(traffic);

	// A rule id of the longest form (32 characters) is cut to 27 bytes, as the contract says.
	f.monitor.rule_read("abcdefghijklmnopqrstuvwxyz-_0123", *f.model.find("a"));

	std::vector<Json> events = events_of(f.monitor);
	REQUIRE(events.size() == 3);
	for (const Json& e : events) {
		CHECK(e["ms"].number == static_cast<double>((uint64_t{1} << 40) + 12345));
		CHECK(e["t"].number == 4102444800.0);
	}
	CHECK(events[0]["value"].number == -0.5);
	CHECK(events[0]["name"].text == "b");
	CHECK(events[1]["ieee"].text == "ffeeddccbbaa9988");
	CHECK(events[1]["short"].number == 0xFFF7);
	CHECK(events[1]["ep"].number == 242);
	CHECK(events[1]["cluster"].number == 0xFC00);
	CHECK(events[1]["attr"].number == 0xFFFE);
	CHECK(events[1]["value"].number == 4000000000.0);
	CHECK(events[2]["rule"].text == "abcdefghijklmnopqrstuvwxyz-");
	CHECK(events[2]["action"].text == "read");
	CHECK(events[2]["name"].text == "a");

	// Without a synchronized clock no event has `t`.
	f.clock.synchronized = false;
	REQUIRE(f.model.command(0, Value::of_bool(true)) == DatapointStatus::ok);
	CHECK_FALSE(events_of(f.monitor, 3)[0].has("t"));
}

TEST_CASE("MonitorFeed: the event's 8-bit generation is exact over hundreds of model changes (issue #45)") {
	RingFixture f;
	// Every command is followed by a model change: no command event the ring holds refers to the
	// current model, also when the generation has wrapped around 256 (more than twice the capacity).
	for (int i = 0; i < 600; ++i) {
		REQUIRE(f.model.command(0, Value::of_bool((i & 1) != 0)) == DatapointStatus::ok);
		f.monitor.model_changed(nullptr);
		if (i == 126 || i == 255 || i == 256 || i == 599) {
			for (const Json& e : events_of(f.monitor, f.monitor.first())) {
				CHECK_FALSE(e.has("dp"));
			}
		}
	}
	CHECK(f.monitor.generation() == 600);
	REQUIRE(f.model.command(0, Value::of_bool(true)) == DatapointStatus::ok);
	std::vector<Json> events = events_of(f.monitor, f.monitor.first());
	REQUIRE(events.back()["kind"].text == "command");
	CHECK(events.back()["dp"].number == 0);
	CHECK(events.back()["name"].text == "a");

	// The oldest event a full ring holds is kCapacity - 1 model changes old: still not current.
	RingFixture g;
	REQUIRE(g.model.command(1, Value::of_real(1.0)) == DatapointStatus::ok);
	for (size_t i = 0; i + 1 < MonitorFeed::kCapacity; ++i) {
		g.monitor.model_changed(nullptr);
	}
	REQUIRE(g.monitor.first() == 0);
	std::vector<Json> held = events_of(g.monitor, 0);
	REQUIRE(held.size() == MonitorFeed::kCapacity);
	CHECK(held[0]["kind"].text == "command");
	CHECK_FALSE(held[0].has("dp"));
	CHECK(held[0]["value"].number == 1);
}

TEST_CASE("MonitorFeed: command events carry the declared origin, names follow the model generation") {
	RingFixture f;
	{
		hc::MonitorOrigin origin(&f.monitor, MonitorEvent::CommandOrigin::ws, MonitorEvent::Via::token);
		REQUIRE(f.model.command(0, Value::of_bool(true)) == DatapointStatus::ok);
	}
	{
		hc::MonitorOrigin origin(&f.monitor, MonitorEvent::CommandOrigin::api);
		REQUIRE(f.model.command(1, Value::of_real(2.5)) == DatapointStatus::ok);
	}
	REQUIRE(f.model.command(0, Value::of_bool(false)) == DatapointStatus::ok);
	std::vector<Json> events = events_of(f.monitor);
	REQUIRE(events.size() == 3);
	CHECK(events[0]["kind"].text == "command");
	CHECK(events[0]["origin"].text == "ws");
	CHECK(events[0]["via"].text == "token");
	CHECK(events[0]["dp"].number == 0);
	CHECK(events[0]["name"].text == "a");
	CHECK(events[0]["value"].boolean == true);
	CHECK_FALSE(events[0].has("t")); // the fake wall clock is not synchronized
	CHECK(events[1]["origin"].text == "api");
	CHECK(events[1]["via"].text == "session");
	CHECK(events[1]["value"].number == 2.5);
	CHECK(events[2]["origin"].text == "app");
	CHECK_FALSE(events[2].has("via"));

	// After a model change the ids may mean other datapoints: the name and id are left out.
	f.monitor.model_changed(nullptr);
	events = events_of(f.monitor);
	REQUIRE(events.size() == 4);
	CHECK_FALSE(events[0].has("dp"));
	CHECK_FALSE(events[0].has("name"));
	CHECK(events[0]["value"].boolean == true);
	CHECK(events[3]["kind"].text == "image");
	CHECK(events[3]["event"].text == "reload");
	CHECK_FALSE(events[3].has("project"));
}

TEST_CASE("MonitorFeed: Zigbee reports, actions and commands of a real adapter") {
	hc_test::ZigbeeFixture z;
	MonitorFeed monitor{z.clock, z.model};
	REQUIRE(monitor.start());
	monitor.attach_zigbee(&z.adapter);
	CHECK(z.adapter.traffic_listener() == &monitor);
	z.start_network();
	z.join_bulb();
	z.join_button();
	uint32_t from = monitor.next();
	z.report(hc_test::kBulbShort, 11, hc::zigbee::cluster::kOnOff, hc::zigbee::attribute::kOnOff,
		hc::zigbee::zcl_type::kBool, 1);
	z.adapter.command_received(hc_test::ZigbeeFixture::by_short(hc_test::kButtonShort), 1, hc::zigbee::cluster::kOnOff,
		0x02, nullptr, 0);
	const hc::Datapoint* level = z.dp("zigbee.00124b0001020304.11.level");
	REQUIRE(level != nullptr);
	REQUIRE(z.model.command(level->id(), Value::of_real(50.0)) == DatapointStatus::ok);
	// An attribute no function maps: its own value.
	z.report(hc_test::kBulbShort, 11, 0x0006, 0x4003, hc::zigbee::zcl_type::kUint8, 7);

	std::vector<Json> events = events_of(monitor, from);
	REQUIRE(events.size() == 5);
	CHECK(events[0]["kind"].text == "zigbee");
	CHECK(events[0]["event"].text == "report");
	CHECK(events[0]["ieee"].text == "00124b0001020304");
	CHECK(events[0]["short"].number == hc_test::kBulbShort);
	CHECK(events[0]["ep"].number == 11);
	CHECK(events[0]["cluster"].number == 6);
	CHECK(events[0]["attr"].number == 0);
	CHECK(events[0]["name"].text == "zigbee.00124b0001020304.11.on");
	CHECK(events[0]["value"].boolean == true);

	CHECK(events[1]["event"].text == "action");
	CHECK(events[1]["ieee"].text == "000b57fffe112233");
	CHECK(events[1]["cmd"].number == 2);
	CHECK(events[1]["name"].text.find(".action") != std::string::npos);
	CHECK(events[1].has("value"));

	// The technology sends first, then the model announces the command.
	CHECK(events[2]["event"].text == "command");
	CHECK(events[2]["cluster"].number == 8);
	CHECK(events[2]["cmd"].number == 4);
	CHECK(events[2]["name"].text == "zigbee.00124b0001020304.11.level");
	CHECK(events[2]["value"].number == 50);
	CHECK(events[3]["kind"].text == "command");
	CHECK(events[3]["origin"].text == "app");
	CHECK(events[3]["dp"].number == level->id());

	CHECK(events[4]["attr"].number == 0x4003);
	CHECK_FALSE(events[4].has("dp"));
	CHECK(events[4]["value"].number == 7);

	monitor.attach_zigbee(nullptr);
	CHECK(z.adapter.traffic_listener() == nullptr);
}

TEST_CASE("MonitorFeed: KNX telegrams with decoded values, the rule's action and the image events") {
	MonitorFixture f;
	f.connect();
	CHECK(f.telegrams.listener() == &f.feed.monitor());
	std::vector<Json> events = events_of(f.feed.monitor());
	REQUIRE_FALSE(events.empty());
	CHECK(events.back()["kind"].text == "image");
	CHECK(events.back()["event"].text == "load");
	CHECK(events.back()["project"].text == "monitor-test");
	CHECK(events.back()["revision"].number == 3);
	CHECK(events.back()["source"].text == "api");

	uint32_t from = f.feed.monitor().next();
	f.receive(hc::knx::GroupAddress::of(1, 0, 5).raw, "00 81"); // presence: on
	f.pump();
	f.receive(hc::knx::GroupAddress::of(1, 2, 3).raw, "00 80 0C 33"); // 21.5 degrees
	f.receive(hc::knx::GroupAddress::of(3, 3, 3).raw, "00 80 0C 1A"); // nobody listens
	events = events_of(f.feed.monitor(), from);
	REQUIRE(events.size() == 5);

	CHECK(events[0]["kind"].text == "knx");
	CHECK(events[0]["dir"].text == "rx");
	CHECK(events[0]["src"].text == "1.1.10");
	CHECK(events[0]["dst"].text == "1/0/5");
	CHECK(events[0]["service"].text == "write");
	CHECK(events[0]["data"].text == "01");
	CHECK(events[0]["short"].boolean == true);
	CHECK(events[0]["prio"].number == 3);
	CHECK(events[0]["hops"].number == 5);
	CHECK(events[0]["name"].text == "hall.presence");
	CHECK(events[0]["dp"].number == f.id("hall.presence"));
	CHECK(events[0]["value"].boolean == true);
	CHECK_FALSE(events[0].has("origin"));

	CHECK(events[1]["kind"].text == "rule");
	CHECK(events[1]["rule"].text == "presence-light");
	CHECK(events[1]["name"].text == "hall.light");
	CHECK(events[1]["value"].boolean == true);

	CHECK(events[2]["kind"].text == "knx");
	CHECK(events[2]["dir"].text == "tx");
	CHECK(events[2]["src"].text == "1.1.250");
	CHECK(events[2]["dst"].text == "1/0/1");
	CHECK(events[2]["name"].text == "hall.light");
	CHECK(events[2]["value"].boolean == true);
	CHECK_FALSE(events[2].has("origin"));

	CHECK(events[3]["dst"].text == "1/2/3");
	CHECK(events[3]["data"].text == "0C 33");
	CHECK_FALSE(events[3].has("short"));
	CHECK(events[3]["value"].number == 21.5);

	CHECK(events[4]["dst"].text == "3/3/3");
	CHECK_FALSE(events[4].has("dp"));
	CHECK_FALSE(events[4].has("value"));

	// A REST command and an unload.
	f.post("/api/datapoints/" + std::to_string(f.id("hall.scene")) + "/command", "{\"value\": 4}", f.cookie);
	events = events_of(f.feed.monitor(), f.feed.monitor().next() - 1);
	REQUIRE(events.size() == 1);
	CHECK(events[0]["kind"].text == "command");
	CHECK(events[0]["origin"].text == "api");
	CHECK(events[0]["via"].text == "session");
	CHECK(events[0]["value"].number == 4);
	const char* error = nullptr;
	REQUIRE(f.image.unload(&error));
	events = events_of(f.feed.monitor(), f.feed.monitor().next() - 1);
	CHECK(events[0]["event"].text == "unload");
	CHECK_FALSE(events[0].has("project"));
}

TEST_CASE("MonitorFeed: a subscribed client gets batches per loop turn, others get nothing") {
	MonitorFixture f;
	f.connect();
	REQUIRE(f.feed.open(1, f.cookie.c_str(), nullptr, nullptr));
	REQUIRE(f.feed.open(2, f.cookie.c_str(), nullptr, nullptr));
	f.send(1, "{\"type\":\"monitor\",\"subscribe\":true,\"ref\":5}");
	std::vector<Json> answers = f.transport.messages(1, "monitor");
	REQUIRE(answers.size() == 1);
	CHECK(answers[0]["subscribed"].boolean == true);
	CHECK(answers[0]["since"].number == f.feed.monitor().next());
	CHECK(answers[0]["next"].number == f.feed.monitor().next());
	CHECK(answers[0]["capacity"].number == MonitorFeed::kCapacity);
	CHECK(answers[0]["ref"].number == 5);
	f.run();
	CHECK(f.batches(1).empty()); // nothing new yet

	// Three commands in one turn: one batch.
	hc::MonitorFeed& monitor = f.feed.monitor();
	uint32_t from = monitor.next();
	f.model.command(f.id("hall.scene"), Value::of_real(1));
	f.model.command(f.id("hall.scene"), Value::of_real(2));
	f.model.command(f.id("hall.scene"), Value::of_real(3));
	CHECK(f.batches(1).empty());
	f.run();
	std::vector<Json> batches = f.batches(1);
	REQUIRE(batches.size() == 1);
	REQUIRE(batches[0]["events"].items.size() == 3);
	CHECK(batches[0]["events"].items[0]["seq"].number == from);
	CHECK(batches[0]["events"].items[2]["value"].number == 3);
	CHECK(batches[0]["next"].number == from + 3);
	CHECK(batches[0].has("now_ms"));
	CHECK(f.transport.messages(2, "monitor").empty());

	// A telegram and the controller's reaction reach the client in order.
	f.receive(hc::knx::GroupAddress::of(1, 0, 5).raw, "00 80");
	f.pump();
	f.run();
	std::vector<Json> events = f.streamed(1);
	REQUIRE(events.size() == 6);
	CHECK(events[3]["dst"].text == "1/0/5");
	CHECK(events[4]["kind"].text == "rule");
	CHECK(events[5]["dir"].text == "tx");
	CHECK(f.transport.messages(2, "monitor").empty());

	// Unsubscribe: nothing more.
	f.send(1, "{\"type\":\"monitor\",\"subscribe\":false}");
	CHECK(f.transport.messages(1, "monitor").back()["subscribed"].boolean == false);
	size_t sent = f.transport.sent.size();
	f.model.command(f.id("hall.scene"), Value::of_real(9));
	f.run();
	CHECK(f.transport.sent.size() == sent);
}

TEST_CASE("MonitorFeed: messages stay within 1400 bytes, catch-up with since, gap when behind") {
	MonitorFixture f;
	REQUIRE(f.feed.open(1, f.cookie.c_str(), nullptr, nullptr));
	hc::MonitorFeed& monitor = f.feed.monitor();
	uint32_t base = monitor.next();
	for (int i = 0; i < 40; ++i) {
		f.model.command(f.id("hall.scene"), Value::of_real(i));
	}
	f.run();
	// Catch-up from `base`: every event once, in order, in messages of at most 1400 bytes.
	f.send(1, "{\"type\":\"monitor\",\"subscribe\":true,\"since\":" + std::to_string(base) + "}");
	for (int i = 0; i < 6; ++i) {
		f.run();
	}
	std::vector<Json> events = f.streamed(1);
	REQUIRE(events.size() == 40);
	for (size_t i = 0; i < events.size(); ++i) {
		CHECK(events[i]["seq"].number == base + i);
	}
	CHECK(f.batches(1).size() > 1);
	for (const auto& s : f.transport.sent) {
		CHECK(s.text.size() <= LiveFeed::kMessageSize);
	}

	// The client's buffers are full while more than the ring holds happens: a gap marker, then the rest.
	f.transport.sent.clear();
	f.transport.refuse = true;
	uint32_t missed_from = monitor.next();
	for (size_t i = 0; i < MonitorFeed::kCapacity + 10; ++i) {
		f.model.command(f.id("hall.scene"), Value::of_real(static_cast<double>(i)));
		f.run();
	}
	CHECK(f.transport.refused > 0);
	f.transport.refuse = false;
	f.feed.writable();
	for (int i = 0; i < 20; ++i) {
		f.run();
	}
	events = f.streamed(1);
	REQUIRE(events.size() == MonitorFeed::kCapacity + 1);
	CHECK(events[0]["kind"].text == "gap");
	CHECK(events[0]["seq"].number == missed_from);
	CHECK(events[0]["count"].number == 10);
	CHECK(events[1]["seq"].number == missed_from + 10);
	CHECK(events.back()["seq"].number == monitor.next() - 1);
	// The datapoint feed was not resynchronized by the monitor's refused sends.
	CHECK(f.transport.messages(1, "datapoints").empty());

	// A since beyond the present (an earlier run) starts at the oldest event held.
	f.send(1, "{\"type\":\"monitor\",\"subscribe\":true,\"since\":4000000000}");
	Json answer = f.transport.messages(1, "monitor").back();
	CHECK(answer["reset"].boolean == true);
	CHECK(answer["since"].number == monitor.first());
	// Malformed.
	f.send(1, "{\"type\":\"monitor\",\"subscribe\":true,\"since\":-1}");
	CHECK(f.transport.messages(1, "error").back()["code"].text == "ws.bad_message");
	f.send(1, "{\"type\":\"monitor\"}");
	CHECK(f.transport.messages(1, "error").back()["code"].text == "ws.bad_message");
}

TEST_CASE("MonitorFeed: an unauthenticated client cannot subscribe") {
	MonitorFixture f;
	REQUIRE(f.feed.open(1, nullptr, nullptr, nullptr));
	f.send(1, "{\"type\":\"monitor\",\"subscribe\":true}");
	CHECK(f.transport.messages(1, "error")[0]["code"].text == "auth.required");
	CHECK(f.transport.messages(1, "monitor").empty());
}

TEST_CASE("MonitorFeed: sending and reading on KNX from the monitor, and the refusals") {
	MonitorFixture f;
	REQUIRE(f.feed.open(1, f.cookie.c_str(), nullptr, nullptr));
	auto result = [&]() { return f.transport.messages(1, "monitor").back(); };

	// The tunnel is down.
	f.send(1, R"({"type":"monitor","send":{"knx":{"address":"1/2/3","dpt":"9.001","value":21.5}},"ref":1})");
	CHECK(result()["result"].text == "send");
	CHECK(result()["ok"].boolean == false);
	CHECK(result()["code"].text == "monitor.tunnel_down");
	CHECK(result()["ref"].number == 1);
	f.send(1, R"({"type":"monitor","read":{"knx":{"address":"1/2/3"}}})");
	CHECK(result()["result"].text == "read");
	CHECK(result()["code"].text == "monitor.tunnel_down");

	f.connect();
	size_t before = f.sent_telegrams().size();
	f.send(1, R"({"type":"monitor","send":{"knx":{"address":"1/2/3","dpt":"9.001","value":21.5}},"ref":2})");
	CHECK(result()["ok"].boolean == true);
	CHECK_FALSE(result().has("code"));
	f.send(1, R"({"type":"monitor","send":{"knx":{"address":"1/0/7","service":"response","data":"01","short":true}}})");
	CHECK(result()["ok"].boolean == true);
	f.send(1, R"({"type":"monitor","read":{"knx":{"address":"1/0/9"}}})");
	CHECK(result()["ok"].boolean == true);
	uint32_t from = f.feed.monitor().next();
	f.pump();
	std::vector<std::string> sent = f.sent_telegrams();
	REQUIRE(sent.size() == before + 3);
	CHECK(sent[before] == "0A 03 03 00 80 0C 33");
	CHECK(sent[before + 1] == "08 07 01 00 41");
	CHECK(sent[before + 2] == "08 09 01 00 00");
	std::vector<Json> events = events_of(f.feed.monitor(), from);
	REQUIRE(events.size() == 3);
	CHECK(events[0]["dir"].text == "tx");
	CHECK(events[0]["dst"].text == "1/2/3");
	CHECK(events[0]["origin"].text == "monitor");
	CHECK(events[1]["service"].text == "response");
	CHECK(events[1]["origin"].text == "monitor");
	CHECK(events[2]["service"].text == "read");
	CHECK(events[2]["data"].text == "");
	CHECK(events[2]["origin"].text == "monitor");

	struct Case {
		const char* message;
		const char* code;
	};
	const Case cases[] = {
		{R"({"type":"monitor","send":{"knx":{"address":"1/2/3/4","dpt":"1.001","value":true}}})", "monitor.bad_address"},
		{R"({"type":"monitor","send":{"knx":{"dpt":"1.001","value":true}}})", "monitor.bad_address"},
		{R"({"type":"monitor","send":{"knx":{"address":"1/2/3","dpt":"x","value":true}}})", "monitor.bad_dpt"},
		{R"({"type":"monitor","send":{"knx":{"address":"1/2/3","dpt":"1.001"}}})", "monitor.value_missing"},
		{R"({"type":"monitor","send":{"knx":{"address":"1/2/3","dpt":"1.001","value":"on"}}})", "monitor.bad_value"},
		{R"({"type":"monitor","send":{"knx":{"address":"1/2/3","data":"zz"}}})", "monitor.bad_value"},
		{R"({"type":"monitor","send":{"knx":{"address":"1/2/3","data":"41","short":true}}})", "monitor.bad_value"},
		{R"({"type":"monitor","send":{"knx":{"address":"1/2/3","service":"read","data":"01"}}})", "monitor.bad_request"},
		{R"({"type":"monitor","send":{"knx":{"address":"1/2/3"}}})", "monitor.value_missing"},
		{R"({"type":"monitor","send":{"zigbee":{"ieee":"00124b0001020304"}}})", "monitor.unsupported"},
		{R"({"type":"monitor","send":{"bus":1}})", "monitor.bad_request"},
		{R"({"type":"monitor","read":{"knx":{"address":"x"}}})", "monitor.bad_address"},
	};
	for (const Case& c : cases) {
		CAPTURE(c.message);
		f.send(1, c.message);
		CHECK(result()["ok"].boolean == false);
		CHECK(result()["code"].text == c.code);
		CHECK_FALSE(result()["error"].text.empty());
	}
	f.pump();
	CHECK(f.sent_telegrams().size() == before + 3); // no refusal sent anything
}

TEST_CASE("MonitorFeed: without a KNX tunnel the monitor refuses to send") {
	RingFixture f;
	const char* code = nullptr;
	const char* reason = nullptr;
	const char* text = R"({"knx":{"address":"1/2/3","dpt":"1.001","value":true}})";
	hc::JsonSpan spec;
	REQUIRE(hc::json_parse(text, std::strlen(text), spec));
	CHECK_FALSE(f.monitor.send(spec, &code, &reason));
	CHECK(std::string(code) == "monitor.knx_unavailable");
	CHECK_FALSE(f.monitor.read(spec, &code, &reason));
	CHECK(std::string(code) == "monitor.knx_unavailable");
}

TEST_CASE("MonitorFeed: GET /api/monitor pages by sequence number") {
	MonitorFixture f;
	hc::MonitorFeed& monitor = f.feed.monitor();
	uint32_t base = monitor.next();
	for (int i = 0; i < 5; ++i) {
		f.model.command(f.id("hall.scene"), Value::of_real(i));
	}
	auto r = f.get("/api/monitor?since=" + std::to_string(base) + "&limit=2", f.cookie);
	REQUIRE(r.status == 200);
	CHECK(r.json["since"].number == base);
	CHECK(r.json["next"].number == base + 5);
	CHECK(r.json["first"].number == monitor.first());
	REQUIRE(r.json["events"].items.size() == 2);
	CHECK(r.json["events"].items[0]["seq"].number == base);
	CHECK(r.json["next_since"].number == base + 2);
	CHECK(r.json["more"].boolean == true);
	r = f.get("/api/monitor?since=" + std::to_string(base + 2) + "&limit=50", f.cookie);
	CHECK(r.json["events"].items.size() == 3);
	CHECK(r.json["more"].boolean == false);
	CHECK(r.json["next_since"].number == base + 5);
	r = f.get("/api/monitor", f.cookie); // everything held
	CHECK(r.json["events"].items.size() == monitor.size());
	r = f.get("/api/monitor?since=999999", f.cookie);
	CHECK(r.json["reset"].boolean == true);

	CHECK(f.get("/api/monitor?limit=0", f.cookie).json["code"].text == "monitor.bad_query");
	CHECK(f.get("/api/monitor?limit=129", f.cookie).json["code"].text == "monitor.bad_query");
	CHECK(f.get("/api/monitor?since=x", f.cookie).status == 400);
	CHECK(f.get("/api/monitor").status == 401);
	CHECK(f.post("/api/monitor", "{}", f.cookie).status == 405);
}

TEST_CASE("MonitorFeed: GET /api/monitor without a live feed") {
	hc_test::WebFixture f;
	std::string cookie = f.onboard();
	auto r = f.get("/api/monitor", cookie);
	CHECK(r.status == 404);
	CHECK(r.json["code"].text == "monitor.unavailable");
}

namespace {

void collect_line(void* context, const char* line) {
	static_cast<std::vector<std::string>*>(context)->push_back(line);
}

/// The line without its uptime ("12.345 rx ..." -> "rx ...").
std::string without_time(const std::string& line) {
	size_t space = line.find(' ');
	return space == std::string::npos ? line : line.substr(space + 1);
}

} // namespace

TEST_CASE("MonitorFeed: knx log is served from the monitor ring, KNX events only (issue #45 item 10)") {
	MonitorFixture f;
	f.connect();
	f.fake.clock.now_ms = 12345;
	f.receive(0x0A03, "00 80 0C 1A"); // hall.temperature, from 1.1.10
	f.receive(0x0A04, "00 80 0C 1A"); // 1/2/4: no datapoint
	// A command from the API (a command event, no telegram) and one that goes out on the bus.
	REQUIRE(f.model.command(f.id("hall.scene"), hc::Value::of_real(3)) == DatapointStatus::ok);
	REQUIRE(f.model.command(f.id("hall.light"), hc::Value::of_bool(true)) == DatapointStatus::ok);
	f.pump();
	CHECK(f.feed.monitor().count(MonitorEvent::Kind::command) >= 2);
	std::vector<std::string> lines;
	size_t printed = f.feed.monitor().write_knx_log(64, collect_line, &lines);
	REQUIRE(printed == 3);
	REQUIRE(lines.size() == 3);
	// The format of the former TelegramLog: uptime with milliseconds, direction, addresses, service,
	// the octets, the datapoint's name.
	CHECK(lines[0] == "12.345 rx 1.1.10 -> 1/2/3 write 0C 1A [hall.temperature]");
	CHECK(without_time(lines[1]) == "rx 1.1.10 -> 1/2/4 write 0C 1A");
	CHECK(without_time(lines[2]) == "tx 1.1.250 -> 1/0/1 write 01 [hall.light]");
	// The last n.
	lines.clear();
	CHECK(f.feed.monitor().write_knx_log(1, collect_line, &lines) == 1);
	CHECK(without_time(lines.at(0)) == "tx 1.1.250 -> 1/0/1 write 01 [hall.light]");
	// After a model change the ids mean other datapoints: the names are left out.
	const char* error = nullptr;
	REQUIRE(f.image.unload(&error));
	f.run();
	lines.clear();
	f.feed.monitor().write_knx_log(64, collect_line, &lines);
	REQUIRE(lines.size() == 3);
	CHECK(lines[0] == "12.345 rx 1.1.10 -> 1/2/3 write 0C 1A");
	// A repeated telegram says so.
	char line[MonitorFeed::kMaxKnxLine];
	MonitorEvent event;
	event.kind = MonitorEvent::Kind::knx;
	event.set_ms(5001);
	event.knx.source = hc::knx::IndividualAddress::of(1, 1, 5).raw;
	event.knx.destination = hc::knx::GroupAddress::of(1, 0, 1).raw;
	event.knx.service = static_cast<uint8_t>(hc::knx::GroupService::read);
	event.flags = MonitorEvent::kRepeated;
	CHECK(std::string(f.feed.monitor().format_knx(event, line, sizeof(line))) == "5.001 rx 1.1.5 -> 1/0/1 read (repeated)");
}
