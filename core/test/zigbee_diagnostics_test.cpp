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
	// the sensor is stale after 1800 s, not 18 h (none of its configurations is answered here: every
	// point counts, homecontrol #117).
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

// ---- Rework from the house controller: a rejoin with a new address, the routers' tables ----------

namespace {

constexpr uint16_t kSml002NewShort = 0x5025;

/// The plug of the house as a router: its announce names it one.
void make_router(ZigbeeFixture& f) {
	f.join_bulb();
	REQUIRE(f.adapter.set_label(kBulb, "Steckdose"));
	f.adapter.device_announced(ZigbeeFixture::node(kBulb, kBulbShort), 0x8e);
}

ZigbeeNeighbor router_entry(hc::zigbee::Ieee ieee, uint16_t short_addr, ZigbeeNeighbor::Relationship relation,
	uint8_t lqi, uint8_t depth, bool coordinator = false) {
	ZigbeeNeighbor n = neighbor(ieee, short_addr, coordinator, relation);
	n.coordinator = coordinator;
	n.lqi = lqi;
	n.depth = depth;
	return n;
}

/// The plug's table: the coordinator (its parent) and the sensor as its child.
std::vector<ZigbeeNeighbor> plug_table() {
	return {router_entry(0x60554bfffe010203ull, 0x0000, ZigbeeNeighbor::Relationship::parent, 40, 0, true),
		router_entry(kSml002, kSml002NewShort, ZigbeeNeighbor::Relationship::child, 120, 2)};
}

/// Runs the loop in steps of 100 ms until the adapter asks a router for its table (at most
/// `limit_ms`); the request's line, or "" when none came.
std::string next_request(ZigbeeFixture& f, uint64_t limit_ms = ZigbeeAdapter::kTopologyPeriodMs + 10000) {
	size_t before = f.driver.count("lqi");
	for (uint64_t waited = 0; waited < limit_ms; waited += 100) {
		f.run_for(100);
		if (f.driver.count("lqi") > before) {
			return f.driver.calls.back();
		}
	}
	return "";
}

/// Answers the pass the joins asked for with an empty table.
void settle(ZigbeeFixture& f) {
	REQUIRE(next_request(f, ZigbeeAdapter::kTopologyDelayMs + 1000) == "lqi 0x1a2b 0");
	f.adapter.neighbor_table(ZigbeeFixture::node(kBulb, kBulbShort), 0, 0, 0, nullptr, 0);
}

} // namespace

TEST_CASE("ZigbeeDiagnosticsRework: an announce with a new short address is one rejoin line, without a notice") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	join_sml002(f);
	f.adapter.device_announced(ZigbeeFixture::node(kSml002, kSml002NewShort), 0x80);
	CHECK(lines_with(capture.sink, {"0017880106498a49", "0xaf9e -> 0x5025"}) == 1);
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof) rejoined with a new address: short "
								"0xaf9e -> 0x5025, no trust-center notice (its parent is looked up in the routers' "
								"tables)"));
	CHECK_FALSE(capture.sink.contains("announced itself (short 0x5025)"));
	CHECK(f.adapter.devices().find(kSml002)->short_addr == kSml002NewShort);
	// The same address again is no rejoin.
	f.adapter.device_announced(ZigbeeFixture::node(kSml002, kSml002NewShort), 0x80);
	CHECK(lines_with(capture.sink, {"rejoined with a new address"}) == 1);
	CHECK(capture.sink.contains("(Bewegungsmelder Hof) announced itself (short 0x5025)"));
}

TEST_CASE("ZigbeeDiagnosticsRework: a rejoin the trust center reported names the notice and the parent once") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	make_router(f);
	join_sml002(f);
	// The notice moves the short address first; the announce that follows still knows the old one.
	f.adapter.device_updated(ZigbeeFixture::node(kSml002, kSml002NewShort), ZigbeeAdapter::kUpdateSecuredRejoin, 0,
		kBulbShort);
	f.adapter.device_announced(ZigbeeFixture::node(kSml002, kSml002NewShort), 0x80);
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof) rejoined with a new address: short "
								"0xaf9e -> 0x5025 through 00124b0001020304 (Steckdose), after the trust center's "
								"secured rejoin notice"));
	// Its second announce (the SML002 sends two) is no second rejoin.
	f.adapter.device_announced(ZigbeeFixture::node(kSml002, kSml002NewShort), 0x80);
	CHECK(lines_with(capture.sink, {"rejoined with a new address"}) == 1);
	// A notice long before the announce is not its notice.
	f.adapter.device_updated(ZigbeeFixture::node(kSml002, kSml002NewShort), ZigbeeAdapter::kUpdateSecuredRejoin, 0,
		kBulbShort);
	f.run_for(ZigbeeAdapter::kNoticeBeforeAnnounceMs + 1000);
	f.adapter.device_announced(ZigbeeFixture::node(kSml002, 0x6161), 0x80);
	CHECK(capture.sink.contains("short 0x5025 -> 0x6161, no trust-center notice"));
}

