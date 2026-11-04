/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "doctest.h"
#include "hc/demo.h"
#include "hc/live_feed.h"
#include "hc/value_json.h"
#include "hc/ws_send_ring.h"
#include "json_reader.h"
#include "web_test_support.h"

using hc::LiveFeed;
using hc::WsSendRing;
using hc_test::Json;
using hc_test::JsonReader;

namespace {

/// A message whose every byte follows from its sequence number and length: a reader can tell a
/// torn or mixed message from a whole one.
std::string pattern(uint32_t seq, size_t length) {
	std::string text(length, '\0');
	for (size_t i = 0; i < length; ++i) {
		text[i] = static_cast<char>('a' + (seq * 7 + i) % 26);
	}
	return text;
}

struct Lcg {
	uint32_t state = 12345;
	uint32_t next(uint32_t bound) {
		state = state * 1103515245u + 12345u;
		return (state >> 8) % bound;
	}
};

} // namespace

TEST_CASE("WsSendRing: whole messages in order, never torn, through many wraps") {
	alignas(4) uint8_t storage[4096];
	WsSendRing ring(storage, sizeof(storage));
	Lcg random;
	uint32_t written = 0;
	uint32_t read = 0;
	std::map<uint32_t, size_t> lengths;
	for (int round = 0; round < 5000; ++round) {
		// Write a few: claims formatted in place and pushed copies, of every length up to the largest.
		int writes = static_cast<int>(random.next(4));
		for (int w = 0; w < writes; ++w) {
			size_t length = random.next(4) == 0 ? WsSendRing::kMaxMessage : random.next(WsSendRing::kMaxMessage + 1);
			std::string text = pattern(written, length);
			uint32_t client = written;
			bool ok;
			if (random.next(2) == 0) {
				char* claim = ring.claim(WsSendRing::kMaxMessage, false);
				ok = claim != nullptr;
				if (ok) {
					std::memcpy(claim, text.data(), length);
					ring.commit(&client, 1, length);
				}
			} else {
				ok = ring.push(&client, 1, text.data(), length, false, false);
			}
			if (ok) {
				lengths[written] = length;
				++written;
			}
		}
		// Read a few.
		int reads = static_cast<int>(random.next(4));
		WsSendRing::Entry entry;
		for (int r = 0; r < reads && ring.front(entry); ++r) {
			REQUIRE(entry.count == 1);
			REQUIRE(entry.clients[0] == read);
			REQUIRE(entry.length == lengths[read]);
			REQUIRE(std::string(entry.data, entry.length) == pattern(read, entry.length));
			ring.pop();
			++read;
		}
	}
	WsSendRing::Entry entry;
	while (ring.front(entry)) {
		REQUIRE(entry.clients[0] == read);
		REQUIRE(std::string(entry.data, entry.length) == pattern(read, lengths[read]));
		ring.pop();
		++read;
	}
	CHECK(read == written);
	CHECK(written > 3000);
	CHECK(ring.free_bytes() == ring.capacity());
	CHECK(ring.lowest_free() < WsSendRing::cost(WsSendRing::kMaxMessage) * 2);
}

TEST_CASE("WsSendRing: a refused claim queues nothing; bulk keeps room for a single message") {
	alignas(4) uint8_t storage[4096];
	WsSendRing ring(storage, sizeof(storage));
	uint32_t client = 7;
	std::string big = pattern(1, WsSendRing::kMaxMessage);
	// Bulk takes one full message, then refuses: the reserve stays for single messages.
	CHECK(ring.push(&client, 1, big.data(), big.size(), false, true));
	CHECK_FALSE(ring.push(&client, 1, big.data(), big.size(), false, true));
	CHECK(ring.claim(WsSendRing::kMaxMessage, true) == nullptr);
	CHECK(ring.push(&client, 1, big.data(), big.size(), false, false));
	// Full now for a claim of the largest message, while a small copy still fits.
	CHECK(ring.claim(WsSendRing::kMaxMessage, false) == nullptr);
	std::string small = pattern(2, 100);
	CHECK(ring.push(&client, 1, small.data(), small.size(), false, false));
	// Exactly three messages are visible, each whole.
	WsSendRing::Entry entry;
	std::vector<std::string> seen;
	while (ring.front(entry)) {
		seen.push_back(std::string(entry.data, entry.length));
		ring.pop();
	}
	REQUIRE(seen.size() == 3);
	CHECK(seen[0] == big);
	CHECK(seen[1] == big);
	CHECK(seen[2] == small);
	// An abandoned claim leaves nothing behind.
	REQUIRE(ring.claim(WsSendRing::kMaxMessage, false) != nullptr);
	ring.abandon();
	CHECK_FALSE(ring.front(entry));
}

