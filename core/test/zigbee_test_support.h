/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// A recording ZigbeeDriver and a fixture with the adapter under the fake clock.
#ifndef HC_ZIGBEE_TEST_SUPPORT_H
#define HC_ZIGBEE_TEST_SUPPORT_H

#include <cstdio>
#include <string>
#include <vector>

#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/datapoint.h"
#include "hc/event_loop.h"
#include "hc/platform/zigbee_driver.h"
#include "hc/zigbee/adapter.h"
#include "test_support.h"

namespace hc_test {

/// Records every request as a line ("active 0x1234", "read 0x1234 1 0x0000 4,5", ...); `accept`
/// false refuses them all.
class FakeZigbeeDriver : public hc::ZigbeeDriver {
public:
	bool permit_join(uint8_t seconds) override { return record("permit " + std::to_string(seconds)); }
	bool request_active_endpoints(hc::zigbee::Ieee, uint16_t short_addr) override {
		return record("active " + hex4(short_addr));
	}
	bool request_simple_descriptor(hc::zigbee::Ieee, uint16_t short_addr, uint8_t endpoint) override {
		return record("simple " + hex4(short_addr) + " " + std::to_string(endpoint));
	}
	bool read_attributes(hc::zigbee::Ieee, uint16_t short_addr, uint8_t endpoint, uint16_t cluster,
		const uint16_t* attributes, size_t count) override {
		std::string line = "read " + hex4(short_addr) + " " + std::to_string(endpoint) + " " + hex4(cluster) + " ";
		for (size_t i = 0; i < count; ++i) {
			line += (i > 0 ? "," : "") + std::to_string(attributes[i]);
		}
		return record(line);
	}
	bool configure_reporting(hc::zigbee::Ieee, uint16_t short_addr, uint8_t endpoint, uint16_t cluster,
		const hc::ReportingConfig& config) override {
		return record("configure " + hex4(short_addr) + " " + std::to_string(endpoint) + " " + hex4(cluster) + " " +
			hex4(config.attribute) + " " + std::to_string(config.min_s) + ".." + std::to_string(config.max_s) + " " +
			std::to_string(config.change));
	}
	bool bind(hc::zigbee::Ieee, uint16_t short_addr, uint8_t endpoint, uint16_t cluster) override {
		return record("bind " + hex4(short_addr) + " " + std::to_string(endpoint) + " " + hex4(cluster));
	}
	bool ias_enroll(hc::zigbee::Ieee, uint16_t short_addr, uint8_t endpoint) override {
		return record("enroll " + hex4(short_addr) + " " + std::to_string(endpoint));
	}
	bool on_off(hc::zigbee::Ieee, uint16_t short_addr, uint8_t endpoint, hc::OnOffCommand command) override {
		const char* name = command == hc::OnOffCommand::on ? "on" : command == hc::OnOffCommand::off ? "off" : "toggle";
		return record("onoff " + hex4(short_addr) + " " + std::to_string(endpoint) + " " + name);
	}
	bool move_to_level(hc::zigbee::Ieee, uint16_t short_addr, uint8_t endpoint, uint8_t level,
		uint16_t transition_ds) override {
		return record("level " + hex4(short_addr) + " " + std::to_string(endpoint) + " " + std::to_string(level) + " " +
			std::to_string(transition_ds));
	}
	bool move_to_color_temperature(hc::zigbee::Ieee, uint16_t short_addr, uint8_t endpoint, uint16_t mireds,
		uint16_t transition_ds) override {
		return record("ct " + hex4(short_addr) + " " + std::to_string(endpoint) + " " + std::to_string(mireds) + " " +
			std::to_string(transition_ds));
	}
	bool identify(hc::zigbee::Ieee, uint16_t short_addr, uint8_t endpoint, uint16_t seconds) override {
		return record("identify " + hex4(short_addr) + " " + std::to_string(endpoint) + " " + std::to_string(seconds));
	}
	bool leave(hc::zigbee::Ieee, uint16_t short_addr) override { return record("leave " + hex4(short_addr)); }

