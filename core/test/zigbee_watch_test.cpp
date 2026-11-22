/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The reporting watch (homecontrol #108): the state of each point's configuration from the device's
// answer, the re-send after a silence right after a frame of the device, the warning when the repeats
// are exhausted, the start's reads and configuration of a sleepy device deferred to its first frame;
// and the saved short address of a rejoin (homecontrol #109). The house controller's Philips SML002
// motion sensor (a sleepy end device behind the plug LOM007) is the fixture.

#include <string>
#include <vector>

#include "doctest.h"
#include "hc/zigbee/adapter.h"
#include "test_support.h"
#include "zigbee_test_support.h"

using hc::ZigbeeNeighbor;
using hc::zigbee::Function;
using hc::zigbee::PointWatch;
using hc::zigbee::Receiver;
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

const char* const kConfigureTemperature = "configure 0xaf9e 2 0x0402 0x0000 30..600 10";
const char* const kBindTemperature = "bind 0xaf9e 2 0x0402";

void join_sml002(ZigbeeFixture& f) {
	f.adapter.device_announced(ZigbeeFixture::node(kSml002, kSml002Short), 0x80); // an end device, receiver off
	uint8_t endpoints[] = {kEndpoint};
	f.adapter.active_endpoints(ZigbeeFixture::by_short(kSml002Short), 0, endpoints, 1);
	f.descriptor(kSml002Short, kEndpoint, hc::zigbee::kHomeAutomationProfile, 0x0107,
		{0x0000, 0x0001, 0x0003, 0x0406, 0x0400, 0x0402}, {0x0019});
	f.basic(kSml002Short, kEndpoint, "Philips", "SML002");
	REQUIRE(f.adapter.interviewing() == 0);
	REQUIRE(f.adapter.set_label(kSml002, "Bewegungsmelder Hof"));
}

void join_plug(ZigbeeFixture& f) {
	f.adapter.device_announced(ZigbeeFixture::node(kPlug, kPlugShort), 0x8e); // a mains-powered router
	uint8_t endpoints[] = {11};
	f.adapter.active_endpoints(ZigbeeFixture::by_short(kPlugShort), 0, endpoints, 1);
	f.descriptor(kPlugShort, 11, hc::zigbee::kHomeAutomationProfile, 0x010A, {0x0000, 0x0003, 0x0006}, {0x0019});
	f.basic(kPlugShort, 11, "Signify Netherlands B.V.", "LOM007");
	REQUIRE(f.adapter.interviewing() == 0);
}

/// The sensor's answers to its four configurations (illuminance, occupancy, temperature, battery).
void answer_all(ZigbeeFixture& f, uint8_t status = 0) {
	for (uint16_t c : {cluster::kIlluminanceMeasurement, cluster::kOccupancySensing, cluster::kTemperatureMeasurement,
			 cluster::kPowerConfiguration}) {
		f.adapter.reporting_configured(ZigbeeFixture::by_short(kSml002Short), kEndpoint, c, status);
	}
}

/// An illuminance and an occupancy report: frames of the sensor that are not the temperature's.
void report_illuminance(ZigbeeFixture& f) {
	f.report(kSml002Short, kEndpoint, cluster::kIlluminanceMeasurement, attribute::kMeasuredValue, zcl_type::kUint16,
		12000);
	f.report(kSml002Short, kEndpoint, cluster::kOccupancySensing, attribute::kOccupancy, zcl_type::kBitmap8, 0);
}

void report_temperature(ZigbeeFixture& f) {
	f.report(kSml002Short, kEndpoint, cluster::kTemperatureMeasurement, attribute::kMeasuredValue, zcl_type::kInt16, 595);
}

const PointWatch* temperature(const ZigbeeFixture& f) {
	return f.adapter.watch(kSml002, kEndpoint, Function::temperature);
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

/// The device's frames every `every_ms` (an illuminance and an occupancy report each) for `total_ms`.
void frames(ZigbeeFixture& f, uint64_t every_ms, uint64_t total_ms) {
	for (uint64_t t = 0; t < total_ms; t += every_ms) {
		f.run_for(every_ms);
		report_illuminance(f);
	}
}

std::string database_text(const ZigbeeFixture& f) {
	auto it = f.store.blobs.find(hc::zigbee::DeviceDatabase::kBlobName);
	return it == f.store.blobs.end() ? std::string() : std::string(it->second.begin(), it->second.end());
}

} // namespace

// ---- The state of the configuration -------------------------------------------------------------

TEST_CASE("ZigbeeWatch: the interview's configuration is requested, then configured by the device's answer") {
	ZigbeeFixture f;
	f.start_network();
	join_sml002(f);
	REQUIRE(temperature(f) != nullptr);
	CHECK(temperature(f)->state() == ReportingState::requested);
	CHECK(contains(view_of(f.adapter, kSml002), "{\"datapoint\":\"zigbee.0017880106498a49.2.temperature\","
		"\"function\":\"temperature\",\"endpoint\":2,\"cluster\":1026,\"attribute\":0,\"max_s\":600,"
		"\"reporting\":\"requested\"}"));
	answer_all(f);
	CHECK(temperature(f)->state() == ReportingState::configured);
	report_temperature(f);
	uint64_t reported_ms = f.clock.now_ms;
	f.run_for(42 * 1000);
	// In steps of the watch's tick (10 s).
	uint64_t ago = (f.clock.now_ms / 10000 - reported_ms / 10000) * 10;
	std::string view = view_of(f.adapter, kSml002);
	CHECK(contains(view, "\"max_s\":600,\"reporting\":\"configured\",\"last_report_ago_s\":" + std::to_string(ago) + "}"));
	CHECK(ago >= 40);
	CHECK(ago <= 50);
	// Four points, each with its state (illuminance, occupancy, temperature, battery).
	size_t points = 0;
	for (size_t at = view.find("\"reporting\":\"configured\""); at != std::string::npos;
		 at = view.find("\"reporting\":\"configured\"", at + 1)) {
		++points;
	}
	CHECK(points == 4);
}

TEST_CASE("ZigbeeWatch: a refused configuration shows its status and a warning; it is not sent again") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	join_sml002(f);
	f.adapter.reporting_configured(ZigbeeFixture::by_short(kSml002Short), kEndpoint, cluster::kTemperatureMeasurement,
		0x86, attribute::kMeasuredValue); // UNSUPPORTED_ATTRIBUTE
	CHECK(temperature(f)->state() == ReportingState::refused);
	CHECK(temperature(f)->status == 0x86);
	CHECK(capture.sink.contains("W hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof) refused the reporting configuration "
								"of temperature on endpoint 2 (cluster 0x0402, attribute 0x0000): status 0x86; its values "
								"come when it reports on its own or is read"));
	CHECK(contains(view_of(f.adapter, kSml002), "\"function\":\"temperature\",\"endpoint\":2,\"cluster\":1026,"
		"\"attribute\":0,\"max_s\":600,\"reporting\":\"refused\",\"status\":134}"));
	// A refusal without the attribute names the cluster's point.
	f.adapter.reporting_configured(ZigbeeFixture::by_short(kSml002Short), kEndpoint, cluster::kOccupancySensing, 0x8c);
	CHECK(f.adapter.watch(kSml002, kEndpoint, Function::occupancy)->state() == ReportingState::refused);
	CHECK(capture.sink.contains("refused the reporting configuration of occupancy on endpoint 2 (cluster 0x0406, "
								"attribute 0x0000): status 0x8c"));
	// The device is heard for hours, the temperature never reports: a refusal is not sent again.
	f.driver.calls.clear();
	frames(f, 300 * 1000, 4 * 3600 * 1000);
	CHECK(f.driver.count("configure 0xaf9e 2 0x0402") == 0);
	CHECK(f.driver.count("configure 0xaf9e 2 0x0406") == 0);
}

