/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The requests to a sleepy device and the stale timeout from confirmed promises (homecontrol #117).
// The house controller, 2026-11-22: after a restart the Philips SML002 motion sensor (a sleepy end
// device, a child of the coordinator) had its four reads answered within 1.5 s of its first frame,
// while the bindings and reporting configurations of occupancy and battery never got an answer
// ("no answer to the bindings and reporting configurations of occupancy and battery (0x85 TIMEOUT),
// repeated"). The adapter had handed the stack twelve requests for the sensor at once (four reads,
// four bindings, four configurations) in that order; the coordinator's indirect queue holds 12
// frames for all its sleepy children and drops each after 7.68 s. And the sensor was marked
// "silent for 1800 s (3 reporting intervals)" from the 600 s of occupancy, whose configuration it had
// never confirmed.

#include <string>
#include <vector>

#include "doctest.h"
#include "hc/zigbee/adapter.h"
#include "test_support.h"
#include "zigbee_test_support.h"

using hc::zigbee::Function;
using hc::zigbee::PointWatch;
using hc::zigbee::ReportingOverride;
using hc::zigbee::ReportingState;
using hc::zigbee::ZigbeeAdapter;
using hc::zigbee::ZigbeeOverride;
using hc_test::ZigbeeFixture;
namespace cluster = hc::zigbee::cluster;
namespace attribute = hc::zigbee::attribute;
namespace zcl_type = hc::zigbee::zcl_type;
namespace zdo_status = hc::zigbee::zdo_status;

namespace {

constexpr hc::zigbee::Ieee kSml002 = 0x0017880106498a49ull;
constexpr uint16_t kSml002Short = 0xc6db;
constexpr uint8_t kEndpoint = 2;

/// The sensor's four functions with the clusters of their configurations.
struct Point {
	Function function;
	uint16_t cluster;
};
const Point kPoints[] = {
	{Function::temperature, cluster::kTemperatureMeasurement},
	{Function::illuminance, cluster::kIlluminanceMeasurement},
	{Function::occupancy, cluster::kOccupancySensing},
	{Function::battery, cluster::kPowerConfiguration},
};

/// A frame of the sensor that concerns none of its points (a report of the Basic cluster's software
/// build): it is heard, nothing else.
void other_frame(ZigbeeFixture& f) {
	f.report(kSml002Short, kEndpoint, cluster::kBasic, 0x4000, zcl_type::kUint8, 1);
}

void occupancy_report(ZigbeeFixture& f, int value) {
	f.report(kSml002Short, kEndpoint, cluster::kOccupancySensing, attribute::kOccupancy, zcl_type::kBitmap8, value);
}

/// Joins the sensor (an end device whose receiver is off while idle) and answers nothing.
void join_sml002(ZigbeeFixture& f) {
	f.adapter.device_announced(ZigbeeFixture::node(kSml002, kSml002Short), 0x80);
	uint8_t endpoints[] = {kEndpoint};
	f.adapter.active_endpoints(ZigbeeFixture::by_short(kSml002Short), 0, endpoints, 1);
	f.descriptor(kSml002Short, kEndpoint, hc::zigbee::kHomeAutomationProfile, 0x0107,
		{0x0000, 0x0001, 0x0003, 0x0406, 0x0400, 0x0402}, {0x0019});
	f.basic(kSml002Short, kEndpoint, "Philips", "SML002");
	REQUIRE(f.adapter.interviewing() == 0);
	REQUIRE(f.adapter.set_label(kSml002, "Bewegungsmelder Hof"));
}

/// Requests of the sensor among the driver's calls: reads, bindings and configurations.
size_t requests(const ZigbeeFixture& f) {
	return f.driver.count("read 0xc6db") + f.driver.count("bind 0xc6db") + f.driver.count("configure 0xc6db");
}

/// The sensor known from an earlier boot, the controller started afresh: nothing goes to it before
/// its first frame.
struct Rebooted {
	hc_test::FakeBlobStore store;
	ZigbeeFixture f;

	Rebooted() {
		{
			ZigbeeFixture before;
			before.start_network();
			join_sml002(before);
			store = before.store;
		}
		f.store = store;
		f.start_network();
		f.run_for(10 * ZigbeeAdapter::kRefreshSpacingMs);
		REQUIRE(requests(f) == 0);
	}

	const PointWatch* watch(Function function) const { return f.adapter.watch(kSml002, kEndpoint, function); }
};

} // namespace

// ---- The requests to a sleepy device ----------------------------------------------------------

TEST_CASE("ZigbeeSleepy: the boot of the house: after the sensor's first frame it gets at most kSleepyBurst requests "
		  "per frame of its own, and every read, binding and configuration in turn, occupancy and battery too") {
	hc_test::ScopedCapture capture;
	Rebooted r;
	ZigbeeFixture& f = r.f;
	occupancy_report(f, 1); // its first frame
	CHECK(capture.sink.contains("heard (short 0xc6db): reading its datapoints and configuring its reporting now"));
	f.run_for(ZigbeeAdapter::kRefreshSpacingMs);
	CHECK(requests(f) == ZigbeeAdapter::kSleepyBurst);
	CHECK(f.driver.count("read 0xc6db") == ZigbeeAdapter::kSleepyBurst); // the values first
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof): the reporting of 0 attribute(s) "
								"requested, 4 more after its next frames (it sleeps: 3 requests per frame of its own)"));
	// Each answer of the sensor is a frame of its own: the next requests follow it, never more than
	// kSleepyBurst at a time.
	size_t before = requests(f);
	for (int i = 0; i < 10 && requests(f) < 12; ++i) {
		other_frame(f);
		f.run_for(ZigbeeAdapter::kWatchDelayMs);
		CHECK(requests(f) - before <= ZigbeeAdapter::kSleepyBurst);
		CHECK(requests(f) > before); // something goes after every frame until all went
		before = requests(f);
	}
	CHECK(f.driver.count("read 0xc6db") == 4);
	CHECK(f.driver.count("bind 0xc6db") == 4);
	CHECK(f.driver.count("configure 0xc6db") == 4);
	// Each binding goes with its configuration, in the same round.
	for (size_t i = 0; i + 1 < f.driver.calls.size(); ++i) {
		if (f.driver.calls[i].compare(0, 4, "bind") == 0) {
			std::string configure = "configure 0xc6db 2 " + f.driver.calls[i].substr(14, 6);
			CHECK(f.driver.calls[i + 1].compare(0, configure.size(), configure) == 0);
		}
	}
	CHECK(f.driver.has("configure 0xc6db 2 0x0406 0x0000 0..600 0"));
	CHECK(f.driver.has("configure 0xc6db 2 0x0001 0x0021 3600..21600 2"));
	// The sensor answers them: every point is configured, nothing goes out any more.
	for (const Point& p : kPoints) {
		f.adapter.reporting_configured(ZigbeeFixture::by_short(kSml002Short), kEndpoint, p.cluster, 0);
		f.adapter.bind_result(ZigbeeFixture::by_short(kSml002Short), zdo_status::kSuccess, kEndpoint, p.cluster);
		CHECK(r.watch(p.function)->state() == ReportingState::configured);
	}
	f.driver.calls.clear();
	for (int i = 0; i < 3; ++i) {
		f.run_for(60 * 1000);
		occupancy_report(f, i % 2);
		f.run_for(ZigbeeAdapter::kWatchDelayMs);
	}
	CHECK(f.driver.count("bind 0xc6db") == 0);
	CHECK(f.driver.count("configure 0xc6db") == 0);
}

