/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The Zigbee device database, the IEEE address text form and the ZCL value decoder.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "allocation_counter.h"
#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/json.h"
#include "hc/zigbee/device_database.h"
#include "hc/zigbee/zcl.h"

using hc::JsonWriter;
using hc::zigbee::DeviceDatabase;
using hc::zigbee::Ieee;
using hc::zigbee::ZclValue;
using hc::zigbee::ZigbeeDevice;
using hc::zigbee::ZigbeeEndpoint;
namespace zcl_type = hc::zigbee::zcl_type;

namespace {

std::string write(const DeviceDatabase& database) {
	std::vector<char> buffer(hc::BlobStore::kMaxBlobSize);
	JsonWriter json(buffer.data(), buffer.size());
	database.write_json(json);
	REQUIRE_FALSE(json.overflowed());
	return std::string(json.data(), json.length());
}

/// A bulb and a contact sensor with every field set.
void fill(DeviceDatabase& database) {
	ZigbeeDevice* bulb = database.add(0x00124b0001020304ull, 0x1a2b);
	REQUIRE(bulb != nullptr);
	std::strcpy(bulb->manufacturer, "IKEA of Sweden");
	std::strcpy(bulb->model, "TRADFRI bulb E27 WS 806lm");
	std::strcpy(bulb->label, "Küche \"Decke\"");
	bulb->interviewed = true;
	bulb->last_seen_s = 1760000000;
	bulb->endpoint_count = 1;
	uint16_t in[] = {0x0000, 0x0003, 0x0006, 0x0008, 0x0300};
	uint16_t out[] = {0x0019};
	bulb->endpoints[0].id = 11;
	bulb->endpoints[0].profile = 0x0104;
	bulb->endpoints[0].device = 0x010C;
	bulb->endpoints[0].set_clusters(in, 5, out, 1);
	ZigbeeDevice* contact = database.add(0x00158d00090a0b0cull, 0x5e6f);
	REQUIRE(contact != nullptr);
	contact->interviewed = true;
	contact->endpoint_count = 2;
	uint16_t zone[] = {0x0000, 0x0001, 0x0500};
	contact->endpoints[0].id = 1;
	contact->endpoints[0].profile = 0x0104;
	contact->endpoints[0].device = 0x0402;
	contact->endpoints[0].set_clusters(zone, 3, nullptr, 0);
	contact->endpoints[0].zone_type = 0x0015;
	contact->endpoints[1].id = 2;
	contact->endpoints[1].profile = 0xC05E;
}

} // namespace

TEST_CASE("ZigbeeIeee: text form round trip and refusals") {
	char text[hc::zigbee::kIeeeText];
	hc::zigbee::format_ieee(0x00124b001f2a3b4cull, text);
	CHECK(std::string(text) == "00124b001f2a3b4c");
	Ieee ieee = 0;
	CHECK(hc::zigbee::parse_ieee("00124b001f2a3b4c", ieee));
	CHECK(ieee == 0x00124b001f2a3b4cull);
	CHECK(hc::zigbee::parse_ieee("00:12:4B:00:1F:2A:3B:4C", ieee));
	CHECK(ieee == 0x00124b001f2a3b4cull);
	CHECK_FALSE(hc::zigbee::parse_ieee("00124b001f2a3b4", ieee));
	CHECK_FALSE(hc::zigbee::parse_ieee("00124b001f2a3b4cd", ieee));
	CHECK_FALSE(hc::zigbee::parse_ieee("00124b001f2a3b4g", ieee));
	CHECK_FALSE(hc::zigbee::parse_ieee("00-12-4b-00-1f-2a-3b-4c", ieee));
	CHECK_FALSE(hc::zigbee::parse_ieee("0000000000000000", ieee));
	CHECK_FALSE(hc::zigbee::parse_ieee("ffffffffffffffff", ieee));
	CHECK_FALSE(hc::zigbee::parse_ieee(nullptr, ieee));
}