TEST_CASE("ZigbeeDiagnosticsRework: the parent of a router's child comes from the router's paged neighbour table") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	make_router(f);
	join_sml002(f);
	settle(f);
	f.driver.calls.clear();
	f.adapter.device_announced(ZigbeeFixture::node(kSml002, kSml002NewShort), 0x80);
	f.run_for(ZigbeeAdapter::kTopologyDelayMs - 1);
	CHECK(f.driver.count("lqi") == 0);
	f.run_for(1);
	REQUIRE(f.driver.count("lqi") == 1);
	REQUIRE(f.driver.calls.back() == "lqi 0x1a2b 0");
	// Page 1 of 2: the coordinator and the sensor.
	std::vector<ZigbeeNeighbor> table = plug_table();
	f.adapter.neighbor_table(ZigbeeFixture::node(kBulb, kBulbShort), 0, 4, 0, table.data(), table.size());
	REQUIRE(f.driver.calls.back() == "lqi 0x1a2b 2");
	CHECK(f.driver.count("lqi") == 2);
	// Page 2: two devices the controller does not know.
	std::vector<ZigbeeNeighbor> rest = {
		router_entry(0x1111111111111111ull, 0x1111, ZigbeeNeighbor::Relationship::child, 90, 2),
		router_entry(0x2222222222222222ull, 0x2222, ZigbeeNeighbor::Relationship::sibling, 70, 1)};
	f.adapter.neighbor_table(ZigbeeFixture::node(kBulb, kBulbShort), 0, 4, 2, rest.data(), rest.size());
	CHECK(f.driver.count("lqi") == 2); // the table is read to its end

	char bulb[hc::zigbee::kIeeeText];
	hc::zigbee::format_ieee(kBulb, bulb);
	std::string view = view_of(f.adapter, kSml002);
	CHECK(contains(view, std::string("\"parent\":\"") + bulb +
		"\",\"parent_label\":\"Steckdose\",\"parent_lqi\":120,\"depth\":2"));
	CHECK(contains(view_of(f.adapter, kBulb), "\"coordinator_lqi\":40"));
	CHECK_FALSE(contains(view_of(f.adapter, kBulb), "\"parent\""));
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof) is a child of 00124b0001020304 "
								"(Steckdose) (the router's link quality of it 120, depth 2)"));

	// The next pass (every 5 min) finds it there again: no new line. Then it is gone from the table.
	REQUIRE(next_request(f) == "lqi 0x1a2b 0");
	f.adapter.neighbor_table(ZigbeeFixture::node(kBulb, kBulbShort), 0, 2, 0, table.data(), table.size());
	CHECK(lines_with(capture.sink, {"is a child of 00124b0001020304"}) == 1);
	REQUIRE(next_request(f) == "lqi 0x1a2b 0");
	f.adapter.neighbor_table(ZigbeeFixture::node(kBulb, kBulbShort), 0, 1, 0, table.data(), 1);
	// One complete read without it is no proof (homecontrol #114): the parent stays until the next.
	CHECK_FALSE(capture.sink.contains("is no longer a child of 00124b0001020304"));
	CHECK(contains(view_of(f.adapter, kSml002), "\"parent\""));
	REQUIRE(next_request(f) == "lqi 0x1a2b 0");
	f.adapter.neighbor_table(ZigbeeFixture::node(kBulb, kBulbShort), 0, 1, 0, table.data(), 1);
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof) is no longer a child of "
								"00124b0001020304 (Steckdose) (not in its table twice)"));
	CHECK_FALSE(contains(view_of(f.adapter, kSml002), "\"parent\""));
}