TEST_CASE("ZigbeeSleepy: a newly joined sensor: its interview's end configures one point, the others follow its "
		  "frames") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	join_sml002(f);
	f.run_for(ZigbeeAdapter::kRefreshSpacingMs);
	CHECK(f.driver.count("configure 0xc6db") <= 1);
	CHECK(capture.sink.contains("more after its next frames (it sleeps: 3 requests per frame of its own)"));
	// Nothing while it sleeps.
	size_t before = requests(f);
	f.run_for(5 * 60 * 1000);
	CHECK(requests(f) == before);
	for (int i = 0; i < 10 && f.driver.count("configure 0xc6db") < 4; ++i) {
		other_frame(f);
		f.run_for(ZigbeeAdapter::kWatchDelayMs);
	}
	CHECK(f.driver.count("configure 0xc6db") == 4);
	CHECK(f.driver.count("bind 0xc6db") == 4);
}

TEST_CASE("ZigbeeSleepy: a listening device gets its requests at once") {
	ZigbeeFixture f;
	f.start_network();
	f.join_bulb();
	f.run_for(ZigbeeAdapter::kRefreshSpacingMs);
	// The bulb's three points (on, level, colour temperature) are configured with the interview.
	CHECK(f.driver.count("configure 0x1a2b") >= 2);
	CHECK(f.driver.count("bind 0x1a2b") >= 2);
}

