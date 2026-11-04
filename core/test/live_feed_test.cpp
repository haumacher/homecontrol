/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <string>

#include "doctest.h"
#include "hc/demo.h"
#include "hc/live_feed.h"
#include "hc/websocket.h"
#include "web_test_support.h"

using hc::LiveFeed;
using hc_test::FakeTransport;
using hc_test::WebFixture;

namespace {

struct FeedFixture : WebFixture {
	FakeTransport transport;
	LiveFeed feed{api, controller.loop(), transport};
	hc::DemoTechnology demo{model, controller.loop(), fake.clock};
	std::string cookie;

	FeedFixture() {
		cookie = onboard();
		REQUIRE(feed.start());
	}

	void run() {
		controller.loop().run_once(fake.clock.now_ms);
		controller.loop().run_once(fake.clock.now_ms);
	}

	void advance(uint64_t ms) {
		uint64_t end = fake.clock.now_ms + ms;
		while (fake.clock.now_ms < end) {
			fake.clock.advance(100);
			controller.loop().run_once(fake.clock.now_ms);
		}
	}

	void send(uint32_t client, const std::string& text) { feed.message(client, text.data(), text.size()); }
};

} // namespace

TEST_CASE("LiveFeed authenticates by cookie and greets the client") {
	FeedFixture f;
	REQUIRE(f.feed.open(1, f.cookie.c_str(), "http://homecontrol-a1b2c3.local", "homecontrol-a1b2c3.local"));
	auto hello = f.transport.messages(1, "hello");
	REQUIRE(hello.size() == 1);
	CHECK(hello[0]["authenticated"].boolean == true);
	CHECK(f.feed.client_count() == 1);
	f.feed.closed(1);
	CHECK(f.feed.client_count() == 0);
}

TEST_CASE("LiveFeed refuses the fifth client and a foreign origin with a message") {
	FeedFixture f;
	for (uint32_t c = 1; c <= 4; ++c) {
		REQUIRE(f.feed.open(c, f.cookie.c_str(), nullptr, nullptr));
	}
	CHECK_FALSE(f.feed.open(5, f.cookie.c_str(), nullptr, nullptr));
	auto errors = f.transport.messages(5, "error");
	REQUIRE(errors.size() == 1);
	CHECK(errors[0]["code"].text == "ws.too_many_clients");
	REQUIRE(f.transport.closes.size() == 1);
	CHECK(f.transport.closes[0].client == 5);
	CHECK(f.transport.closes[0].code == hc::websocket::kCloseTryAgainLater);
	CHECK(f.feed.client_count() == 4);

	f.feed.closed(4);
	CHECK_FALSE(f.feed.open(6, f.cookie.c_str(), "http://evil.example", "homecontrol-a1b2c3.local:80"));
	CHECK(f.transport.messages(6, "error")[0]["code"].text == "ws.foreign_origin");
	CHECK(LiveFeed::same_origin("http://192.0.2.5:8080", "192.0.2.5:8081"));
	CHECK(LiveFeed::same_origin("https://Device.local", "device.local"));
	CHECK(LiveFeed::same_origin("http://[::1]:8080", "[::1]:8081"));
	CHECK_FALSE(LiveFeed::same_origin("http://device.local.evil", "device.local"));
	CHECK_FALSE(LiveFeed::same_origin("null", "device.local"));
}

TEST_CASE("LiveFeed without a cookie needs a token in the first message or times out") {
	FeedFixture f;
	char token[hc::Auth::kTokenText];
	REQUIRE(f.auth.create_token("script", token) == hc::AuthStatus::ok);
	REQUIRE(f.feed.open(1, nullptr, nullptr, nullptr));
	CHECK(f.transport.messages(1, "hello")[0]["authenticated"].boolean == false);
	f.send(1, std::string("{\"type\":\"auth\",\"token\":\"") + token + "\"}");
	CHECK(f.transport.messages(1, "auth")[0]["ok"].boolean == true);

	REQUIRE(f.feed.open(2, nullptr, nullptr, nullptr));
	f.send(2, "{\"type\":\"subscribe\"}");
	CHECK(f.transport.messages(2, "error")[0]["code"].text == "auth.required");
	CHECK(f.transport.closes.back().code == hc::websocket::kClosePolicy);

	REQUIRE(f.feed.open(3, nullptr, nullptr, nullptr));
	f.send(3, "{\"type\":\"auth\",\"token\":\"hct_00000000000000000000000000000000\"}");
	CHECK(f.transport.messages(3, "error")[0]["code"].text == "auth.token_invalid");

	REQUIRE(f.feed.open(4, nullptr, nullptr, nullptr));
	f.advance(LiveFeed::kAuthTimeoutMs + 1000);
	CHECK(f.transport.messages(4, "error")[0]["code"].text == "auth.timeout");
	// The token client is still there; revoking the token closes it.
	CHECK(f.feed.client_count() == 1);
	f.auth.revoke_token("script");
	f.advance(1000);
	CHECK(f.transport.messages(1, "error").back()["code"].text == "auth.session_ended");
	CHECK(f.feed.client_count() == 0);
}