TEST_CASE("ZigbeeZcl: decodes the integer types little endian, signed with sign extension") {
	ZclValue v;
	uint8_t s16[] = {0x0c, 0xfe}; // -500
	REQUIRE(hc::zigbee::decode_zcl(zcl_type::kInt16, s16, 2, v));
	CHECK(v.number == -500);
	uint8_t u16[] = {0x0c, 0xfe};
	REQUIRE(hc::zigbee::decode_zcl(zcl_type::kUint16, u16, 2, v));
	CHECK(v.number == 0xfe0c);
	uint8_t u24[] = {1, 2, 3};
	REQUIRE(hc::zigbee::decode_zcl(zcl_type::kUint24, u24, 3, v));
	CHECK(v.number == 0x030201);
	uint8_t s8[] = {0x80};
	REQUIRE(hc::zigbee::decode_zcl(zcl_type::kInt8, s8, 1, v));
	CHECK(v.number == -128);
	uint8_t b[] = {0x01};
	REQUIRE(hc::zigbee::decode_zcl(zcl_type::kBool, b, 1, v));
	CHECK(v.number == 1);
	uint8_t s32[] = {0xff, 0xff, 0xff, 0xff};
	REQUIRE(hc::zigbee::decode_zcl(zcl_type::kInt32, s32, 4, v));
	CHECK(v.number == -1);
}

TEST_CASE("ZigbeeZcl: character strings and refusals") {
	ZclValue v;
	uint8_t text[] = {4, 'L', 'U', 'M', 'I'};
	REQUIRE(hc::zigbee::decode_zcl(zcl_type::kCharString, text, 5, v));
	CHECK(v.is_string());
	CHECK(std::string(v.text) == "LUMI");
	uint8_t invalid[] = {0xff};
	REQUIRE(hc::zigbee::decode_zcl(zcl_type::kCharString, invalid, 1, v));
	CHECK(std::string(v.text).empty());
	const char* error = nullptr;
	uint8_t short_string[] = {5, 'a'};
	CHECK_FALSE(hc::zigbee::decode_zcl(zcl_type::kCharString, short_string, 2, v, &error));
	CHECK(std::string(error) == "character string shorter than its length octet");
	std::vector<uint8_t> long_string(34, 'x');
	long_string[0] = 33;
	CHECK_FALSE(hc::zigbee::decode_zcl(zcl_type::kCharString, long_string.data(), long_string.size(), v, &error));
	CHECK(std::string(error) == "character string longer than 32 characters");
	uint8_t two[] = {1, 2};
	CHECK_FALSE(hc::zigbee::decode_zcl(zcl_type::kUint8, two, 2, v, &error));
	CHECK(std::string(error) == "length does not match the ZCL data type");
	CHECK_FALSE(hc::zigbee::decode_zcl(0x39, two, 2, v, &error)); // single precision: not supported
	CHECK(std::string(error) == "unsupported ZCL data type");
}

TEST_CASE("ZigbeeDatabase: JSON round trip through JsonWriter and json_parse") {
	DeviceDatabase database(nullptr);
	fill(database);
	std::string text = write(database);
	CHECK(text.find("\"format\":1") != std::string::npos);
	CHECK(text.find("\"ieee\":\"00124b0001020304\"") != std::string::npos);
	CHECK(text.find("\"zone_type\":21") != std::string::npos);

	DeviceDatabase copy(nullptr);
	const char* error = nullptr;
	REQUIRE_MESSAGE(copy.read_json(text.data(), text.size(), &error), error);
	REQUIRE(copy.size() == 2);
	const ZigbeeDevice* bulb = copy.find(0x00124b0001020304ull);
	REQUIRE(bulb != nullptr);
	CHECK(bulb->short_addr == 0x1a2b);
	CHECK(std::string(bulb->manufacturer) == "IKEA of Sweden");
	CHECK(std::string(bulb->model) == "TRADFRI bulb E27 WS 806lm");
	CHECK(std::string(bulb->label) == "Küche \"Decke\"");
	CHECK(bulb->interviewed);
	CHECK(bulb->last_seen_s == 1760000000);
	REQUIRE(bulb->endpoint_count == 1);
	CHECK(bulb->endpoints[0].id == 11);
	CHECK(bulb->endpoints[0].device == 0x010C);
	CHECK(bulb->endpoints[0].in_count == 5);
	CHECK(bulb->endpoints[0].has_in(0x0300));
	CHECK(bulb->endpoints[0].has_out(0x0019));
	CHECK(bulb->endpoints[0].zone_type == ZigbeeEndpoint::kNoZoneType);
	const ZigbeeDevice* contact = copy.find(0x00158d00090a0b0cull);
	REQUIRE(contact != nullptr);
	REQUIRE(contact->endpoint_count == 2);
	CHECK(contact->endpoints[0].zone_type == 0x0015);
	CHECK(contact->endpoints[1].profile == 0xC05E);
	CHECK(write(copy) == text);
}