TEST_CASE("ZigbeeWatch: an unanswered configuration times out, a late answer still counts") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	join_sml002(f);
	f.run_for(ZigbeeAdapter::kReportingAnswerMs - 10000);
	CHECK(temperature(f)->state() == ReportingState::requested);
	f.run_for(ZigbeeAdapter::kCheckPeriodMs + 10000);
	CHECK(temperature(f)->state() == ReportingState::timed_out);
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof): no answer within 30 s to the "
								"reporting configuration of temperature on endpoint 2 (cluster 0x0402, attribute 0x0000)"));
	CHECK(contains(view_of(f.adapter, kSml002), "\"reporting\":\"timed_out\""));
	answer_all(f);
	CHECK(temperature(f)->state() == ReportingState::configured);
	CHECK(capture.sink.contains("the reporting configuration of temperature on endpoint 2 answered late: configured"));
}

// ---- The watch ----------------------------------------------------------------------------------

TEST_CASE("ZigbeeWatch: a point silent beyond 2 x max while its device sends gets its configuration again, once, "
		  "after a frame") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	join_sml002(f);
	answer_all(f);
	f.driver.calls.clear();
	// The sensor's other points report every 5 min; the temperature stays silent (the house controller,
	// 2026-11-22). Its answer to our read does not count as a report.
	hc::zigbee::AttributeRecord record;
	uint8_t data[2] = {0x53, 0x02};
	record.attribute = attribute::kMeasuredValue;
	record.type = zcl_type::kInt16;
	record.data = data;
	record.length = 2;
	f.adapter.attributes(ZigbeeFixture::by_short(kSml002Short), kEndpoint, cluster::kTemperatureMeasurement, &record, 1,
		false);
	frames(f, 300 * 1000, 900 * 1000); // 15 min: not yet 2 x 600 s
	CHECK(f.driver.count(kConfigureTemperature) == 0);
	f.run_for(300 * 1000); // 20 min of silence, but no frame yet: nothing goes out
	CHECK(f.driver.count(kConfigureTemperature) == 0);
	report_illuminance(f); // the frame
	CHECK(f.driver.count(kConfigureTemperature) == 0); // after the frame's own reports
	f.run_for(ZigbeeAdapter::kWatchDelayMs);
	CHECK(f.driver.count(kConfigureTemperature) == 1);
	CHECK(f.driver.count(kBindTemperature) == 1);
	CHECK(f.driver.count("configure") == 1); // only the silent point
	CHECK(f.driver.count("bind") == 1);
	CHECK(temperature(f)->repeats() == 1);
	CHECK(temperature(f)->state() == ReportingState::requested);
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof): temperature on endpoint 2 (cluster "
								"0x0402, attribute 0x0000) has not reported for 1200 s (max 600 s) while the device is "
								"heard: binding and reporting configuration sent again (1 of 3)"));
	CHECK(contains(view_of(f.adapter, kSml002), "\"reporting\":\"requested\",\"repeats\":1}"));
	// Once: the following frames send nothing until the next interval with the backoff (3 x max).
	f.adapter.reporting_configured(ZigbeeFixture::by_short(kSml002Short), kEndpoint, cluster::kTemperatureMeasurement, 0);
	frames(f, 60 * 1000, 1740 * 1000);
	CHECK(f.driver.count(kConfigureTemperature) == 1);
	frames(f, 60 * 1000, 120 * 1000);
	CHECK(f.driver.count(kConfigureTemperature) == 2);
	// A report ends the repeats.
	report_temperature(f);
	CHECK(temperature(f)->repeats() == 0);
	CHECK(capture.sink.contains("temperature on endpoint 2 reports again (its reporting configuration was sent again 2 "
								"time(s))"));
}