	static std::string hex4(uint16_t value) {
		char text[8];
		std::snprintf(text, sizeof(text), "0x%04x", static_cast<unsigned>(value));
		return text;
	}

	/// Number of recorded lines starting with `prefix`.
	size_t count(const std::string& prefix) const {
		size_t n = 0;
		for (const std::string& line : calls) {
			n += line.compare(0, prefix.size(), prefix) == 0 ? 1 : 0;
		}
		return n;
	}

	bool has(const std::string& line) const {
		for (const std::string& c : calls) {
			if (c == line) {
				return true;
			}
		}
		return false;
	}

	std::vector<std::string> calls;
	bool accept = true;

private:
	bool record(const std::string& line) {
		calls.push_back(line);
		return accept;
	}
};

constexpr hc::zigbee::Ieee kBulb = 0x00124b0001020304ull;
constexpr uint16_t kBulbShort = 0x1a2b;
constexpr hc::zigbee::Ieee kSensor = 0x00158d0005060708ull;
constexpr uint16_t kSensorShort = 0x3c4d;
constexpr hc::zigbee::Ieee kContact = 0x00158d00090a0b0cull;
constexpr uint16_t kContactShort = 0x5e6f;
constexpr hc::zigbee::Ieee kButton = 0x000b57fffe112233ull;
constexpr uint16_t kButtonShort = 0x7a8b;

/// A ZCL character string with its length octet.
inline std::vector<uint8_t> zcl_string(const std::string& text) {
	std::vector<uint8_t> bytes;
	bytes.push_back(static_cast<uint8_t>(text.size()));
	bytes.insert(bytes.end(), text.begin(), text.end());
	return bytes;
}

/// The adapter with its model, loop, blob store and driver; helpers that play a device's part.
struct ZigbeeFixture {
	FakeClock clock;
	hc::EventLoop loop;
	hc::DatapointModel model{clock};
	FakeBlobStore store;
	FakeZigbeeDriver driver;
	hc::zigbee::ZigbeeAdapter adapter{loop, clock, model, driver, &store};

	/// Runs the loop, jumping the clock from deadline to deadline, until `until_ms`.
	void run_until(uint64_t until_ms) {
		for (;;) {
			loop.run_once(clock.now_ms);
			uint64_t deadline = loop.next_deadline();
			if (deadline > until_ms) {
				clock.now_ms = until_ms;
				loop.run_once(clock.now_ms);
				return;
			}
			if (deadline > clock.now_ms) {
				clock.now_ms = deadline;
			}
		}
	}

	void run_for(uint64_t ms) { run_until(clock.now_ms + ms); }

	void start_network() {
		REQUIRE(adapter.start());
		adapter.network_state(true, 0x1a62, 15, 0xdddddddddddddddd);
	}

	static hc::zigbee::NodeAddress node(hc::zigbee::Ieee ieee, uint16_t short_addr) {
		hc::zigbee::NodeAddress address;
		address.ieee = ieee;
		address.short_addr = short_addr;
		return address;
	}

	/// Only the short address (most ZCL frames carry nothing else).
	static hc::zigbee::NodeAddress by_short(uint16_t short_addr) {
		hc::zigbee::NodeAddress address;
		address.short_addr = short_addr;
		return address;
	}

	void descriptor(uint16_t short_addr, uint8_t endpoint, uint16_t profile, uint16_t device,
		std::vector<uint16_t> in, std::vector<uint16_t> out) {
		hc::zigbee::SimpleDescriptor d;
		d.endpoint = endpoint;
		d.profile = profile;
		d.device = device;
		d.in = in.data();
		d.in_count = in.size();
		d.out = out.data();
		d.out_count = out.size();
		adapter.simple_descriptor(by_short(short_addr), 0, d);
	}