TEST_CASE("ZigbeeDatabase: save and load through the blob store") {
	hc_test::FakeBlobStore store;
	DeviceDatabase database(&store);
	CHECK(database.load()); // no blob yet: empty
	CHECK(database.size() == 0);
	fill(database);
	REQUIRE(database.save());
	CHECK(store.writes_of["zigbee-devices.json"] == 1);
	DeviceDatabase copy(&store);
	REQUIRE(copy.load());
	CHECK(copy.size() == 2);
	CHECK(write(copy) == write(database));

	store.fail_writes = true;
	const char* error = nullptr;
	CHECK_FALSE(database.save(&error));
	CHECK(std::string(error) == "storage error");
}

/// A database blob written by the code before the save measured its text (commit b173023): four
/// devices covering an absent short address, an empty interview, escapes in a label, the full
/// cluster lists and four endpoints. The persisted format is frozen (spec/zigbee.md): this must load
/// unchanged, and a save of what it loaded must write it back byte for byte.
const char* const kOldBlob =
	R"json({"format":1,"devices":[{"ieee":"00124b0001020304","short":6699,"manufacturer":"IKEA of Sweden","model":"TRADFRI bulb E27 WS 806lm","label":"Küche \"Decke\"","interviewed":true,"last_seen":1760000000,"endpoints":[{"id":11,"profile":260,"device":268,"in":[0,3,6,8,768],"out":[25]}])json"
	R"json(},{"ieee":"00158d00090a0b0c","short":24175,"manufacturer":"","model":"","label":"","interviewed":true,"endpoints":[{"id":1,"profile":260,"device":1026,"in":[0,1,1280],"out":[],"zone_type":21},{"id":2,"profile":49246,"device":0,"in":[],"out":[]}])json"
	R"json(},{"ieee":"a4c1380000000001","manufacturer":"","model":"","label":"","interviewed":false,"endpoints":[])json"
	R"json(},{"ieee":"0017880100aabbcc","short":1,"manufacturer":"Signify Netherlands B.V.","model":"RWL022","label":"Flur\t/ Tür","interviewed":true,"last_seen":1,"endpoints":[{"id":1,"profile":260,"device":2080,"in":[0,1,3,6,8,768,1026,1029,1030,1024,1280,18],"out":[1026,1029,1030,1024,1280,18,64512,65535]},{"id":2,"profile":260,"device":2081,"in":[0],"out":[]},{"id":3,"profile":260,"device":2082,"in":[0,1],"out":[]},{"id":240,"profile":260,"device":2083,"in":[0,1,3],"out":[]}]}]})json";

TEST_CASE("ZigbeeDatabase: a blob of the previous code loads and is written back byte for byte") {
	hc_test::FakeBlobStore store;
	store.blobs["zigbee-devices.json"].assign(kOldBlob, kOldBlob + std::strlen(kOldBlob));
	DeviceDatabase database(&store);
	const char* error = nullptr;
	REQUIRE_MESSAGE(database.load(&error), error);
	REQUIRE(database.size() == 4);
	const ZigbeeDevice* fresh = database.find(0xa4c1380000000001ull);
	REQUIRE(fresh != nullptr);
	CHECK(fresh->short_addr == hc::zigbee::kNoShortAddress);
	CHECK_FALSE(fresh->interviewed);
	const ZigbeeDevice* remote = database.find(0x0017880100aabbccull);
	REQUIRE(remote != nullptr);
	CHECK(std::string(remote->label) == "Flur\t/ T\xc3\xbcr");
	REQUIRE(remote->endpoint_count == 4);
	CHECK(remote->endpoints[0].in_count == 12);
	CHECK(remote->endpoints[0].out_count == 8);
	CHECK(remote->endpoints[3].id == 240);

	store.blobs.clear();
	REQUIRE(database.save());
	const std::vector<uint8_t>& blob = store.blobs["zigbee-devices.json"];
	CHECK(std::string(blob.begin(), blob.end()) == std::string(kOldBlob));
}