TEST_CASE("LiveFeed sends a snapshot on subscribe, then batched updates per loop turn") {
	FeedFixture f;
	REQUIRE(f.demo.load());
	REQUIRE(f.feed.open(1, f.cookie.c_str(), nullptr, nullptr));
	f.send(1, "{\"type\":\"subscribe\"}");
	CHECK(f.transport.messages(1, "status").size() == 1);
	f.run();
	auto snapshot = f.transport.messages(1, "datapoints");
	// The demo's datapoints take two messages of kMessageSize (with the pending member, #72).
	REQUIRE(snapshot.size() == 2);
	size_t total = 0;
	for (size_t i = 0; i < snapshot.size(); ++i) {
		CHECK(snapshot[i]["snapshot"].boolean == true);
		CHECK(snapshot[i]["done"].boolean == (i + 1 == snapshot.size()));
		total += snapshot[i]["updates"].items.size();
	}
	CHECK(total == f.model.size());
	CHECK(snapshot[0]["updates"].items[0]["name"].text == "Living room light");
	CHECK(snapshot[0]["updates"].items[0]["pending"].boolean == false);

	f.transport.sent.clear();
	// Three updates in one turn, one datapoint twice: one message, two entries, latest value.
	f.model.command(0, hc::Value::of_bool(true));
	f.model.command(5, hc::Value::of_real(22.5));
	f.model.command(0, hc::Value::of_bool(false));
	CHECK(f.transport.messages(1, "datapoints").empty());
	f.run();
	auto updates = f.transport.messages(1, "datapoints");
	REQUIRE(updates.size() == 1);
	const auto& items = updates[0]["updates"].items;
	REQUIRE(items.size() == 2);
	CHECK(items[0]["id"].number == 0);
	CHECK(items[0]["value"].boolean == false);
	CHECK(items[1]["value"].number == 22.5);
	CHECK_FALSE(items[0].has("name"));
}

TEST_CASE("LiveFeed executes commands and answers with the result and the reason") {
	FeedFixture f;
	REQUIRE(f.demo.load());
	REQUIRE(f.feed.open(1, f.cookie.c_str(), nullptr, nullptr));
	f.send(1, "{\"type\":\"command\",\"id\":0,\"value\":true,\"ref\":17}");
	auto results = f.transport.messages(1, "result");
	REQUIRE(results.size() == 1);
	CHECK(results[0]["ok"].boolean == true);
	CHECK(results[0]["ref"].number == 17);
	CHECK(f.model.get(0)->value() == hc::Value::of_bool(true));
	f.send(1, "{\"type\":\"command\",\"id\":2,\"value\":1}");
	CHECK(f.transport.messages(1, "result").back()["code"].text == "dp.not_writable");
	f.send(1, "{\"type\":\"command\",\"id\":1,\"value\":150}");
	CHECK(f.transport.messages(1, "result").back()["code"].text == "dp.refused");
	f.send(1, "{\"type\":\"command\",\"id\":0,\"value\":\"on\"}");
	CHECK(f.transport.messages(1, "result").back()["code"].text == "dp.wrong_type");
	f.send(1, "{\"type\":\"command\",\"id\":999,\"value\":true}");
	CHECK(f.transport.messages(1, "result").back()["code"].text == "dp.unknown_id");
	f.send(1, "not json");
	CHECK(f.transport.messages(1, "error").back()["code"].text == "ws.bad_message");
	f.send(1, "{\"type\":\"dance\"}");
	CHECK(f.transport.messages(1, "error").back()["code"].text == "ws.unknown_type");
}

TEST_CASE("LiveFeed answers a ping with a pong, but only once authenticated") {
	FeedFixture f;
	REQUIRE(f.feed.open(1, f.cookie.c_str(), nullptr, nullptr));
	f.send(1, "{\"type\":\"ping\"}");
	CHECK(f.transport.messages(1, "pong").size() == 1);
	REQUIRE(f.feed.open(2, nullptr, nullptr, nullptr));
	f.send(2, "{\"type\":\"ping\"}");
	CHECK(f.transport.messages(2, "pong").empty());
	CHECK(f.feed.client_count() == 1);
}

TEST_CASE("LiveFeed pushes the status on change and as a heartbeat") {
	FeedFixture f;
	f.controller.start();
	REQUIRE(f.feed.open(1, f.cookie.c_str(), nullptr, nullptr));
	f.send(1, "{\"type\":\"subscribe\"}");
	f.advance(1500);
	size_t before = f.transport.messages(1, "status").size();
	f.advance(LiveFeed::kHeartbeatMs);
	size_t after = f.transport.messages(1, "status").size();
	CHECK(after >= before + 1);
	CHECK(after <= before + 2);
	f.host.mdns = false; // a discrete field changes
	f.advance(1000);
	auto last = f.transport.messages(1, "status").back();
	CHECK(last["network"]["mdns_announced"].boolean == false);
}

TEST_CASE("LiveFeed resynchronizes a client that missed a message") {
	FeedFixture f;
	REQUIRE(f.demo.load());
	REQUIRE(f.feed.open(1, f.cookie.c_str(), nullptr, nullptr));
	f.send(1, "{\"type\":\"subscribe\"}");
	f.run();
	f.transport.refuse = true;
	f.model.command(0, hc::Value::of_bool(true));
	f.run();
	CHECK(f.transport.refused > 0);
	f.transport.refuse = false;
	f.transport.sent.clear();
	f.feed.writable();
	f.run();
	auto snapshot = f.transport.messages(1, "datapoints");
	REQUIRE_FALSE(snapshot.empty());
	CHECK(snapshot[0]["snapshot"].boolean == true);
	CHECK(snapshot[0]["updates"].items[0]["value"].boolean == true);
}

TEST_CASE("LiveFeed logout ends the WebSocket of that session") {
	FeedFixture f;
	REQUIRE(f.feed.open(1, f.cookie.c_str(), nullptr, nullptr));
	f.post("/api/auth/logout", "{}", f.cookie);
	f.advance(1000);
	CHECK(f.transport.messages(1, "error").back()["code"].text == "auth.session_ended");
	CHECK(f.feed.client_count() == 0);
}