TEST_CASE("ZigbeeSleepy: a read that waits for its turn is dropped when the point reports first") {
	hc_test::ScopedCapture capture;
	Rebooted r;
	ZigbeeFixture& f = r.f;
	other_frame(f); // the first frame
	f.run_for(ZigbeeAdapter::kRefreshSpacingMs);
	REQUIRE(f.driver.count("read 0xc6db") == ZigbeeAdapter::kSleepyBurst);
	// The read left waits; which one is the point table's order: whichever it is, its report ends it.
	Function waiting = Function::temperature;
	for (const Point& p : kPoints) {
		if (r.watch(p.function)->read_wanted()) {
			waiting = p.function;
		}
	}
	REQUIRE(r.watch(waiting)->read_wanted());
	REQUIRE(r.watch(waiting)->reads() == 0);
	for (const Point& p : kPoints) {
		if (p.function == waiting) {
			hc::zigbee::AttributeRecord record;
			uint8_t data[2] = {1, 0};
			record.attribute = p.function == Function::battery ? attribute::kBatteryPercentage : attribute::kMeasuredValue;
			record.type = p.function == Function::occupancy ? zcl_type::kBitmap8
				: p.function == Function::battery           ? zcl_type::kUint8
				: p.function == Function::temperature       ? zcl_type::kInt16
															: zcl_type::kUint16;
			if (p.function == Function::occupancy) {
				record.attribute = attribute::kOccupancy;
			}
			record.data = data;
			record.length = hc::zigbee::zcl_type_size(record.type);
			f.adapter.attributes(ZigbeeFixture::by_short(kSml002Short), kEndpoint, p.cluster, &record, 1, true);
		}
	}
	CHECK_FALSE(r.watch(waiting)->read_wanted());
	size_t reads = f.driver.count("read 0xc6db");
	for (int i = 0; i < 6; ++i) {
		other_frame(f);
		f.run_for(ZigbeeAdapter::kWatchDelayMs);
	}
	CHECK(f.driver.count("read 0xc6db") == reads); // not read any more
}

TEST_CASE("ZigbeeSleepy: a changed reporting of a sleepy device waits for its next frame") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	join_sml002(f);
	for (int i = 0; i < 10 && f.driver.count("configure 0xc6db") < 4; ++i) {
		other_frame(f);
		f.run_for(ZigbeeAdapter::kWatchDelayMs);
	}
	REQUIRE(f.driver.count("configure 0xc6db") == 4);
	ZigbeeOverride binding;
	binding.name = "hof.temperature";
	binding.ieee = kSml002;
	binding.endpoint = kEndpoint;
	binding.cluster = cluster::kTemperatureMeasurement;
	binding.attribute = attribute::kMeasuredValue;
	binding.reporting.set = ReportingOverride::kMax;
	binding.reporting.max_s = 3600;
	REQUIRE(f.adapter.configure(&binding, 1));
	f.driver.calls.clear();
	REQUIRE(f.adapter.configure(&binding, 1)); // applied: the second application changes nothing
	ZigbeeOverride changed = binding;
	changed.reporting.max_s = 1800;
	REQUIRE(f.adapter.configure(&changed, 1));
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof): the new bindings change the "
								"reporting of 1 attribute(s): configured after its next frames (it sleeps)"));
	CHECK(f.driver.count("configure 0xc6db") == 0); // it sleeps: nothing yet
	f.run_for(60 * 1000);
	CHECK(f.driver.count("configure 0xc6db") == 0);
	other_frame(f);
	f.run_for(ZigbeeAdapter::kWatchDelayMs);
	CHECK(f.driver.has("configure 0xc6db 2 0x0402 0x0000 30..1800 10"));
	CHECK(f.driver.count("configure 0xc6db") == 1);
}

