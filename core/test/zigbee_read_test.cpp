/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The watch of reads (homecontrol #114) and of bindings and configurations without an answer
// (homecontrol #113): the sequence of the house controller's boot, where the Philips SML002 motion
// sensor (a sleepy end device behind the plug LOM007) sent an occupancy report as its first frame,
// all its points were read and the illuminance had its first value only 7½ min later; and its rejoin,
// during which four bindings were answered with the ZDO status 0x85 (TIMEOUT) and logged as refusals.

#include <string>
#include <vector>

#include "doctest.h"
#include "hc/zigbee/adapter.h"
#include "test_support.h"
#include "zigbee_test_support.h"

using hc::zigbee::Function;
using hc::zigbee::PointWatch;
using hc::zigbee::ReportingState;
using hc::zigbee::ZigbeeAdapter;
using hc_test::ZigbeeFixture;
namespace cluster = hc::zigbee::cluster;
namespace attribute = hc::zigbee::attribute;
namespace zcl_type = hc::zigbee::zcl_type;

namespace {

constexpr hc::zigbee::Ieee kSml002 = 0x0017880106498a49ull;
constexpr uint16_t kSml002Short = 0xaf9e;
constexpr hc::zigbee::Ieee kPlug = 0x001788010f069a57ull;
constexpr uint16_t kPlugShort = 0x6b21;
constexpr uint8_t kEndpoint = 2;

const char* const kReadIlluminance = "read 0xaf9e 2 0x0400 0";

/// A frame of the sensor that concerns none of its points (a report of the Basic cluster's software
/// build): it is heard, nothing else.
void other_frame(ZigbeeFixture& f) {
	f.report(kSml002Short, kEndpoint, cluster::kBasic, 0x4000, zcl_type::kUint8, 1);
}

/// `count` frames of the sensor, each followed by the requests it gets after it (kSleepyBurst each,
/// homecontrol #117).
void rounds(ZigbeeFixture& f, int count) {
	for (int i = 0; i < count; ++i) {
		other_frame(f);
		f.run_for(ZigbeeAdapter::kWatchDelayMs);
	}
}

/// Joins the sensor; its answers to the first bindings and configurations (frames of its own) bring
/// the others: four rounds in all.
void join_sml002(ZigbeeFixture& f) {
	f.adapter.device_announced(ZigbeeFixture::node(kSml002, kSml002Short), 0x80); // an end device, receiver off
	uint8_t endpoints[] = {kEndpoint};
	f.adapter.active_endpoints(ZigbeeFixture::by_short(kSml002Short), 0, endpoints, 1);
	f.descriptor(kSml002Short, kEndpoint, hc::zigbee::kHomeAutomationProfile, 0x0107,
		{0x0000, 0x0001, 0x0003, 0x0406, 0x0400, 0x0402}, {0x0019});
	f.basic(kSml002Short, kEndpoint, "Philips", "SML002");
	REQUIRE(f.adapter.interviewing() == 0);
	REQUIRE(f.adapter.set_label(kSml002, "Bewegungsmelder Hof"));
	// After the interview: the reads of the refresh, then one point's binding and configuration per frame.
	for (int i = 0; i < 8 && (f.driver.count("configure 0xaf9e") < 4 || f.driver.count("read 0xaf9e") < 4); ++i) {
		rounds(f, 1);
	}
	REQUIRE(f.driver.count("configure 0xaf9e") == 4);
	REQUIRE(f.driver.count("bind 0xaf9e") == 4);
}

void join_plug(ZigbeeFixture& f) {
	f.adapter.device_announced(ZigbeeFixture::node(kPlug, kPlugShort), 0x8e); // a mains-powered router
	uint8_t endpoints[] = {11};
	f.adapter.active_endpoints(ZigbeeFixture::by_short(kPlugShort), 0, endpoints, 1);
	f.descriptor(kPlugShort, 11, hc::zigbee::kHomeAutomationProfile, 0x010A, {0x0000, 0x0003, 0x0006}, {0x0019});
	f.basic(kPlugShort, 11, "Signify Netherlands B.V.", "LOM007");
	REQUIRE(f.adapter.interviewing() == 0);
}

/// A read's answer (not a report) of one attribute with a little-endian value of the type's size, or
/// a failure `status` without a value.
void answer(ZigbeeFixture& f, uint16_t short_addr, uint8_t endpoint, uint16_t cluster_id, uint16_t attribute_id,
	uint8_t type, int64_t value, uint8_t status = 0) {
	uint8_t data[4] = {};
	size_t size = hc::zigbee::zcl_type_size(type);
	for (size_t i = 0; i < size; ++i) {
		data[i] = static_cast<uint8_t>(static_cast<uint64_t>(value) >> (8 * i));
	}
	hc::zigbee::AttributeRecord record;
	record.attribute = attribute_id;
	record.status = status;
	record.type = status == 0 ? type : 0;
	record.data = status == 0 ? data : nullptr;
	record.length = status == 0 ? size : 0;
	f.adapter.attributes(ZigbeeFixture::by_short(short_addr), endpoint, cluster_id, &record, 1, false);
}

/// The sensor's answers to its four configurations: they are in effect.
void configured(ZigbeeFixture& f) {
	for (uint16_t c : {cluster::kIlluminanceMeasurement, cluster::kOccupancySensing, cluster::kTemperatureMeasurement,
			 cluster::kPowerConfiguration}) {
		f.adapter.reporting_configured(ZigbeeFixture::by_short(kSml002Short), kEndpoint, c, 0);
	}
}

void occupancy_report(ZigbeeFixture& f, int value = 1) {
	f.report(kSml002Short, kEndpoint, cluster::kOccupancySensing, attribute::kOccupancy, zcl_type::kBitmap8, value);
}

/// The sensor known from an earlier boot (its receiver off), the controller started afresh: nothing is
/// read until the sensor's first frame.
struct Rebooted {
	hc_test::FakeBlobStore store;
	ZigbeeFixture f;

	Rebooted() {
		{
			ZigbeeFixture before;
			before.start_network();
			join_plug(before);
			join_sml002(before);
			store = before.store;
		}
		f.store = store;
		f.start_network();
		f.run_for(10 * ZigbeeAdapter::kRefreshSpacingMs);
		REQUIRE(f.driver.count("read 0xaf9e") == 0);
	}

	/// The first frame and the reads that follow it: kSleepyBurst after the frame, the rest after the next frame of
	/// the device (its answer), kSleepyBurst requests per frame (homecontrol #117).
	void first_frame() {
		occupancy_report(f);
		f.run_for(ZigbeeAdapter::kRefreshSpacingMs);
		REQUIRE(f.driver.count("read 0xaf9e") == ZigbeeAdapter::kSleepyBurst);
		other_frame(f);
		f.run_for(ZigbeeAdapter::kWatchDelayMs);
		REQUIRE(f.driver.count("read 0xaf9e") == 4);
	}

	const PointWatch* watch(Function function) const { return f.adapter.watch(kSml002, kEndpoint, function); }
};

} // namespace

// ---- Reads (homecontrol #114) ---------------------------------------------------------------------

TEST_CASE("ZigbeeRead: the boot of the house: the first frame is an occupancy report, every point is read; the "
		  "illuminance read without an answer goes again after the next frame") {
	hc_test::ScopedCapture capture;
	Rebooted r;
	ZigbeeFixture& f = r.f;
	r.first_frame();
	CHECK(f.driver.has("read 0xaf9e 2 0x0406 0"));
	CHECK(f.driver.has(kReadIlluminance));
	CHECK(f.driver.has("read 0xaf9e 2 0x0402 0"));
	CHECK(f.driver.has("read 0xaf9e 2 0x0001 33"));
	CHECK(r.watch(Function::illuminance)->reads() == 1);
	// The parent delivers three of the four reads; the illuminance read is lost.
	answer(f, kSml002Short, kEndpoint, cluster::kOccupancySensing, attribute::kOccupancy, zcl_type::kBitmap8, 1);
	answer(f, kSml002Short, kEndpoint, cluster::kTemperatureMeasurement, attribute::kMeasuredValue, zcl_type::kInt16, 595);
	answer(f, kSml002Short, kEndpoint, cluster::kPowerConfiguration, attribute::kBatteryPercentage, zcl_type::kUint8, 200);
	// One line per point with its outcome, at info for a sleepy device (the next boot's ring shows it).
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof): occupancy on endpoint 2 read: on "
								"(read 1 of 3)"));
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof): temperature on endpoint 2 read: "
								"5.95 (read 1 of 3)"));
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof): battery on endpoint 2 read: 100 "
								"(read 1 of 3)"));
	CHECK(r.watch(Function::occupancy)->reads() == 0);
	CHECK(r.watch(Function::illuminance)->reads() == 1);
	CHECK(f.dp("zigbee.0017880106498a49.2.illuminance")->value().empty());

	// A frame soon after: the answer may still come, nothing is read again.
	f.run_for(10 * 1000);
	occupancy_report(f, 0);
	f.run_for(ZigbeeAdapter::kWatchDelayMs);
	CHECK(f.driver.count(kReadIlluminance) == 1);
	// The next frame after kReadAnswerMs: the illuminance alone is read again.
	f.run_for(ZigbeeAdapter::kReadAnswerMs);
	occupancy_report(f, 1);
	CHECK(f.driver.count(kReadIlluminance) == 1); // after the frame's own reports
	f.run_for(ZigbeeAdapter::kWatchDelayMs);
	CHECK(f.driver.count(kReadIlluminance) == 2);
	CHECK(f.driver.count("read 0xaf9e") == 5);
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof): no answer to the read of "
								"illuminance on endpoint 2 within 30 s: read again (2 of 3)"));
	answer(f, kSml002Short, kEndpoint, cluster::kIlluminanceMeasurement, attribute::kMeasuredValue, zcl_type::kUint16, 1);
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof): illuminance on endpoint 2 read: "));
	CHECK(capture.sink.contains("(read 2 of 3)"));
	CHECK_FALSE(f.dp("zigbee.0017880106498a49.2.illuminance")->value().empty());
	// Answered: further frames read nothing.
	for (int i = 0; i < 5; ++i) {
		f.run_for(60 * 1000);
		occupancy_report(f, i % 2);
	}
	f.run_for(ZigbeeAdapter::kWatchDelayMs);
	CHECK(f.driver.count("read 0xaf9e") == 5);
}