	void basic(uint16_t short_addr, uint8_t endpoint, const std::string& manufacturer, const std::string& model_id) {
		std::vector<uint8_t> m = zcl_string(manufacturer);
		std::vector<uint8_t> n = zcl_string(model_id);
		hc::zigbee::AttributeRecord records[2];
		records[0].attribute = hc::zigbee::attribute::kManufacturerName;
		records[0].type = hc::zigbee::zcl_type::kCharString;
		records[0].data = m.data();
		records[0].length = m.size();
		records[1].attribute = hc::zigbee::attribute::kModelIdentifier;
		records[1].type = hc::zigbee::zcl_type::kCharString;
		records[1].data = n.data();
		records[1].length = n.size();
		adapter.attributes(by_short(short_addr), endpoint, hc::zigbee::cluster::kBasic, records, 2);
	}

	/// One attribute report with a little-endian value of the type's size.
	void report(uint16_t short_addr, uint8_t endpoint, uint16_t cluster, uint16_t attribute, uint8_t type,
		int64_t value) {
		uint8_t data[4];
		size_t size = hc::zigbee::zcl_type_size(type);
		for (size_t i = 0; i < size; ++i) {
			data[i] = static_cast<uint8_t>(static_cast<uint64_t>(value) >> (8 * i));
		}
		hc::zigbee::AttributeRecord record;
		record.attribute = attribute;
		record.type = type;
		record.data = data;
		record.length = size;
		adapter.attributes(by_short(short_addr), endpoint, cluster, &record, 1);
	}

	/// The full join of a colour-temperature bulb (endpoint 11, plus a Green Power endpoint 242).
	void join_bulb() {
		adapter.device_announced(node(kBulb, kBulbShort));
		uint8_t endpoints[] = {11, 242};
		adapter.active_endpoints(by_short(kBulbShort), 0, endpoints, 2);
		descriptor(kBulbShort, 11, hc::zigbee::kHomeAutomationProfile, 0x010C, {0x0000, 0x0003, 0x0004, 0x0005, 0x0006,
			0x0008, 0x0300, 0x1000}, {0x0019});
		descriptor(kBulbShort, 242, 0xA1E0, 0x0061, {}, {0x0021});
		basic(kBulbShort, 11, "IKEA of Sweden", "TRADFRI bulb E27 WS 806lm");
	}

	/// A temperature/humidity sensor on endpoint 1 with battery.
	void join_sensor() {
		adapter.device_announced(node(kSensor, kSensorShort));
		uint8_t endpoints[] = {1};
		adapter.active_endpoints(by_short(kSensorShort), 0, endpoints, 1);
		descriptor(kSensorShort, 1, hc::zigbee::kHomeAutomationProfile, 0x0302, {0x0000, 0x0001, 0x0003, 0x0402,
			0x0405}, {0x0019});
		basic(kSensorShort, 1, "LUMI", "lumi.weather");
	}

	/// A door contact (IAS zone type 0x0015) on endpoint 1.
	void join_contact() {
		adapter.device_announced(node(kContact, kContactShort));
		uint8_t endpoints[] = {1};
		adapter.active_endpoints(by_short(kContactShort), 0, endpoints, 1);
		descriptor(kContactShort, 1, hc::zigbee::kHomeAutomationProfile, 0x0402, {0x0000, 0x0001, 0x0500}, {});
		basic(kContactShort, 1, "SONOFF", "DS01");
		report(kContactShort, 1, hc::zigbee::cluster::kIasZone, hc::zigbee::attribute::kZoneType,
			hc::zigbee::zcl_type::kEnum16, 0x0015);
	}

	/// A remote with On/Off and Level Control client clusters on endpoint 1.
	void join_button() {
		adapter.device_announced(node(kButton, kButtonShort));
		uint8_t endpoints[] = {1};
		adapter.active_endpoints(by_short(kButtonShort), 0, endpoints, 1);
		descriptor(kButtonShort, 1, hc::zigbee::kHomeAutomationProfile, 0x0820, {0x0000, 0x0003}, {0x0003, 0x0006,
			0x0008});
		basic(kButtonShort, 1, "IKEA of Sweden", "TRADFRI on/off switch");
	}

	const hc::Datapoint* dp(const std::string& name) const { return model.find(name.c_str()); }
};

} // namespace hc_test

#endif // HC_ZIGBEE_TEST_SUPPORT_H