TEST_CASE("WsSendRing: one entry for a message to several clients, and close frames") {
	alignas(4) uint8_t storage[4096];
	WsSendRing ring(storage, sizeof(storage));
	uint32_t clients[] = {1, 2, 3, 4};
	char* claim = ring.claim(WsSendRing::kMaxMessage, false);
	REQUIRE(claim != nullptr);
	std::memcpy(claim, "{\"type\":\"status\"}", 17);
	ring.commit(clients, 4, 17);
	CHECK(ring.capacity() - ring.free_bytes() == WsSendRing::cost(17));
	uint8_t close[2] = {0x03, 0xE8};
	CHECK(ring.push(&clients[1], 1, reinterpret_cast<const char*>(close), 2, true, false));
	WsSendRing::Entry entry;
	REQUIRE(ring.front(entry));
	CHECK(entry.count == 4);
	CHECK(entry.clients[3] == 4);
	CHECK_FALSE(entry.close);
	CHECK(std::string(entry.data, entry.length) == "{\"type\":\"status\"}");
	ring.pop();
	REQUIRE(ring.front(entry));
	CHECK(entry.close);
	CHECK(entry.clients[0] == 2);
	ring.pop();
	CHECK_FALSE(ring.front(entry));
}

namespace {

/// The device's transport in miniature: the live feed writes into a WsSendRing of the device's
/// size, the test is the httpd task that drains it. A client in `slow` cannot be written (its
/// socket's send times out): the drain cuts it, as WebServer::drain() does.
struct RingTransport : hc::WsTransport {
	alignas(4) uint8_t storage[4096];
	WsSendRing ring{storage, sizeof(storage)};
	LiveFeed* feed = nullptr;
	bool starved = false;
	std::set<uint32_t> slow;
	std::set<uint32_t> gone;
	std::vector<uint32_t> cut;
	std::map<uint32_t, std::vector<std::string>> received;
	size_t claims_refused = 0;

	bool ws_send(uint32_t client, const char* text, size_t length) override {
		bool ok = ring.push(&client, 1, text, length, false, false);
		starved = starved || !ok;
		return ok;
	}
	bool ws_send_bulk(uint32_t client, const char* text, size_t length) override {
		bool ok = ring.push(&client, 1, text, length, false, true);
		starved = starved || !ok;
		return ok;
	}
	void ws_close(uint32_t client, uint16_t code, const char* reason) override {
		(void)code;
		(void)reason;
		ring.push(&client, 1, "", 0, true, false);
	}
	char* ws_claim(bool bulk) override {
		char* claim = ring.claim(LiveFeed::kMessageSize, bulk);
		if (claim == nullptr) {
			starved = true;
			++claims_refused;
		}
		return claim;
	}
	void ws_commit(const uint32_t* clients, size_t count, size_t length) override { ring.commit(clients, count, length); }
	void ws_abandon() override { ring.abandon(); }

	/// The httpd task's drain: everything queued goes out; then the closes and writable().
	void drain() {
		std::vector<uint32_t> closed;
		WsSendRing::Entry entry;
		while (ring.front(entry)) {
			for (size_t i = 0; i < entry.count; ++i) {
				uint32_t c = entry.clients[i];
				if (gone.count(c) != 0) {
					continue;
				}
				if (entry.close || slow.count(c) != 0) {
					gone.insert(c);
					closed.push_back(c);
					if (!entry.close) {
						cut.push_back(c);
					}
					continue;
				}
				received[c].push_back(std::string(entry.data, entry.length));
			}
			ring.pop();
		}
		for (uint32_t c : closed) {
			feed->closed(c);
		}
		if (starved) {
			starved = false;
			feed->writable();
		}
	}
};

struct RingFeedFixture : hc_test::WebFixture {
	RingTransport transport;
	LiveFeed feed{api, controller.loop(), transport};
	hc::DemoTechnology demo{model, controller.loop(), fake.clock};
	std::string cookie;

	RingFeedFixture() {
		cookie = onboard();
		transport.feed = &feed;
		REQUIRE(feed.start());
		REQUIRE(demo.load());
	}
	void turn() {
		fake.clock.advance(100);
		controller.loop().run_once(fake.clock.now_ms);
	}
	void send(uint32_t client, const std::string& text) { feed.message(client, text.data(), text.size()); }

	/// What the client knows when it applies its messages in order: snapshots from 0 replace it.
	std::map<int, Json> state(uint32_t client) {
		std::map<int, Json> values;
		for (const std::string& text : transport.received[client]) {
			Json json;
			std::string error;
			REQUIRE_MESSAGE(JsonReader(text).parse(json, error), "torn message: " << text);
			REQUIRE(text.size() <= LiveFeed::kMessageSize);
			if (json["type"].text != "datapoints") {
				continue;
			}
			if (json["snapshot"].boolean && json["from"].number == 0) {
				values.clear();
			}
			for (const Json& item : json["updates"].items) {
				values[static_cast<int>(item["id"].number)] = item["value"];
			}
		}
		return values;
	}
	Json model_value(uint16_t id) {
		char buffer[256];
		hc::JsonWriter json(buffer, sizeof(buffer));
		hc::write_value_json(json, model.get(id)->value());
		Json value;
		std::string error;
		REQUIRE(JsonReader(std::string(buffer, json.length())).parse(value, error));
		return value;
	}
};

} // namespace

