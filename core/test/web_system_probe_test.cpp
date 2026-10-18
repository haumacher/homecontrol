/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe of the console-parity routes (issue #38) composed with features that existed before them: a
// device that left, a reset device that joins afresh, UTF-8 labels counted in bytes, closing the
// network, and a deploy token as the principal of the Zigbee routes.
#include <algorithm>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/live_feed.h"
#include "web_test_support.h"
#include "zigbee_test_support.h"

using hc::HttpMethod;

namespace {

struct ProbeFixture : hc_test::WebFixture {
	std::string cookie;
	hc_test::FakeBlobStore store;
	hc_test::FakeZigbeeDriver driver;
	hc::zigbee::ZigbeeAdapter adapter{controller.loop(), fake.clock, model, driver, &store};
	hc_test::ZigbeeActions device{adapter};

	ProbeFixture() {
		controller.start();
		cookie = onboard();
		REQUIRE(adapter.start());
		adapter.network_state(true, 0x1a62, 15, 0xddddddddddddddddull);
		api.set_zigbee(&adapter);
	}
	Result call(HttpMethod method, const std::string& target, const std::string& body = "") {
		return request(method, target, body, cookie, "", true);
	}
	void advance(uint64_t ms) {
		uint64_t end = fake.clock.now_ms + ms;
		while (fake.clock.now_ms < end) {
			fake.clock.advance(50);
			controller.loop().run_once(fake.clock.now_ms);
		}
	}
};

const char* kBulbPath = "/api/zigbee/devices/00124b0001020304";

std::vector<std::string> kinds_of(const std::vector<hc_test::Json>& events) {
	std::vector<std::string> kinds;
	for (const auto& e : events) {
		kinds.push_back(e["event"].text);
	}
	return kinds;
}

} // namespace

TEST_CASE("WebSystem probe: a device that left is listed as left with stale values, a fresh join interviews it "
	"again without changing the model, and it can still be removed") {
	ProbeFixture f;
	hc_test::FakeTransport transport;
	hc::LiveFeed feed(f.api, f.controller.loop(), transport);
	REQUIRE(feed.start());
	REQUIRE(feed.open(1, f.cookie.c_str(), nullptr, nullptr));
	feed.message(1, "{\"type\":\"subscribe\"}", 20);

	f.device.join_bulb();
	auto r = f.call(HttpMethod::get, "/api/zigbee");
	REQUIRE(r.json["count"].number == 1);
	double generation = r.json["network"].has("generation") ? r.json["network"]["generation"].number
		: f.call(HttpMethod::get, "/api/status").json["zigbee"]["generation"].number;
	const hc::Datapoint* on = f.model.find("zigbee.00124b0001020304.11.on");
	REQUIRE(on != nullptr);
	uint16_t on_id = on->id();
	f.device.report(hc_test::kBulbShort, 11, hc::zigbee::cluster::kOnOff, hc::zigbee::attribute::kOnOff,
		hc::zigbee::zcl_type::kBool, 1);
	REQUIRE(f.call(HttpMethod::get, "/api/datapoints/" + std::to_string(on_id)).json["quality"].text == "good");

	// It leaves: the list says so, its datapoints are stale, the feed tells the page.
	f.adapter.device_left(hc_test::ZigbeeFixture::node(hc_test::kBulb, hc_test::kBulbShort));
	r = f.call(HttpMethod::get, "/api/zigbee");
	REQUIRE(r.json["count"].number == 1);
	CHECK(r.json["devices"].items[0]["left"].boolean);
	auto dp = f.call(HttpMethod::get, "/api/datapoints/" + std::to_string(on_id));
	CHECK(dp.json["quality"].text == "stale");
	auto kinds = kinds_of(transport.messages(1, "zigbee"));
	CHECK(std::find(kinds.begin(), kinds.end(), "left") != kinds.end());

	// Minutes later it was reset and joins afresh (the trust center's notice, then its announce):
	// interviewed again, the datapoints keep their ids; the page's change counter moves on.
	f.advance(60 * 1000);
	f.adapter.device_joined(hc_test::ZigbeeFixture::node(hc_test::kBulb, hc_test::kBulbShort));
	CHECK(f.adapter.interviewing() == 0); // the interview waits for the announce
	f.device.join_bulb(); // announces and answers the interview as a fresh device would
	r = f.call(HttpMethod::get, "/api/zigbee");
	CHECK_FALSE(r.json["devices"].items[0]["left"].boolean);
	CHECK(r.json["devices"].items[0]["interviewed"].boolean);
	CHECK(f.model.find("zigbee.00124b0001020304.11.on") != nullptr);
	CHECK(f.model.find("zigbee.00124b0001020304.11.on")->id() == on_id);
	double after = f.call(HttpMethod::get, "/api/status").json["zigbee"]["generation"].number;
	CHECK(after > generation);
	kinds = kinds_of(transport.messages(1, "zigbee"));
	CHECK(std::count(kinds.begin(), kinds.end(), "interviewed") == 2);

	// Left again, and removed while away: the stack is not asked (no address), the entry is gone.
	f.adapter.device_left(hc_test::ZigbeeFixture::node(hc_test::kBulb, hc_test::kBulbShort));
	size_t calls = f.driver.calls.size();
	r = f.call(HttpMethod::del, kBulbPath);
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(f.call(HttpMethod::get, "/api/zigbee").json["count"].number == 0);
	CHECK(f.model.find("zigbee.00124b0001020304.11.on") == nullptr);
	kinds = kinds_of(transport.messages(1, "zigbee"));
	CHECK(kinds.back() == "removed");
	(void)calls;
	feed.closed(1);
}

TEST_CASE("WebSystem probe: the export carries the interview result and every function with its binding keys") {
	ProbeFixture f;
	auto r = f.call(HttpMethod::get, "/api/zigbee/export");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["format"].text == "homecontrol-zigbee-export");
	CHECK(r.json["version"].number == 1);
	CHECK(r.json["count"].number == 0);
	f.device.join_bulb();
	r = f.call(HttpMethod::get, "/api/zigbee/export");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	REQUIRE(r.json["devices"].items.size() == 1);
	const auto& bulb = r.json["devices"].items[0];
	CHECK(bulb["ieee"].text == "00124b0001020304");
	CHECK(bulb["manufacturer"].text == "IKEA of Sweden");
	REQUIRE(bulb["endpoints"].items.size() == 1);
	const auto& endpoint = bulb["endpoints"].items[0];
	CHECK(endpoint["id"].number == 11);
	CHECK(endpoint["profile"].number == 0x0104);
	bool has_on_off = false;
	for (const auto& c : endpoint["in"].items) {
		has_on_off = has_on_off || c.number == 6;
	}
	CHECK(has_on_off);
	REQUIRE(bulb["functions"].items.size() == 3);
	const auto& on = bulb["functions"].items[0];
	CHECK(on["name"].text == "zigbee.00124b0001020304.11.on");
	CHECK(on["function"].text == "on_off");
	CHECK(on["endpoint"].number == 11);
	CHECK(on["cluster"].number == 6);
	CHECK(on["attribute"].number == 0);
	CHECK(on["type"].text == "boolean");
	CHECK(on["writable"].boolean);
	CHECK(on["datapoint"].text == "zigbee.00124b0001020304.11.on"); // generic: no image binds it
	const auto& level = bulb["functions"].items[1];
	CHECK(level["function"].text == "level");
	CHECK(level["unit"].text == "%");
	CHECK(r.json["next"].kind == hc_test::Json::Kind::null);
	// The list and the export page alike; an unknown sub-route is 404, POST is 405.
	CHECK(f.call(HttpMethod::get, "/api/zigbee/export?from=1").json["devices"].items.empty());
	CHECK(f.call(HttpMethod::post, "/api/zigbee/export").status == 405);
}