TEST_CASE("ZigbeeDatabase: a save of 30 devices needs no block larger than its JSON") {
	hc_test::FakeBlobStore store;
	DeviceDatabase database(&store);
	uint16_t clusters[] = {0x0000, 0x0001, 0x0003, 0x0004, 0x0005, 0x0006, 0x0008, 0x0300, 0x0402, 0x0405, 0x0b05, 0x1000};
	for (int i = 0; i < 30; ++i) {
		ZigbeeDevice* device = database.add(0x00124b0000000100ull + static_cast<Ieee>(i), static_cast<uint16_t>(0x100 + i));
		REQUIRE(device != nullptr);
		std::strcpy(device->manufacturer, "A manufacturer of 32 characters!");
		std::strcpy(device->model, "A model name with 32 characters!");
		std::snprintf(device->label, sizeof(device->label), "Device number %d of thirty", i);
		device->interviewed = true;
		device->last_seen_s = 1760000000 + i;
		device->endpoint_count = 2;
		for (size_t e = 0; e < 2; ++e) {
			device->endpoints[e].id = static_cast<uint8_t>(e + 1);
			device->endpoints[e].profile = 0x0104;
			device->endpoints[e].device = 0x0100;
			device->endpoints[e].set_clusters(clusters, 12, clusters, 12);
		}
	}
	size_t json = write(database).size();
	// Between 12 and 20 KB: the old save needed a 20 KB block for it besides the copies.
	REQUIRE(json > 12000);
	REQUIRE(json < hc::BlobStore::kMaxBlobSize);
	size_t largest = 0;
	{
		hc_test::AllocationScope counting;
		REQUIRE(database.save());
		largest = counting.largest();
	}
	// The fake store copies the content into its map (a vector of exactly the blob's size).
	CHECK(largest <= json + 1);
	CHECK(store.blobs["zigbee-devices.json"].size() == json);

	DeviceDatabase copy(&store);
	{
		hc_test::AllocationScope counting;
		REQUIRE(copy.load());
		largest = counting.largest();
		// One buffer of the blob (and the fake store's std::string of the name), no temporary
		// table of 32 devices (12 KB before).
		CHECK(counting.peak() <= json + 1 + 64);
	}
	CHECK(largest <= json + 1);
	CHECK(copy.size() == 30);
	CHECK((write(copy) == write(database)));
}

TEST_CASE("ZigbeeDatabase: a refusal in the second half of the devices keeps the loaded content") {
	DeviceDatabase database(nullptr);
	fill(database);
	std::string before = write(database);
	std::string text = before;
	// A third device, valid, and a fourth repeating the first's address.
	std::string extra = R"(,{"ieee":"a4c1380000000001"},{"ieee":"00124b0001020304"}]})";
	text.replace(text.size() - 2, 2, extra);
	const char* error = nullptr;
	CHECK_FALSE(database.read_json(text.data(), text.size(), &error));
	CHECK(std::string(error) == "a device is listed twice");
	CHECK(write(database) == before);
}

TEST_CASE("ZigbeeDatabase: a refused document changes nothing") {
	DeviceDatabase database(nullptr);
	fill(database);
	std::string before = write(database);
	const char* cases[][2] = {
		{"[]", "the database is not a JSON object"},
		{"{\"format\":2,\"devices\":[]}", "the database format is not 1"},
		{"{\"format\":1}", "the database has no devices array"},
		{"{\"format\":1,\"devices\":[{\"ieee\":\"xyz\"}]}", "device ieee missing or not 16 hex digits"},
		{"{\"format\":1,\"devices\":[{\"ieee\":\"00124b0001020304\"},{\"ieee\":\"00124b0001020304\"}]}",
			"a device is listed twice"},
		{"{\"format\":1,\"devices\":[{\"ieee\":\"00124b0001020304\",\"short\":70000}]}",
			"device short address is not a number 0..65535"},
		{"{\"format\":1,\"devices\":[{\"ieee\":\"00124b0001020304\",\"endpoints\":[{\"id\":0}]}]}",
			"endpoint id missing or outside 1..240"},
		{"{\"format\":1,\"devices\":[{\"ieee\":\"00124b0001020304\",\"endpoints\":[{\"id\":1},{\"id\":1}]}]}",
			"device lists an endpoint twice"},
		{"{\"format\":1,\"devices\":[{\"ieee\":\"00124b0001020304\",\"endpoints\":[{\"id\":1},{\"id\":2},{\"id\":3},"
			"{\"id\":4},{\"id\":5}]}]}", "device has more than 4 endpoints"},
		{"{\"format\":1,\"devices\":[{\"ieee\":\"00124b0001020304\",\"endpoints\":[{\"id\":1,\"in\":[0,1,2,3,4,5,6,7,8,"
			"9,10,11,12]}]}]}", "endpoint cluster list invalid (numbers 0..65535, at most 12)"},
		{"{\"format\":1,\"devices\":[{\"ieee\":\"00124b0001020304\",\"label\":\"0123456789012345678901234567890123\"}]}",
			"device manufacturer, model or label is not a string of at most 32 bytes"},
		{"{\"format\":1,\"devices\":[{\"ieee\":\"00124b0001020304\",\"interviewed\":1}]}",
			"device interviewed is not a boolean"},
	};
	for (const auto& c : cases) {
		CAPTURE(c[0]);
		const char* error = nullptr;
		CHECK_FALSE(database.read_json(c[0], std::strlen(c[0]), &error));
		REQUIRE(error != nullptr);
		CHECK(std::string(error) == c[1]);
		CHECK(write(database) == before);
	}
	// Unknown members are ignored (a newer writer may add some).
	const char* newer = "{\"format\":1,\"extra\":true,\"devices\":[{\"ieee\":\"00124b0001020304\",\"colour\":\"red\"}]}";
	CHECK(database.read_json(newer, std::strlen(newer)));
	CHECK(database.size() == 1);
}