TEST_CASE("ZigbeeWatch: a point that never reports after the repeats is a warning naming device, cluster and "
		  "attribute") {
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.start_network();
	join_sml002(f);
	answer_all(f);
	f.driver.calls.clear();
	// Silent through the three repeats: 2, 3, 4 and then 5 times its maximum after each sending.
	frames(f, 300 * 1000, (2 + 3 + 4 + 5) * 600 * 1000 + 300 * 1000);
	CHECK(f.driver.count(kConfigureTemperature) == 3);
	CHECK(capture.sink.count("W hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof): temperature on endpoint 2 (cluster "
							 "0x0402, attribute 0x0000) has not reported for 3000 s although the device is heard, after 3 "
							 "repeats of its reporting configuration (max 600 s): its value comes only from reads") == 1);
	REQUIRE(temperature(f)->has(PointWatch::kSilent));
	CHECK(contains(view_of(f.adapter, kSml002), "\"repeats\":3,\"silent\":true}"));
	// Silent from then on: no more sends, no more warnings.
	frames(f, 300 * 1000, 6 * 3600 * 1000);
	CHECK(f.driver.count(kConfigureTemperature) == 3);
	CHECK(capture.sink.count("has not reported for") == 4); // the three repeats and the one warning
	// The other points kept reporting: they were never sent again.
	CHECK(f.driver.count("configure 0xaf9e 2 0x0400") == 0);
	// Its report ends the silence.
	report_temperature(f);
	CHECK_FALSE(temperature(f)->has(PointWatch::kSilent));
	CHECK_FALSE(contains(view_of(f.adapter, kSml002), "\"silent\""));
}