TEST_CASE("ZigbeeRead: three reads without an answer, then given up at info; the value comes with the report") {
	hc_test::ScopedCapture capture;
	Rebooted r;
	ZigbeeFixture& f = r.f;
	r.first_frame();
	// The device answers nothing; it sends a frame every minute.
	for (int i = 0; i < 10; ++i) {
		f.run_for(60 * 1000);
		occupancy_report(f, i % 2);
	}
	f.run_for(ZigbeeAdapter::kWatchDelayMs);
	CHECK(f.driver.count(kReadIlluminance) == ZigbeeAdapter::kReadTries);
	CHECK(f.driver.count("read 0xaf9e 2 0x0402 0") == ZigbeeAdapter::kReadTries);
	CHECK(capture.sink.count("no answer to the read of illuminance on endpoint 2 within 30 s: read again") ==
		ZigbeeAdapter::kReadTries - 1);
	CHECK(capture.sink.count("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof): no answer to 3 reads of illuminance on "
							 "endpoint 2: its value comes with its next report") == 1);
	CHECK(r.watch(Function::illuminance)->reads() == 0);
	// The occupancy's own reports answered its read at once.
	CHECK(f.driver.count("read 0xaf9e 2 0x0406 0") == 1);
	CHECK(capture.sink.contains("occupancy on endpoint 2 reported off before the answer to its read (read 1 of 3)"));
}

