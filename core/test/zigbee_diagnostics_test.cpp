/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Zigbee diagnostics (homecontrol #98): the stale timeout from the shortest reporting interval, the
// radio facts of the adapter view (lqi, rssi, parent, the child table's timeout and remaining time),
// and the info lines of joins, rejoins, authorizations, associations and child-table changes.

#include <string>
#include <vector>

#include "doctest.h"
#include "hc/zigbee/adapter.h"
#include "test_support.h"
#include "zigbee_test_support.h"

using hc::ZigbeeNeighbor;
using hc::zigbee::ZigbeeAdapter;
using hc_test::kBulb;
using hc_test::kBulbShort;
using hc_test::kSensor;
using hc_test::kSensorShort;
using hc_test::ZigbeeFixture;

namespace {

/// The Philips Hue motion sensor of homecontrol #98/#99 (a legacy HA 1.2 battery end device).
constexpr hc::zigbee::Ieee kSml002 = 0x0017880106498a49ull;
constexpr uint16_t kSml002Short = 0xaf9e;

void join_sml002(ZigbeeFixture& f) {
	f.adapter.device_announced(ZigbeeFixture::node(kSml002, kSml002Short), 0x80); // an end device
	uint8_t endpoints[] = {2};
	f.adapter.active_endpoints(ZigbeeFixture::by_short(kSml002Short), 0, endpoints, 1);
	f.descriptor(kSml002Short, 2, hc::zigbee::kHomeAutomationProfile, 0x0107,
		{0x0000, 0x0001, 0x0003, 0x0406, 0x0400, 0x0402}, {0x0019});
	f.basic(kSml002Short, 2, "Philips", "SML002");
	REQUIRE(f.adapter.interviewing() == 0);
	REQUIRE(f.adapter.set_label(kSml002, "Bewegungsmelder Hof"));
}

std::string view_of(const ZigbeeAdapter& adapter, hc::zigbee::Ieee ieee) {
	for (size_t i = 0; i < adapter.devices().size(); ++i) {
		if (adapter.devices().at(i).ieee == ieee) {
			char buffer[4096];
			hc::JsonWriter json(buffer, sizeof(buffer));
			adapter.write_device_json(json, i);
			REQUIRE_FALSE(json.overflowed());
			return std::string(json.data(), json.length());
		}
	}
	return "";
}

bool contains(const std::string& text, const std::string& part) {
	return text.find(part) != std::string::npos;
}

ZigbeeNeighbor neighbor(hc::zigbee::Ieee ieee, uint16_t short_addr, bool router, ZigbeeNeighbor::Relationship relation,
	uint32_t timeout_s = 0, uint32_t expires_ms = 0) {
	ZigbeeNeighbor n;
	n.ieee = ieee;
	n.short_addr = short_addr;
	n.router = router;
	n.relationship = relation;
	n.timeout_s = timeout_s;
	n.expires_ms = expires_ms;
	return n;
}

/// The lines of the capture that contain every one of `parts`.
size_t lines_with(const hc_test::CapturingSink& sink, std::vector<std::string> parts) {
	size_t n = 0;
	for (const std::string& line : sink.lines) {
		bool all = true;
		for (const std::string& part : parts) {
			all = all && contains(line, part);
		}
		n += all ? 1 : 0;
	}
	return n;
}

} // namespace

// ---- Stale timeout ------------------------------------------------------------------------------

TEST_CASE("ZigbeeDiagnostics: the stale timeout is three times the shortest periodic reporting interval") {
	ZigbeeFixture f;
	f.start_network();
	join_sml002(f);
	// Temperature, illuminance and occupancy promise a report every 600 s, the battery every 21600 s:
	// the sensor is stale after 1800 s, not 18 h.
	CHECK(f.driver.has("configure 0xaf9e 2 0x0402 0x0000 30..600 10"));
	CHECK(f.driver.has("configure 0xaf9e 2 0x0001 0x0021 3600..21600 2"));
	CHECK(f.adapter.stale_timeout_s(kSml002) == 1800);
	f.run_for(1790 * 1000);
	CHECK_FALSE(f.adapter.devices().find(kSml002)->stale);
	f.run_for(10 * 1000);
	CHECK(f.adapter.devices().find(kSml002)->stale);
	CHECK(contains(view_of(f.adapter, kSml002), "\"stale\":true"));
}

TEST_CASE("ZigbeeDiagnostics: a device whose points report on change only has no stale timeout") {
	ZigbeeFixture f;
	f.start_network();
	hc::zigbee::ZigbeeOverride bindings[2];
	const char* names[] = {"bath.temperature", "bath.humidity"};
	uint16_t clusters[] = {hc::zigbee::cluster::kTemperatureMeasurement, hc::zigbee::cluster::kRelativeHumidity};
	for (size_t i = 0; i < 2; ++i) {
		bindings[i].name = names[i];
		bindings[i].ieee = kSensor;
		bindings[i].endpoint = 1;
		bindings[i].cluster = clusters[i];
		bindings[i].attribute = 0x0000;
		bindings[i].reporting.set = hc::zigbee::ReportingOverride::kMax;
		bindings[i].reporting.max_s = 0; // ZCL: on a change only
	}
	REQUIRE(f.adapter.configure(bindings, 2));
	f.join_sensor();
	CHECK(f.adapter.stale_timeout_s(kSensor) == 0);
	f.run_for(48ull * 3600 * 1000);
	CHECK_FALSE(f.adapter.devices().find(kSensor)->stale);
}

// ---- Radio facts in the adapter view ------------------------------------------------------------

TEST_CASE("ZigbeeDiagnostics: lqi and rssi appear with the first frame the stack hands them for") {
	ZigbeeFixture f;
	f.start_network();
	join_sml002(f);
	std::string view = view_of(f.adapter, kSml002);
	CHECK_FALSE(contains(view, "\"lqi\""));
	CHECK_FALSE(contains(view, "\"rssi\""));
	f.adapter.radio(ZigbeeFixture::by_short(kSml002Short), 156, -71);
	view = view_of(f.adapter, kSml002);
	CHECK(contains(view, "\"lqi\":156,\"rssi\":-71"));
	// The facts of a frame are not a sign of life: the frame's own event is (radio() comes first).
	CHECK(f.adapter.devices().find(kSml002)->seen_ms == f.clock.now_ms);
	f.run_for(1800 * 1000);
	f.adapter.radio(ZigbeeFixture::by_short(kSml002Short), 80, -88);
	CHECK(f.adapter.devices().find(kSml002)->stale);
	CHECK(contains(view_of(f.adapter, kSml002), "\"lqi\":80,\"rssi\":-88"));
	// An unknown sender is ignored.
	f.adapter.radio(ZigbeeFixture::by_short(0x7777), 1, -100);
}

TEST_CASE("ZigbeeDiagnostics: a child of the coordinator shows its parent, timeout and remaining time") {
	ZigbeeFixture f;
	f.start_network();
	join_sml002(f);
	CHECK_FALSE(contains(view_of(f.adapter, kSml002), "\"parent\""));
	CHECK_FALSE(contains(view_of(f.adapter, kSml002), "\"timeout_s\""));

	f.driver.neighbors.push_back(neighbor(kSml002, kSml002Short, false, ZigbeeNeighbor::Relationship::child, 256 * 60,
		200000));
	f.run_for(ZigbeeAdapter::kCheckPeriodMs);
	std::string view = view_of(f.adapter, kSml002);
	CHECK(contains(view, "\"parent\":\"coordinator\",\"timeout_s\":15360,\"expires_in_s\":200"));

	// The next read gives the remaining time of then.
	f.driver.neighbors[0].expires_ms = 15350000;
	f.run_for(ZigbeeAdapter::kCheckPeriodMs);
	CHECK(contains(view_of(f.adapter, kSml002), "\"expires_in_s\":15350"));

	// An unreadable table keeps what the last read found.
	f.driver.neighbors_readable = false;
	f.run_for(ZigbeeAdapter::kCheckPeriodMs);
	CHECK(contains(view_of(f.adapter, kSml002), "\"parent\":\"coordinator\",\"timeout_s\":15360"));

	// Gone from the table: no parent any more.
	f.driver.neighbors_readable = true;
	f.driver.neighbors.clear();
	f.run_for(ZigbeeAdapter::kCheckPeriodMs);
	view = view_of(f.adapter, kSml002);
	CHECK_FALSE(contains(view, "\"parent\""));
	CHECK_FALSE(contains(view, "\"timeout_s\""));
	CHECK_FALSE(contains(view, "\"expires_in_s\""));
}

TEST_CASE("ZigbeeDiagnostics: a router has no parent, a child of a router names it") {
	ZigbeeFixture f;
	f.start_network();
	f.join_bulb();
	join_sml002(f);
	REQUIRE(f.adapter.set_label(kBulb, "Steckdose"));
	// The bulb is a router neighbour (a sibling of the coordinator in the table).
	f.driver.neighbors.push_back(neighbor(kBulb, kBulbShort, true, ZigbeeNeighbor::Relationship::sibling));
	f.run_for(ZigbeeAdapter::kCheckPeriodMs);
	CHECK_FALSE(contains(view_of(f.adapter, kBulb), "\"parent\""));
	// The sensor rejoins through the bulb: the trust center names its parent.
	f.adapter.device_updated(ZigbeeFixture::node(kSml002, kSml002Short), ZigbeeAdapter::kUpdateSecuredRejoin, 0,
		kBulbShort);
	char bulb[hc::zigbee::kIeeeText];
	hc::zigbee::format_ieee(kBulb, bulb);
	std::string view = view_of(f.adapter, kSml002);
	CHECK(contains(view, std::string("\"parent\":\"") + bulb + "\",\"parent_label\":\"Steckdose\""));
	CHECK_FALSE(contains(view, "\"timeout_s\"")); // only the coordinator's own children have its timeout
	// A router known from its announce.
	f.adapter.device_announced(ZigbeeFixture::node(kBulb, kBulbShort), 0x8e);
	f.adapter.device_updated(ZigbeeFixture::node(kBulb, kBulbShort), ZigbeeAdapter::kUpdateSecuredRejoin, 0, 0x0000);
	CHECK_FALSE(contains(view_of(f.adapter, kBulb), "\"parent\""));
	// A leave forgets the parent.
	f.adapter.device_left(ZigbeeFixture::node(kSml002, kSml002Short));
	CHECK_FALSE(contains(view_of(f.adapter, kSml002), "\"parent\""));
}

// ---- Joins, rejoins and the child table in the log ----------------------------------------------

TEST_CASE("ZigbeeDiagnostics: a secured rejoin is one info line naming the device, its parent and the answer") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	join_sml002(f);
	f.driver.calls.clear();
	f.adapter.device_updated(ZigbeeFixture::node(kSml002, kSml002Short), ZigbeeAdapter::kUpdateSecuredRejoin, 0,
		0x0000);
	CHECK(lines_with(capture.sink, {"0017880106498a49"}) >= 1);
	CHECK(lines_with(capture.sink, {"secured rejoin"}) == 1);
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof): secured rejoin (short 0xaf9e) "
								"through the coordinator, trust center: authorize"));
	// A rejoin is no fresh join: nothing is interviewed again.
	CHECK(f.driver.count("active") == 0);

	// A trust center rejoin and a denied one name their kind and the answer.
	f.adapter.device_updated(ZigbeeFixture::node(kSml002, kSml002Short), ZigbeeAdapter::kUpdateTrustCenterRejoin, 1,
		0x0000);
	CHECK(capture.sink.contains("Bewegungsmelder Hof): trust center rejoin (short 0xaf9e) through the coordinator, "
								"trust center: deny"));
	// The authorization that follows, success and failure.
	f.adapter.device_authorized(ZigbeeFixture::node(kSml002, kSml002Short), 0, 0);
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof) (short 0xaf9e): authorization "
								"(legacy device): success"));
	f.adapter.device_authorized(ZigbeeFixture::node(kSml002, kSml002Short), 1, 1);
	CHECK(capture.sink.contains("W hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof) (short 0xaf9e): authorization "
								"(trust center link key exchange): timeout"));
	f.adapter.device_associated(kSml002);
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof) associated with the coordinator"));
}