TEST_CASE("ZigbeeWatch: one device at a time: another device's re-send waits until the first one's answers came") {
	ZigbeeFixture f;
	f.start_network();
	join_sml002(f);
	f.join_sensor(); // temperature, humidity, battery on endpoint 1 (0x3c4d)
	answer_all(f);
	for (uint16_t c : {cluster::kTemperatureMeasurement, cluster::kRelativeHumidity, cluster::kPowerConfiguration}) {
		f.adapter.reporting_configured(ZigbeeFixture::by_short(hc_test::kSensorShort), 1, c, 0);
	}
	f.driver.calls.clear();
	// Both temperatures silent for 20 min while the devices send other frames.
	for (int i = 0; i < 4; ++i) {
		f.run_for(300 * 1000);
		report_illuminance(f);
		f.report(hc_test::kSensorShort, 1, cluster::kRelativeHumidity, attribute::kMeasuredValue, zcl_type::kUint16, 4500);
	}
	f.run_for(ZigbeeAdapter::kWatchDelayMs);
	CHECK(f.driver.count(kConfigureTemperature) == 1);
	CHECK(f.driver.count("configure 0x3c4d 1 0x0402") == 0); // waits: the first device has not answered
	// Its frame while the first device's answer is outstanding sends nothing.
	f.report(hc_test::kSensorShort, 1, cluster::kRelativeHumidity, attribute::kMeasuredValue, zcl_type::kUint16, 4500);
	f.run_for(ZigbeeAdapter::kWatchDelayMs);
	CHECK(f.driver.count("configure 0x3c4d 1 0x0402") == 0);
	// The first device answers: the next frame of the second sends its configuration.
	f.adapter.reporting_configured(ZigbeeFixture::by_short(kSml002Short), kEndpoint, cluster::kTemperatureMeasurement, 0);
	f.report(hc_test::kSensorShort, 1, cluster::kRelativeHumidity, attribute::kMeasuredValue, zcl_type::kUint16, 4500);
	f.run_for(ZigbeeAdapter::kWatchDelayMs);
	CHECK(f.driver.count("configure 0x3c4d 1 0x0402") == 1);
}

// ---- The start of a sleepy device ---------------------------------------------------------------