TEST_CASE("ZigbeeDiagnosticsRework: one request in flight, a timeout, late answers ignored, pages bounded") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	make_router(f);
	join_sml002(f);
	REQUIRE(next_request(f, ZigbeeAdapter::kTopologyDelayMs + 1000) == "lqi 0x1a2b 0");
	size_t asked = f.driver.count("lqi");
	// More reasons for a pass while one runs: no second request in flight.
	f.adapter.device_announced(ZigbeeFixture::node(kSml002, kSml002NewShort), 0x80);
	f.run_for(ZigbeeAdapter::kTopologyTimeoutMs - 1);
	CHECK(f.driver.count("lqi") == asked);
	// No answer: one line, the parent known stays, the pass that was asked for follows.
	f.run_for(1);
	CHECK(capture.sink.contains("I hc.zigbee: 00124b0001020304 (Steckdose) did not answer the request for its neighbour "
								"table (entries from 0) within 5 s"));
	f.run_for(ZigbeeAdapter::kTopologyDelayMs);
	CHECK(f.driver.count("lqi") == asked + 1);
	f.run_for(ZigbeeAdapter::kTopologyTimeoutMs);
	CHECK(lines_with(capture.sink, {"did not answer the request for its neighbour table"}) == 1); // once
	// A late answer is ignored.
	std::vector<ZigbeeNeighbor> table = plug_table();
	f.adapter.neighbor_table(ZigbeeFixture::node(kBulb, kBulbShort), 0, 2, 0, table.data(), table.size());
	CHECK_FALSE(contains(view_of(f.adapter, kSml002), "\"parent_lqi\""));

	// A table that claims 255 entries and hands one per page: at most kTopologyMaxPages requests.
	f.driver.calls.clear();
	REQUIRE(next_request(f) == "lqi 0x1a2b 0");
	for (unsigned page = 0; page < 40 && f.driver.calls.back() == "lqi 0x1a2b " + std::to_string(page); ++page) {
		f.adapter.neighbor_table(ZigbeeFixture::node(kBulb, kBulbShort), 0, 255, static_cast<uint8_t>(page),
			table.data() + 1, 1);
	}
	CHECK(f.driver.count("lqi") == ZigbeeAdapter::kTopologyMaxPages);
	CHECK(capture.sink.contains("00124b0001020304 (Steckdose) answers the requests for its neighbour table again"));
	CHECK(contains(view_of(f.adapter, kSml002), "\"parent_lqi\":120"));
}

TEST_CASE("ZigbeeDiagnosticsRework: a device that turns stale has its parent looked up") {
	ZigbeeFixture f;
	f.start_network();
	make_router(f);
	// The sensor joins half-way between two periodic passes, so that its stale moment (1800 s later) is
	// not one of theirs.
	f.run_for(ZigbeeAdapter::kTopologyPeriodMs / 2);
	join_sml002(f);
	// Every request is answered at once (an empty table); the bulb keeps reporting, the sensor falls
	// silent and turns stale after 1800 s; a pass follows kTopologyDelayMs later.
	uint64_t stale_ms = 0;
	uint64_t asked_after_stale_ms = 0;
	size_t answered = 0;
	for (uint64_t t = 0; t < 2000 * 1000 && asked_after_stale_ms == 0; t += 100) {
		f.run_for(100);
		if (t % (60 * 1000) == 0) {
			f.report(kBulbShort, 11, hc::zigbee::cluster::kOnOff, 0x0000, hc::zigbee::zcl_type::kBool, 1);
		}
		if (stale_ms == 0 && f.adapter.devices().find(kSml002)->stale) {
			stale_ms = f.clock.now_ms;
		}
		if (f.driver.count("lqi") > answered) {
			answered = f.driver.count("lqi");
			if (stale_ms != 0) {
				asked_after_stale_ms = f.clock.now_ms;
			}
			f.adapter.neighbor_table(ZigbeeFixture::node(kBulb, kBulbShort), 0, 0, 0, nullptr, 0);
		}
	}
	REQUIRE(stale_ms != 0);
	CHECK_FALSE(f.adapter.devices().find(kBulb)->stale);
	CHECK(asked_after_stale_ms - stale_ms <= ZigbeeAdapter::kTopologyDelayMs);
}

// ---- A known parent and the reads of its table (homecontrol #114) ---------------------------------