TEST_CASE("ZigbeeDatabase: add, short address moves, remove, capacity") {
	DeviceDatabase database(nullptr);
	ZigbeeDevice* a = database.add(0x1111, 0x0001);
	ZigbeeDevice* b = database.add(0x2222, 0x0002);
	REQUIRE(a != nullptr);
	REQUIRE(b != nullptr);
	CHECK(database.find_short(0x0002) == b);
	// b rejoined with a's old address: a's entry loses it.
	database.add(0x2222, 0x0001);
	CHECK(database.find(0x1111)->short_addr == hc::zigbee::kNoShortAddress);
	CHECK(database.find_short(0x0001)->ieee == 0x2222);
	CHECK(database.find_short(hc::zigbee::kNoShortAddress) == nullptr);
	CHECK(database.remove(0x1111));
	CHECK_FALSE(database.remove(0x1111));
	CHECK(database.size() == 1);
	const char* error = nullptr;
	CHECK(database.add(0, 1, &error) == nullptr);
	CHECK(std::string(error) == "reserved IEEE address");
	for (Ieee i = 0; database.size() < DeviceDatabase::kMaxDevices; ++i) {
		REQUIRE(database.add(0x9000 + i, hc::zigbee::kNoShortAddress) != nullptr);
	}
	CHECK(database.add(0x8888, 0x0008, &error) == nullptr);
	CHECK(std::string(error) == "the device database is full (32 devices)");
}

TEST_CASE("ZigbeeDatabase: an endpoint with too many clusters keeps the known ones first") {
	ZigbeeEndpoint endpoint;
	std::vector<uint16_t> in;
	for (uint16_t c = 0xFC00; c < 0xFC0E; ++c) {
		in.push_back(c); // 14 manufacturer-specific clusters first
	}
	in.push_back(0x0402);
	in.push_back(0x0006);
	in.push_back(0x0006); // a duplicate counts once
	size_t dropped = endpoint.set_clusters(in.data(), in.size(), nullptr, 0);
	CHECK(endpoint.in_count == ZigbeeEndpoint::kMaxClusters);
	CHECK(dropped == 4);
	CHECK(endpoint.in[0] == 0x0402);
	CHECK(endpoint.in[1] == 0x0006);
	CHECK(endpoint.has_in(0xFC00));
	CHECK_FALSE(endpoint.has_in(0xFC0D));
}

TEST_CASE("ZigbeeDatabase: copy_text cuts at a character boundary") {
	char field[6];
	hc::zigbee::copy_text(field, sizeof(field), "abcd\xc3\xbc"); // "abcdü": ü would need bytes 5 and 6
	CHECK(std::string(field) == "abcd");
	hc::zigbee::copy_text(field, sizeof(field), "ab");
	CHECK(std::string(field) == "ab");
	hc::zigbee::copy_text(field, sizeof(field), nullptr);
	CHECK(std::string(field).empty());
}