TEST_CASE("ZigbeeRead: a read answered with a failure status is not read again") {
	hc_test::ScopedCapture capture;
	Rebooted r;
	ZigbeeFixture& f = r.f;
	r.first_frame();
	answer(f, kSml002Short, kEndpoint, cluster::kIlluminanceMeasurement, attribute::kMeasuredValue, 0, 0,
		hc::zigbee::zcl_status::kUnsupportedAttribute);
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof): the read of illuminance on endpoint "
								"2 answered status 0x86 (read 1 of 3)"));
	f.run_for(60 * 1000);
	occupancy_report(f);
	f.run_for(ZigbeeAdapter::kWatchDelayMs);
	CHECK(f.driver.count(kReadIlluminance) == 1);
}

TEST_CASE("ZigbeeRead: a listening device's answer is a debug line; the application's read is watched too") {
	hc_test::ScopedCapture capture(hc::LogLevel::debug);
	ZigbeeFixture f;
	f.start_network();
	join_plug(f);
	const hc::Datapoint* on = f.dp("zigbee.001788010f069a57.11.on");
	REQUIRE(on != nullptr);
	f.run_for(2 * ZigbeeAdapter::kRefreshSpacingMs); // the start's reads
	answer(f, kPlugShort, 11, cluster::kOnOff, attribute::kOnOff, zcl_type::kBool, 0);
	f.driver.calls.clear();
	REQUIRE(f.adapter.read(*on));
	CHECK(f.driver.count("read 0x6b21 11 0x0006 0") == 1);
	// No answer; the plug's next frame after kReadAnswerMs reads again.
	f.run_for(ZigbeeAdapter::kReadAnswerMs + PointWatch::kTickMs);
	f.report(kPlugShort, 11, cluster::kLevelControl, attribute::kCurrentLevel, zcl_type::kUint8, 10); // an unmapped frame
	f.run_for(ZigbeeAdapter::kWatchDelayMs);
	CHECK(f.driver.count("read 0x6b21 11 0x0006 0") == 2);
	answer(f, kPlugShort, 11, cluster::kOnOff, attribute::kOnOff, zcl_type::kBool, 1);
	CHECK(capture.sink.contains("I hc.zigbee: 001788010f069a57 (LOM007): on_off on endpoint 11 read: on (read 2 of 3)"));
	// A first answer of a listening device is routine: debug.
	REQUIRE(f.adapter.read(*on));
	answer(f, kPlugShort, 11, cluster::kOnOff, attribute::kOnOff, zcl_type::kBool, 0);
	CHECK(capture.sink.contains("D hc.zigbee: 001788010f069a57 (LOM007): on_off on endpoint 11 read: off (read 1 of 3)"));
}