TEST_CASE("ZigbeeWatch: a sleepy device's read on start goes out after its first frame, with its configuration") {
	hc_test::FakeBlobStore store;
	{
		ZigbeeFixture f;
		f.start_network();
		join_plug(f);
		join_sml002(f);
		store = f.store;
		CHECK(contains(database_text(f), "\"rx_on_when_idle\":false"));
		CHECK(contains(database_text(f), "\"rx_on_when_idle\":true"));
	}
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.store = store;
	f.start_network();
	CHECK(f.adapter.devices().find(kSml002)->receiver == Receiver::off_when_idle);
	CHECK(capture.sink.contains("I hc.zigbee: 1 device(s) that may sleep are read and their reporting configured when "
								"next heard"));
	f.run_for(10 * ZigbeeAdapter::kRefreshSpacingMs);
	CHECK(f.driver.count("read 0x6b21") == 1); // the plug listens: read and configured at the start
	CHECK(f.driver.count("configure 0x6b21") == 1);
	CHECK(f.driver.count("read 0xaf9e") == 0); // the sensor sleeps: nothing yet
	CHECK(f.driver.count("configure 0xaf9e") == 0);
	CHECK(contains(view_of(f.adapter, kSml002), "\"reporting\":\"not_requested\""));
	// Its first frame (an occupancy report): the reads and the configuration follow, kSleepyBurst
	// requests per frame of the sensor (homecontrol #117): its answers are its next frames.
	f.report(kSml002Short, kEndpoint, cluster::kOccupancySensing, attribute::kOccupancy, zcl_type::kBitmap8, 1);
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof) heard (short 0xaf9e): reading its "
								"datapoints and configuring its reporting now"));
	f.run_for(ZigbeeAdapter::kRefreshSpacingMs);
	CHECK(f.driver.count("read 0xaf9e") == ZigbeeAdapter::kSleepyBurst);
	CHECK(f.driver.count("configure 0xaf9e") == 0);
	CHECK(capture.sink.contains("I hc.zigbee: 0017880106498a49 (Bewegungsmelder Hof): the reporting of 0 attribute(s) "
								"requested, 4 more after its next frames (it sleeps: 3 requests per frame of its own)"));
	for (int i = 0; i < 4; ++i) {
		f.report(kSml002Short, kEndpoint, cluster::kBasic, 0x4000, zcl_type::kUint8, 1); // its answers
		f.run_for(ZigbeeAdapter::kWatchDelayMs);
		CHECK(f.driver.count("configure 0xaf9e") == static_cast<size_t>(i + 1));
	}
	CHECK(f.driver.count("read 0xaf9e 2 0x0402 0") == 1);
	CHECK(f.driver.count("read 0xaf9e") == 4);
	CHECK(f.driver.count("bind 0xaf9e") == 4);
	CHECK(f.driver.has(kConfigureTemperature));
	answer_all(f);
	std::string view = view_of(f.adapter, kSml002);
	CHECK_FALSE(contains(view, "\"reporting\":\"not_requested\""));
	CHECK_FALSE(contains(view, "\"reporting\":\"requested\""));
	// Further frames read nothing more.
	f.report(kSml002Short, kEndpoint, cluster::kOccupancySensing, attribute::kOccupancy, zcl_type::kBitmap8, 0);
	f.run_for(ZigbeeAdapter::kRefreshSpacingMs);
	CHECK(f.driver.count("read 0xaf9e") == 4);
}

TEST_CASE("ZigbeeWatch: the receiver comes from the neighbour tables too and is saved") {
	ZigbeeFixture f;
	f.start_network();
	join_plug(f);
	// Joined before the receiver was kept: announces without their capability.
	f.join_sensor();
	f.adapter.device_announced(ZigbeeFixture::node(kSml002, kSml002Short));
	REQUIRE(f.adapter.devices().find(kSml002)->receiver == Receiver::unknown);
	REQUIRE(f.adapter.devices().find(hc_test::kSensor)->receiver == Receiver::unknown);
	// The plug's table (the topology pass after the announce) lists the motion sensor.
	f.run_for(ZigbeeAdapter::kTopologyDelayMs);
	REQUIRE(f.driver.has("lqi 0x6b21 0"));
	ZigbeeNeighbor child;
	child.ieee = kSml002;
	child.short_addr = kSml002Short;
	child.relationship = ZigbeeNeighbor::Relationship::child;
	child.receiver = Receiver::off_when_idle;
	f.adapter.neighbor_table(ZigbeeFixture::node(kPlug, kPlugShort), 0, 1, 0, &child, 1);
	CHECK(f.adapter.devices().find(kSml002)->receiver == Receiver::off_when_idle);
	// The coordinator's own table lists the other sensor as its sleepy child.
	ZigbeeNeighbor sensor;
	sensor.ieee = hc_test::kSensor;
	sensor.short_addr = hc_test::kSensorShort;
	sensor.relationship = ZigbeeNeighbor::Relationship::child;
	sensor.receiver = Receiver::off_when_idle;
	f.driver.neighbors = {sensor};
	f.run_for(ZigbeeAdapter::kCheckPeriodMs);
	CHECK(f.adapter.devices().find(hc_test::kSensor)->receiver == Receiver::off_when_idle);
	f.run_for(ZigbeeAdapter::kCheckPeriodMs);
	CHECK(contains(database_text(f), "\"ieee\":\"0017880106498a49\",\"short\":44958,\"manufacturer\":\"\","
		"\"model\":\"\",\"label\":\"\",\"interviewed\":false,\"rx_on_when_idle\":false"));
}