TEST_CASE("ZigbeeTopology: a partial or changing read of the parent's table keeps the parent; two complete reads "
		  "without the child drop it") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	make_router(f);
	join_sml002(f);
	char bulb[hc::zigbee::kIeeeText];
	hc::zigbee::format_ieee(kBulb, bulb);
	std::string parent = std::string("\"parent\":\"") + bulb + "\"";
	// In the middle of its rejoin the plug lists the sensor as an unauthenticated child: its child.
	REQUIRE(next_request(f, ZigbeeAdapter::kTopologyDelayMs + 1000) == "lqi 0x1a2b 0");
	std::vector<ZigbeeNeighbor> table = plug_table();
	table[1].relationship = ZigbeeNeighbor::Relationship::unauthenticated_child;
	f.adapter.neighbor_table(ZigbeeFixture::node(kBulb, kBulbShort), 0, 2, 0, table.data(), table.size());
	CHECK(contains(view_of(f.adapter, kSml002), parent + ",\"parent_label\":\"Steckdose\",\"parent_lqi\":120"));

	ZigbeeNeighbor coordinator = plug_table()[0];
	ZigbeeNeighbor sibling = router_entry(0x2222222222222222ull, 0x2222, ZigbeeNeighbor::Relationship::sibling, 70, 1);
	std::vector<ZigbeeNeighbor> first_page = {coordinator, sibling};
	for (int pass = 0; pass < 2; ++pass) {
		// An empty page before the table's end.
		REQUIRE(next_request(f) == "lqi 0x1a2b 0");
		f.adapter.neighbor_table(ZigbeeFixture::node(kBulb, kBulbShort), 0, 3, 0, nullptr, 0);
		// A table that changed between its pages: the child's entry moved from the second page to the first.
		REQUIRE(next_request(f) == "lqi 0x1a2b 0");
		f.adapter.neighbor_table(ZigbeeFixture::node(kBulb, kBulbShort), 0, 3, 0, first_page.data(), first_page.size());
		REQUIRE(f.driver.calls.back() == "lqi 0x1a2b 2");
		f.adapter.neighbor_table(ZigbeeFixture::node(kBulb, kBulbShort), 0, 2, 2, nullptr, 0);
		// A table that claims more than the pages read hand out.
		REQUIRE(next_request(f) == "lqi 0x1a2b 0");
		f.adapter.neighbor_table(ZigbeeFixture::node(kBulb, kBulbShort), 0, 3, 0, first_page.data(), first_page.size());
		f.run_for(ZigbeeAdapter::kTopologyTimeoutMs); // the second page stays unanswered
	}
	CHECK(contains(view_of(f.adapter, kSml002), parent));
	CHECK_FALSE(capture.sink.contains("is no longer a child of"));

	// The complete table without the sensor: kept after the first read, dropped after the second.
	REQUIRE(next_request(f) == "lqi 0x1a2b 0");
	f.adapter.neighbor_table(ZigbeeFixture::node(kBulb, kBulbShort), 0, 2, 0, first_page.data(), first_page.size());
	CHECK(contains(view_of(f.adapter, kSml002), parent));
	// Listed again in between: the miss is forgotten.
	REQUIRE(next_request(f) == "lqi 0x1a2b 0");
	f.adapter.neighbor_table(ZigbeeFixture::node(kBulb, kBulbShort), 0, 2, 0, table.data(), table.size());
	REQUIRE(next_request(f) == "lqi 0x1a2b 0");
	f.adapter.neighbor_table(ZigbeeFixture::node(kBulb, kBulbShort), 0, 2, 0, first_page.data(), first_page.size());
	CHECK(contains(view_of(f.adapter, kSml002), parent));
	REQUIRE(next_request(f) == "lqi 0x1a2b 0");
	f.adapter.neighbor_table(ZigbeeFixture::node(kBulb, kBulbShort), 0, 2, 0, first_page.data(), first_page.size());
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof) is no longer a child of "
								"00124b0001020304 (Steckdose) (not in its table twice)"));
	CHECK_FALSE(contains(view_of(f.adapter, kSml002), "\"parent\""));
}

TEST_CASE("ZigbeeTopology: a rejoin with a new address without a notice keeps the parent until a pass decides") {
	ZigbeeFixture f;
	f.start_network();
	make_router(f);
	join_sml002(f);
	REQUIRE(next_request(f, ZigbeeAdapter::kTopologyDelayMs + 1000) == "lqi 0x1a2b 0");
	std::vector<ZigbeeNeighbor> table = plug_table();
	table[1].short_addr = kSml002Short;
	f.adapter.neighbor_table(ZigbeeFixture::node(kBulb, kBulbShort), 0, 2, 0, table.data(), table.size());
	char bulb[hc::zigbee::kIeeeText];
	hc::zigbee::format_ieee(kBulb, bulb);
	std::string parent = std::string("\"parent\":\"") + bulb + "\",\"parent_label\":\"Steckdose\"";
	REQUIRE(contains(view_of(f.adapter, kSml002), parent + ",\"parent_lqi\":120"));
	// The rejoin: the parent stays, unconfirmed (no link quality of it) until the pass that follows.
	f.adapter.device_announced(ZigbeeFixture::node(kSml002, kSml002NewShort), 0x80);
	std::string view = view_of(f.adapter, kSml002);
	CHECK(contains(view, parent));
	CHECK_FALSE(contains(view, "\"parent_lqi\""));
	REQUIRE(next_request(f, ZigbeeAdapter::kTopologyDelayMs + 1000) == "lqi 0x1a2b 0");
	table = plug_table();
	f.adapter.neighbor_table(ZigbeeFixture::node(kBulb, kBulbShort), 0, 2, 0, table.data(), table.size());
	CHECK(contains(view_of(f.adapter, kSml002), parent + ",\"parent_lqi\":120"));
}