TEST_CASE("ZigbeeDiagnostics: an unsecured join is logged and interviews a known device again") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	join_sml002(f);
	f.adapter.device_updated(ZigbeeFixture::node(kSml002, kSml002Short), ZigbeeAdapter::kUpdateUnsecuredJoin, 0,
		0x0000);
	CHECK(capture.sink.contains("(Bewegungsmelder Hof): unsecured join (short 0xaf9e) through the coordinator, "
								"trust center: authorize"));
	CHECK(capture.sink.contains("joined afresh (short 0xaf9e): it was reset, interviewing it again"));
	// A device not in the database yet is named by its IEEE address.
	f.adapter.device_updated(ZigbeeFixture::node(0x00124b00aabbccddull, 0x4242), ZigbeeAdapter::kUpdateUnsecuredJoin, 0,
		kSml002Short);
	CHECK(capture.sink.contains("I hc.zigbee: 00124b00aabbccdd: unsecured join (short 0x4242) through "
								"0017880106498a49 (Bewegungsmelder Hof), trust center: authorize"));
}

TEST_CASE("ZigbeeDiagnostics: a child removed from the coordinator's table is one info line naming the device") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	join_sml002(f);
	f.driver.neighbors.push_back(neighbor(kSml002, kSml002Short, false, ZigbeeNeighbor::Relationship::child, 15360,
		9000));
	f.run_for(ZigbeeAdapter::kCheckPeriodMs);
	// The first read names every child it finds.
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof) is a child of the coordinator "
								"(short 0xaf9e, end-device timeout 15360 s, 9 s left)"));
	size_t reads = f.driver.neighbor_reads;
	f.run_for(5 * ZigbeeAdapter::kCheckPeriodMs);
	CHECK(f.driver.neighbor_reads == reads + 5);
	CHECK(lines_with(capture.sink, {"child of the coordinator"}) == 1); // no change, no line

	// Aged out: one line with what the table said before.
	uint32_t generation = f.adapter.generation();
	f.driver.neighbors.clear();
	f.run_for(ZigbeeAdapter::kCheckPeriodMs);
	CHECK(lines_with(capture.sink, {"0017880106498a49 (Bewegungsmelder Hof)", "child table"}) == 1);
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof) is no longer a child of the "
								"coordinator: removed from its child table (end-device timeout 15360 s, 9 s were left 10 s "
								"before; last heard 70 s ago)"));
	CHECK(f.adapter.generation() > generation); // the pages read the device again
	f.run_for(3 * ZigbeeAdapter::kCheckPeriodMs);
	CHECK(lines_with(capture.sink, {"child table"}) == 1);

	// Back: one line.
	f.driver.neighbors.push_back(neighbor(kSml002, kSml002Short, false, ZigbeeNeighbor::Relationship::child, 15360,
		15360000));
	f.run_for(ZigbeeAdapter::kCheckPeriodMs);
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof) became a child of the "
								"coordinator (short 0xaf9e, end-device timeout 15360 s, 15360 s left)"));
}