// ---- Bindings and configurations without an answer (homecontrol #113) ----------------------------

TEST_CASE("ZigbeeBind: a ZDO timeout (0x85) is no refusal: an info line naming TIMEOUT, the binding sent again after "
		  "the next frame") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	join_sml002(f);
	configured(f);
	REQUIRE(f.driver.count("bind 0xaf9e 2 0x0406") == 1);
	uint64_t seen_ms = f.adapter.devices().find(kSml002)->seen_ms;
	f.run_for(5000);
	f.adapter.bind_result(ZigbeeFixture::by_short(kSml002Short), hc::zigbee::zdo_status::kTimeout, kEndpoint,
		cluster::kOccupancySensing);
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof): no answer to the binding of cluster "
								"0x0406 on endpoint 2 to the coordinator (ZDO status 0x85 TIMEOUT): sent again after its "
								"next frame"));
	CHECK_FALSE(capture.sink.contains("refused the binding"));
	// The stack's own answer is no frame of the device.
	CHECK(f.adapter.devices().find(kSml002)->seen_ms == seen_ms);
	CHECK(f.adapter.watch(kSml002, kEndpoint, Function::occupancy)->has(PointWatch::kBindDue));
	f.run_for(60 * 1000);
	CHECK(f.driver.count("bind 0xaf9e 2 0x0406") == 1); // the sensor sleeps: nothing before its frame
	f.driver.calls.clear();
	occupancy_report(f);
	f.run_for(ZigbeeAdapter::kWatchDelayMs);
	CHECK(f.driver.count("bind 0xaf9e 2 0x0406") == 1);
	CHECK(f.driver.count("bind") == 1); // only that binding
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof): the binding of occupancy on endpoint "
								"2 to the coordinator had no answer: sent again after its frame (1 of 3)"));
	// Bounded: three re-sends, then no more.
	for (int i = 0; i < 5; ++i) {
		f.adapter.bind_result(ZigbeeFixture::by_short(kSml002Short), hc::zigbee::zdo_status::kTimeout, kEndpoint,
			cluster::kOccupancySensing);
		f.run_for(60 * 1000);
		occupancy_report(f, i % 2);
		f.run_for(ZigbeeAdapter::kWatchDelayMs);
	}
	CHECK(f.driver.count("bind 0xaf9e 2 0x0406") == ZigbeeAdapter::kTimeoutResends);
	CHECK(capture.sink.contains("(ZDO status 0x85 TIMEOUT): not sent again (4 times without an answer)"));
	// Its success ends the watch of the binding.
	f.adapter.bind_result(ZigbeeFixture::by_short(kSml002Short), hc::zigbee::zdo_status::kSuccess, kEndpoint,
		cluster::kOccupancySensing);
	CHECK(f.adapter.watch(kSml002, kEndpoint, Function::occupancy)->binds() == 0);
}