// ---- The short address of a rejoin (homecontrol #109) -------------------------------------------

TEST_CASE("ZigbeeShort: an announce with a new address is saved and the restart starts from it") {
	hc_test::FakeBlobStore store;
	{
		ZigbeeFixture f;
		f.start_network();
		join_sml002(f);
		size_t writes = f.store.writes_of[hc::zigbee::DeviceDatabase::kBlobName];
		f.adapter.device_announced(ZigbeeFixture::node(kSml002, 0xab51), 0x80);
		// Bounded: a second change before the check shares its write.
		f.adapter.device_announced(ZigbeeFixture::node(kSml002, 0x7d31), 0x80);
		CHECK(f.store.writes_of[hc::zigbee::DeviceDatabase::kBlobName] == writes);
		f.run_for(ZigbeeAdapter::kCheckPeriodMs);
		CHECK(f.store.writes_of[hc::zigbee::DeviceDatabase::kBlobName] == writes + 1);
		// Nothing changed: no further write.
		f.run_for(5 * ZigbeeAdapter::kCheckPeriodMs);
		CHECK(f.store.writes_of[hc::zigbee::DeviceDatabase::kBlobName] == writes + 1);
		store = f.store;
	}
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.store = store;
	f.start_network();
	CHECK(f.adapter.devices().find(kSml002)->short_addr == 0x7d31);
	// The next rejoin logs the address the previous boot learnt.
	f.adapter.device_announced(ZigbeeFixture::node(kSml002, 0x9061), 0x80);
	CHECK(capture.sink.contains("rejoined with a new address: short 0x7d31 -> 0x9061"));
}

TEST_CASE("ZigbeeShort: a short address the trust center's notice moves, and one another device held, are saved") {
	hc_test::FakeBlobStore store;
	{
		ZigbeeFixture f;
		f.start_network();
		join_plug(f);
		join_sml002(f);
		// The trust center's notice gives the sensor an address the database still has for the plug (an
		// outdated entry): the sensor takes it, the plug's entry loses it; both are saved.
		f.adapter.device_updated(ZigbeeFixture::node(kSml002, kPlugShort), ZigbeeAdapter::kUpdateSecuredRejoin,
			ZigbeeAdapter::kTcAuthorize, kPlugShort);
		f.run_for(ZigbeeAdapter::kCheckPeriodMs);
		store = f.store;
	}
	ZigbeeFixture f;
	f.store = store;
	REQUIRE(f.adapter.start());
	CHECK(f.adapter.devices().find(kSml002)->short_addr == kPlugShort);
	CHECK(f.adapter.devices().find(kPlug)->short_addr == hc::zigbee::kNoShortAddress);
}

// ---- Probes: the watch composed with the other mechanisms ---------------------------------------