TEST_CASE("ZigbeeDiagnostics: the child table is not read while the network is down") {
	ZigbeeFixture f;
	REQUIRE(f.adapter.start());
	f.run_for(3 * ZigbeeAdapter::kCheckPeriodMs);
	CHECK(f.driver.neighbor_reads == 0);
	f.adapter.network_state(true, 0x1a62, 15, 0xdddddddddddddddd);
	f.run_for(ZigbeeAdapter::kCheckPeriodMs);
	CHECK(f.driver.neighbor_reads == 1);
}

TEST_CASE("ZigbeeDiagnosticsProbe: a child that moved to a router keeps that parent when the coordinator drops it") {
	ZigbeeFixture f;
	f.start_network();
	f.join_bulb();
	join_sml002(f);
	REQUIRE(f.adapter.set_label(kBulb, "Steckdose"));
	f.driver.neighbors.push_back(neighbor(kBulb, kBulbShort, true, ZigbeeNeighbor::Relationship::sibling));
	f.driver.neighbors.push_back(neighbor(kSml002, kSml002Short, false, ZigbeeNeighbor::Relationship::child, 256 * 60,
		200000));
	f.run_for(ZigbeeAdapter::kCheckPeriodMs);
	CHECK(contains(view_of(f.adapter, kSml002), "\"parent\":\"coordinator\""));
	// The sensor rejoins through the plug; the coordinator's table still holds it until it ages out.
	f.adapter.device_updated(ZigbeeFixture::node(kSml002, kSml002Short), ZigbeeAdapter::kUpdateSecuredRejoin, 0,
		kBulbShort);
	char bulb[hc::zigbee::kIeeeText];
	hc::zigbee::format_ieee(kBulb, bulb);
	std::string expected = std::string("\"parent\":\"") + bulb + "\",\"parent_label\":\"Steckdose\"";
	// The coordinator drops its stale entry at the next read: the plug stays the parent.
	f.driver.neighbors.pop_back();
	f.run_for(ZigbeeAdapter::kCheckPeriodMs);
	std::string view = view_of(f.adapter, kSml002);
	CHECK(contains(view, expected));
	CHECK_FALSE(contains(view, "\"timeout_s\""));
}