// ---- Undelivered requests ---------------------------------------------------------------------

TEST_CASE("ZigbeeSleepy: an undelivered request is one info line and no sign of life") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	join_sml002(f);
	uint64_t seen_ms = f.adapter.devices().find(kSml002)->seen_ms;
	f.run_for(10 * 1000);
	f.adapter.request_undelivered(ZigbeeFixture::by_short(kSml002Short), kEndpoint, 37, "ESP_FAIL");
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof): a request to endpoint 2 (ZCL "
								"transaction 37) was not delivered: ESP_FAIL"));
	CHECK(f.adapter.devices().find(kSml002)->seen_ms == seen_ms);
	f.adapter.request_undelivered(ZigbeeFixture::by_short(0x1234), 1, 5, "ESP_FAIL");
	CHECK(capture.sink.contains("an unknown device (short 0x1234): a request to endpoint 1 (ZCL transaction 5) was not "
								"delivered: ESP_FAIL"));
}

// ---- The stale timeout from confirmed promises ---------------------------------------------------

TEST_CASE("ZigbeeSleepy: only a confirmed configuration sets the stale timeout once the device confirmed one") {
	ZigbeeFixture f;
	f.start_network();
	join_sml002(f);
	auto answer = [&](uint16_t cluster_id, uint8_t status) {
		f.adapter.reporting_configured(ZigbeeFixture::by_short(kSml002Short), kEndpoint, cluster_id, status);
	};
	// Nothing confirmed yet: every promise counts (600 s of temperature, illuminance, occupancy).
	CHECK(f.adapter.stale_timeout_s(kSml002) == 3 * 600);
	// The battery's configuration confirmed alone (21600 s): the others' 600 s promise nothing yet.
	answer(cluster::kPowerConfiguration, 0);
	CHECK(f.adapter.stale_timeout_s(kSml002) == 3 * 21600);
	// The house's case: temperature and illuminance confirmed, occupancy never answered (timed out).
	answer(cluster::kTemperatureMeasurement, 0);
	CHECK(f.adapter.stale_timeout_s(kSml002) == 3 * 600);
	f.run_for(ZigbeeAdapter::kReportingAnswerMs + ZigbeeAdapter::kCheckPeriodMs);
	CHECK(f.adapter.watch(kSml002, kEndpoint, Function::occupancy)->state() != ReportingState::configured);
	CHECK(f.adapter.stale_timeout_s(kSml002) == 3 * 600); // the temperature's, confirmed
}

TEST_CASE("ZigbeeSleepy: a device that confirmed nothing keeps its stale timeout; a refused promise never counts") {
	ZigbeeFixture f;
	f.start_network();
	join_sml002(f);
	for (int i = 0; i < 10 && f.driver.count("configure 0xc6db") < 4; ++i) {
		other_frame(f);
		f.run_for(ZigbeeAdapter::kWatchDelayMs);
	}
	// Every configuration timed out: a device that is gone must still turn stale.
	f.run_for(ZigbeeAdapter::kReportingAnswerMs + ZigbeeAdapter::kCheckPeriodMs);
	for (const Point& p : kPoints) {
		REQUIRE(f.adapter.watch(kSml002, kEndpoint, p.function)->state() == ReportingState::timed_out);
	}
	CHECK(f.adapter.stale_timeout_s(kSml002) == 3 * 600);
	// Refused: temperature, illuminance and occupancy promise nothing; the battery's 21600 s is left.
	for (uint16_t c : {cluster::kTemperatureMeasurement, cluster::kIlluminanceMeasurement, cluster::kOccupancySensing}) {
		f.adapter.reporting_configured(ZigbeeFixture::by_short(kSml002Short), kEndpoint, c, 0x86);
	}
	CHECK(f.adapter.stale_timeout_s(kSml002) == 3 * 21600);
}