TEST_CASE("ZigbeeWatch probe: an override without a periodic report is never sent again; a changed one starts afresh") {
	ZigbeeFixture f;
	f.start_network();
	hc::zigbee::ZigbeeOverride binding;
	binding.name = "bath.temperature";
	binding.ieee = hc_test::kSensor;
	binding.endpoint = 1;
	binding.cluster = cluster::kTemperatureMeasurement;
	binding.attribute = attribute::kMeasuredValue;
	binding.reporting.set = hc::zigbee::ReportingOverride::kMax;
	binding.reporting.max_s = 0; // on change only: no promise to check
	REQUIRE(f.adapter.configure(&binding, 1));
	f.join_sensor();
	f.adapter.reporting_configured(ZigbeeFixture::by_short(hc_test::kSensorShort), 1, cluster::kTemperatureMeasurement, 0);
	CHECK(contains(view_of(f.adapter, hc_test::kSensor), "{\"datapoint\":\"bath.temperature\",\"function\":\"temperature\","
		"\"endpoint\":1,\"cluster\":1026,\"attribute\":0,\"max_s\":0,\"reporting\":\"configured\"}"));
	f.driver.calls.clear();
	// Heard every 5 min for a day (a read's answer is a frame), the temperature never reports.
	hc::zigbee::AttributeRecord record;
	uint8_t data[2] = {0x10, 0x00};
	record.attribute = attribute::kZoneType; // an unmapped attribute of the Basic cluster: just a frame
	record.type = zcl_type::kUint16;
	record.data = data;
	record.length = 2;
	for (int i = 0; i < 288; ++i) {
		f.run_for(300 * 1000);
		f.adapter.attributes(ZigbeeFixture::by_short(hc_test::kSensorShort), 1, cluster::kBasic, &record, 1, false);
	}
	CHECK(f.driver.count("configure") == 0);
	// A periodic override: configured at once (the binding changed), watched from its sending.
	binding.reporting.max_s = 900;
	REQUIRE(f.adapter.configure(&binding, 1));
	CHECK(f.driver.count("configure 0x3c4d 1 0x0402 0x0000 30..900 10") == 1);
	f.adapter.reporting_configured(ZigbeeFixture::by_short(hc_test::kSensorShort), 1, cluster::kTemperatureMeasurement, 0);
	for (int i = 0; i < 6; ++i) { // 30 min = 2 x 900 s
		f.run_for(300 * 1000);
		f.adapter.attributes(ZigbeeFixture::by_short(hc_test::kSensorShort), 1, cluster::kBasic, &record, 1, false);
	}
	f.run_for(ZigbeeAdapter::kWatchDelayMs);
	CHECK(f.driver.count("configure 0x3c4d 1 0x0402 0x0000 30..900 10") == 2);
}

TEST_CASE("ZigbeeWatch probe: the first boot of an old database: a sleepy device of unknown receiver is read at the "
		  "start and configured at its first frame") {
	hc_test::FakeBlobStore store;
	{
		ZigbeeFixture f;
		f.start_network();
		join_sml002(f);
		store = f.store;
	}
	// The database as the firmware before #108 wrote it: no receiver.
	std::string text(store.blobs[hc::zigbee::DeviceDatabase::kBlobName].begin(),
		store.blobs[hc::zigbee::DeviceDatabase::kBlobName].end());
	size_t at = text.find(",\"rx_on_when_idle\":false");
	REQUIRE(at != std::string::npos);
	text.erase(at, std::string(",\"rx_on_when_idle\":false").size());
	store.blobs[hc::zigbee::DeviceDatabase::kBlobName] = std::vector<uint8_t>(text.begin(), text.end());
	ZigbeeFixture f;
	f.store = store;
	f.start_network();
	REQUIRE(f.adapter.devices().find(kSml002)->receiver == Receiver::unknown);
	f.run_for(ZigbeeAdapter::kRefreshSpacingMs);
	CHECK(f.driver.count("read 0xaf9e") == 4); // it may listen
	CHECK(f.driver.count("configure 0xaf9e") == 0); // but its configuration waits for its frame
	f.run_for(600 * 1000);
	report_illuminance(f);
	f.run_for(ZigbeeAdapter::kRefreshSpacingMs);
	CHECK(f.driver.count("read 0xaf9e") == 8);
	CHECK(f.driver.count("configure 0xaf9e") == 4);
	// One answer comes; the others time out (and are sent again by the next refresh or after a silence).
	f.adapter.reporting_configured(ZigbeeFixture::by_short(kSml002Short), kEndpoint, cluster::kTemperatureMeasurement, 0);
	f.run_for(ZigbeeAdapter::kReportingAnswerMs + ZigbeeAdapter::kCheckPeriodMs);
	CHECK(f.adapter.watch(kSml002, kEndpoint, Function::battery)->state() == ReportingState::timed_out);
	CHECK(temperature(f)->state() == ReportingState::configured);
}

