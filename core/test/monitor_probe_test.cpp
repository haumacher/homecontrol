/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe of the monitor feed (issue #19), written against spec/api.md without the implementation:
// the REST client that comes back with a cursor the ring has long overwritten, the sequence lookup
// at the ring's edges, and a command issued over the WebSocket itself.

#include "doctest.h"

#include "hc/live_feed.h"
#include "hc/monitor_feed.h"
#include "test_support.h"
#include "web_test_support.h"

using hc::Value;
using hc::MonitorFeed;

namespace {


struct ProbeFixture : hc_test::WebFixture {
	hc_test::FakeTransport transport;
	hc::LiveFeed feed{api, controller.loop(), transport};
	std::string cookie;

	ProbeFixture() {
		controller.start();
		REQUIRE(feed.start());
		cookie = onboard();
		// Without an image service the datapoints come from the model directly (no image event).
		hc::DatapointSpec specs[] = {{"hall.scene", hc::ValueType::real, hc::DatapointFlags::kWritable}};
		REQUIRE(model.load(specs, 1) == hc::DatapointStatus::ok);
		run();
	}

	void run() {
		for (int i = 0; i < 4; ++i) {
			controller.run_once();
		}
	}

	uint16_t scene() const { return model.find("hall.scene")->id(); }
};

} // namespace

TEST_CASE("Monitor probe: a REST cursor below the ring gets one gap marker, then a full page") {
	ProbeFixture f;
	hc::MonitorFeed& monitor = f.feed.monitor();
	uint32_t old_cursor = monitor.next();
	for (size_t i = 0; i < MonitorFeed::kCapacity + 20; ++i) {
		REQUIRE(f.model.command(f.scene(), Value::of_real(static_cast<double>(i))) == hc::DatapointStatus::ok);
	}
	REQUIRE(monitor.first() == old_cursor + 20);
	auto r = f.get("/api/monitor?since=" + std::to_string(old_cursor) + "&limit=10", f.cookie);
	REQUIRE(r.status == 200);
	CHECK(r.json["since"].number == old_cursor);
	CHECK_FALSE(r.json.has("reset"));
	REQUIRE(r.json["events"].items.size() == 11); // the marker does not use up the page
	CHECK(r.json["events"].items[0]["kind"].text == "gap");
	CHECK(r.json["events"].items[0]["seq"].number == old_cursor);
	CHECK(r.json["events"].items[0]["count"].number == 20);
	CHECK(r.json["events"].items[1]["seq"].number == monitor.first());
	CHECK(r.json["events"].items[1]["kind"].text == "command");
	CHECK(r.json["events"].items[1]["origin"].text == "app");
	CHECK(r.json["next_since"].number == monitor.first() + 10);
	CHECK(r.json["more"].boolean == true);

	// The sequence lookup at the edges.
	CHECK(monitor.find(monitor.first() - 1) == nullptr);
	CHECK(monitor.find(monitor.first()) != nullptr);
	CHECK(monitor.find(monitor.next() - 1) != nullptr);
	CHECK(monitor.find(monitor.next()) == nullptr);
	CHECK(monitor.size() == MonitorFeed::kCapacity);
}

TEST_CASE("Monitor probe: a command sent over the WebSocket is recorded with origin ws and via session") {
	ProbeFixture f;
	REQUIRE(f.feed.open(1, f.cookie.c_str(), nullptr, nullptr));
	hc::MonitorFeed& monitor = f.feed.monitor();
	uint32_t from = monitor.next();
	std::string subscribe = R"({"type":"monitor","subscribe":true})";
	f.feed.message(1, subscribe.data(), subscribe.size());
	std::string command = "{\"type\":\"command\",\"id\":" + std::to_string(f.scene()) + ",\"value\":7}";
	f.feed.message(1, command.data(), command.size());
	f.run();
	REQUIRE(monitor.next() == from + 1);
	const hc::MonitorEvent* event = monitor.find(from);
	REQUIRE(event != nullptr);
	CHECK(event->datapoint == f.scene());
	// The subscriber sees its own command in the stream.
	bool seen = false;
	for (const auto& m : f.transport.messages(1, "monitor")) {
		for (const auto& e : m["events"].items) {
			if (e["seq"].number == from) {
				seen = true;
				CHECK(e["kind"].text == "command");
				CHECK(e["origin"].text == "ws");
				CHECK(e["via"].text == "session");
				CHECK(e["name"].text == "hall.scene");
				CHECK(e["value"].number == 7);
			}
		}
	}
	CHECK(seen);
}

TEST_CASE("Monitor probe: a rule's read action is a rule event with action read and no value") {
	ProbeFixture f;
	hc::MonitorFeed& monitor = f.feed.monitor();
	uint32_t from = monitor.next();
	monitor.rule_read("temp-watch", *f.model.find("hall.scene"));
	REQUIRE(monitor.next() == from + 1);
	auto r = f.get("/api/monitor?since=" + std::to_string(from) + "&limit=1", f.cookie);
	REQUIRE(r.status == 200);
	const auto& events = r.json["events"].items;
	REQUIRE(events.size() == 1);
	CHECK(events[0]["kind"].text == "rule");
	CHECK(events[0]["rule"].text == "temp-watch");
	CHECK(events[0]["action"].text == "read");
	CHECK(events[0]["name"].text == "hall.scene");
	CHECK_FALSE(events[0].has("value"));
}