TEST_CASE("LiveFeed over the send ring: a burst and a slow client; no torn message, the others converge") {
	RingFeedFixture f;
	for (uint32_t c = 1; c <= 4; ++c) {
		REQUIRE(f.feed.open(c, f.cookie.c_str(), nullptr, nullptr));
		f.send(c, "{\"type\":\"subscribe\"}");
	}
	for (int i = 0; i < 8; ++i) {
		f.turn();
		f.transport.drain();
	}
	// Client 3 stops reading; the server is stuck on it (the drain does not run) through a burst of
	// updates over 40 loop turns: the ring fills, the feed's claims are refused.
	f.transport.slow.insert(3);
	for (int i = 0; i < 40; ++i) {
		f.model.command(0, hc::Value::of_bool(i % 2 == 0));
		f.model.command(5, hc::Value::of_real(18.0 + i * 0.25));
		f.turn();
	}
	CHECK(f.transport.claims_refused > 0);
	// The server gets on: client 3 is cut (its send fails), the others resynchronize.
	for (int i = 0; i < 40; ++i) {
		f.transport.drain();
		f.turn();
	}
	f.transport.drain();
	CHECK(f.transport.cut == std::vector<uint32_t>{3});
	CHECK(f.feed.client_count() == 3);
	for (uint32_t c : {1u, 2u, 4u}) {
		auto values = f.state(c);
		REQUIRE(values.size() == f.model.size());
		for (uint16_t id = 0; id < f.model.size(); ++id) {
			Json expected = f.model_value(id);
			CHECK_MESSAGE(values[id].kind == expected.kind, "client " << c << " datapoint " << id);
			CHECK_MESSAGE(values[id].number == expected.number, "client " << c << " datapoint " << id);
			CHECK_MESSAGE(values[id].boolean == expected.boolean, "client " << c << " datapoint " << id);
		}
	}
	// Every message the cut client got before is whole too.
	f.state(3);
}

TEST_CASE("LiveFeed over the send ring: a status to four clients takes one entry") {
	RingFeedFixture f;
	for (uint32_t c = 1; c <= 4; ++c) {
		REQUIRE(f.feed.open(c, f.cookie.c_str(), nullptr, nullptr));
		f.send(c, "{\"type\":\"subscribe\"}");
		f.turn();
		f.transport.drain();
		f.turn();
		f.transport.drain();
	}
	size_t before = f.transport.ring.capacity() - f.transport.ring.free_bytes();
	REQUIRE(before == 0);
	f.fake.clock.advance(LiveFeed::kHeartbeatMs);
	f.turn(); // the heartbeat
	size_t used = f.transport.ring.capacity() - f.transport.ring.free_bytes();
	CHECK(used > 0);
	CHECK(used <= WsSendRing::cost(LiveFeed::kMessageSize));
	f.transport.drain();
	for (uint32_t c = 1; c <= 4; ++c) {
		CHECK(f.transport.received[c].back().find("\"type\":\"status\"") != std::string::npos);
	}
}

TEST_CASE("LiveFeed over the send ring: E-Home's messages do not depend on the browsers' ring") {
	// While the browsers' ring is full (a slow socket holds the server), a broadcast is formatted in
	// E-Home's own send queue and reaches it; the browsers resynchronize later.
	RingFeedFixture f;
	hc_test::FakeTransport ehome;
	REQUIRE(f.feed.open(1, f.cookie.c_str(), nullptr, nullptr));
	f.send(1, "{\"type\":\"subscribe\"}");
	f.feed.attach_ehome(ehome, 99);
	f.feed.ehome_message("{\"type\":\"subscribe\"}", 20);
	for (int i = 0; i < 6; ++i) {
		f.turn();
		f.transport.drain();
	}
	// Fill the browsers' ring without draining it.
	std::string filler(LiveFeed::kMessageSize - 10, 'x');
	uint32_t nobody = 0;
	while (f.transport.ring.push(&nobody, 1, filler.data(), filler.size(), false, false)) {
	}
	REQUIRE(f.transport.ring.claim(LiveFeed::kMessageSize, false) == nullptr);
	size_t before = ehome.messages(99, "status").size();
	f.fake.clock.advance(LiveFeed::kHeartbeatMs);
	f.turn();
	CHECK(ehome.messages(99, "status").size() == before + 1);
	// The browser converges once the ring drains.
	for (int i = 0; i < 10; ++i) {
		f.transport.drain();
		f.turn();
	}
	f.transport.drain();
	auto values = f.state(1);
	CHECK(values.size() == f.model.size());
}