TEST_CASE("ZigbeeWatch probe: the stack refuses the start's configuration: the device's next frame sends it") {
	hc_test::FakeBlobStore store;
	{
		ZigbeeFixture f;
		f.start_network();
		join_plug(f);
		store = f.store;
	}
	hc_test::ScopedCapture capture;
	ZigbeeFixture f;
	f.store = store;
	f.start_network();
	f.driver.accept = false; // the stack's lock is busy
	f.run_for(ZigbeeAdapter::kRefreshSpacingMs);
	CHECK(f.driver.count("configure 0x6b21") == 1);
	CHECK(capture.sink.contains("the stack refused 1 of 1 reporting configuration(s): those are sent again after its next "
								"frame"));
	CHECK(contains(view_of(f.adapter, kPlug), "\"reporting\":\"not_requested\""));
	f.driver.accept = true;
	f.report(kPlugShort, 11, cluster::kOnOff, attribute::kOnOff, zcl_type::kBool, 1);
	f.run_for(ZigbeeAdapter::kRefreshSpacingMs);
	CHECK(f.driver.count("configure 0x6b21") == 2);
	CHECK(contains(view_of(f.adapter, kPlug), "\"reporting\":\"requested\""));
}

TEST_CASE("ZigbeeWatch probe: the reads of the start wait while the network is down and go out when it is up") {
	hc_test::FakeBlobStore store;
	{
		ZigbeeFixture f;
		f.start_network();
		join_plug(f);
		store = f.store;
	}
	ZigbeeFixture f;
	f.store = store;
	REQUIRE(f.adapter.start());
	f.run_for(60 * 1000);
	CHECK(f.driver.count("read") == 0);
	CHECK(f.driver.count("configure") == 0);
	f.adapter.network_state(true, 0x1a62, 15, 0xdddddddddddddddd);
	f.run_for(ZigbeeAdapter::kRefreshSpacingMs);
	CHECK(f.driver.count("read 0x6b21") == 1);
	CHECK(f.driver.count("configure 0x6b21") == 1);
}

TEST_CASE("ZigbeeWatch probe: the 16-bit tick clock wraps after 7.6 days without a false re-send or a missed one") {
	ZigbeeFixture f;
	f.start_network();
	join_sml002(f);
	answer_all(f);
	f.driver.calls.clear();
	// Every point reports every 5 min for 8 days (65536 ticks of 10 s are 7.6 days).
	for (int i = 0; i < 8 * 288; ++i) {
		f.run_for(300 * 1000);
		report_illuminance(f);
		report_temperature(f);
		f.report(kSml002Short, kEndpoint, cluster::kPowerConfiguration, attribute::kBatteryPercentage, zcl_type::kUint8,
			200);
	}
	CHECK(f.driver.count("configure") == 0);
	CHECK(contains(view_of(f.adapter, kSml002), "\"function\":\"temperature\",\"endpoint\":2,\"cluster\":1026,"
		"\"attribute\":0,\"max_s\":600,\"reporting\":\"configured\",\"last_report_ago_s\":0}"));
	// Then the temperature falls silent: sent again once, after 2 x 600 s.
	for (int i = 0; i < 6; ++i) {
		f.run_for(300 * 1000);
		report_illuminance(f);
	}
	f.run_for(ZigbeeAdapter::kWatchDelayMs);
	CHECK(f.driver.count(kConfigureTemperature) == 1);
	CHECK(f.driver.count("configure") == 1);
}