TEST_CASE("ZigbeeBind: a refused binding (0x8C) is a warning naming TABLE_FULL and is not sent again") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	join_sml002(f);
	configured(f);
	f.adapter.bind_result(ZigbeeFixture::by_short(kSml002Short), 0x8C, kEndpoint, cluster::kIlluminanceMeasurement);
	CHECK(capture.sink.contains("W hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof) refused the binding of cluster "
								"0x0400 on endpoint 2 to the coordinator: ZDO status 0x8c TABLE_FULL"));
	f.driver.calls.clear();
	for (int i = 0; i < 3; ++i) {
		f.run_for(60 * 1000);
		occupancy_report(f, i % 2);
	}
	f.run_for(ZigbeeAdapter::kWatchDelayMs);
	CHECK(f.driver.count("bind 0xaf9e 2 0x0400") == 0);
	// A binding the platform could not name: every binding of the device is concerned.
	f.adapter.bind_result(ZigbeeFixture::by_short(kSml002Short), hc::zigbee::zdo_status::kTimeout);
	CHECK(capture.sink.contains("no answer to a binding to the coordinator (ZDO status 0x85 TIMEOUT): sent again after "
								"its next frame"));
}

TEST_CASE("ZigbeeBind: a configuration without an answer is sent again after the device's next frame, bounded") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	join_sml002(f);
	f.run_for(ZigbeeAdapter::kReportingAnswerMs + ZigbeeAdapter::kCheckPeriodMs);
	const PointWatch* battery = f.adapter.watch(kSml002, kEndpoint, Function::battery);
	REQUIRE(battery->state() == ReportingState::timed_out);
	CHECK(capture.sink.contains("no answer within 30 s to the reporting configuration of battery on endpoint 2 (cluster "
								"0x0001, attribute 0x0021): sent again after its next frame"));
	f.driver.calls.clear();
	occupancy_report(f);
	f.run_for(ZigbeeAdapter::kWatchDelayMs);
	// One point per frame, its binding and configuration (kSleepyBurst, homecontrol #117).
	CHECK(f.driver.count("configure 0xaf9e") == 1);
	CHECK(f.driver.count("bind 0xaf9e") == 1);
	rounds(f, 3);
	CHECK(f.driver.count("configure 0xaf9e") == 4); // the four timed-out points
	CHECK(battery->state() == ReportingState::requested);
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof): the reporting configuration of "
								"battery on endpoint 2 had no answer: sent again after its frame (1 of 3)"));
	// Each time without an answer: again after a frame, three times in all.
	for (int i = 0; i < 6; ++i) {
		f.run_for(ZigbeeAdapter::kReportingAnswerMs + ZigbeeAdapter::kCheckPeriodMs);
		occupancy_report(f, i % 2);
		f.run_for(ZigbeeAdapter::kWatchDelayMs);
		rounds(f, 3);
	}
	CHECK(f.driver.count("configure 0xaf9e 2 0x0001") == ZigbeeAdapter::kTimeoutResends);
	CHECK(capture.sink.contains("battery on endpoint 2 (cluster 0x0001, attribute 0x0021): not sent again (4 times "
								"without an answer)"));
	// A late answer still counts.
	f.adapter.reporting_configured(ZigbeeFixture::by_short(kSml002Short), kEndpoint, cluster::kPowerConfiguration, 0);
	CHECK(battery->state() == ReportingState::configured);
}