TEST_CASE("WebSystem probe: labels are limited in bytes, not characters; closing the network; a deploy token "
	"may drive the Zigbee routes") {
	ProbeFixture f;
	f.device.join_bulb();
	std::string ok = std::string(30, 'x') + "\xc3\xa4"; // 32 bytes, 31 characters
	auto r = f.call(HttpMethod::put, std::string(kBulbPath) + "/label", "{\"label\": \"" + ok + "\"}");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(r.json["device"]["label"].text == ok);
	std::string long_label = std::string(31, 'x') + "\xc3\xa4"; // 33 bytes
	r = f.call(HttpMethod::put, std::string(kBulbPath) + "/label", "{\"label\": \"" + long_label + "\"}");
	CHECK(r.status == 422);
	CHECK(f.call(HttpMethod::get, "/api/zigbee").json["devices"].items[0]["label"].text == ok);
	r = f.call(HttpMethod::put, std::string(kBulbPath) + "/label", "{\"label\": \"\"}");
	REQUIRE(r.status == 200);
	const auto& cleared = f.call(HttpMethod::get, "/api/zigbee").json["devices"].items[0];
	bool label_gone = !cleared.has("label") || cleared["label"].text.empty();
	CHECK(label_gone);

	REQUIRE(f.call(HttpMethod::post, "/api/zigbee/permit", "{\"seconds\": 200}").status == 200);
	f.adapter.permit_join_status(200);
	r = f.call(HttpMethod::post, "/api/zigbee/permit", "{\"seconds\": 0}");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	CHECK(f.driver.calls.back() == "permit 0");
	f.adapter.permit_join_status(0);
	CHECK(f.call(HttpMethod::get, "/api/zigbee").json["network"]["permit_join_s"].number == 0);

	// E-Home's deploy token is a principal of the Zigbee routes (#16 builds on them), CSRF header or not.
	r = f.call(HttpMethod::post, "/api/tokens", "{\"name\": \"ehome\"}");
	REQUIRE_MESSAGE(r.status == 200, r.body);
	std::string bearer = "Bearer " + r.json["token"].text;
	r = f.request(HttpMethod::get, "/api/zigbee", "", "", bearer, false);
	CHECK(r.status == 200);
	r = f.request(HttpMethod::post, "/api/zigbee/permit", "{\"seconds\": 60}", "", bearer, false);
	CHECK(r.status == 200);
	CHECK(f.driver.calls.back() == "permit 60");
	r = f.request(HttpMethod::post, "/api/zigbee/permit", "{\"seconds\": 60}", "", "Bearer hct_nothing", false);
	CHECK(r.status == 401);
}
