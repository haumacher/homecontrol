/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/zigbee/device_database.h"

#include <cmath>
#include <cstring>
#include <new>

namespace hc {
namespace zigbee {

namespace {

/// The clusters an endpoint keeps before all others when its descriptor lists more than
/// ZigbeeEndpoint::kMaxClusters: those the interview and the mapping use.
constexpr uint16_t kPreferred[] = {
	cluster::kBasic,
	cluster::kPowerConfiguration,
	cluster::kIdentify,
	cluster::kOnOff,
	cluster::kLevelControl,
	cluster::kMultistateInput,
	cluster::kColorControl,
	cluster::kIlluminanceMeasurement,
	cluster::kTemperatureMeasurement,
	cluster::kRelativeHumidity,
	cluster::kOccupancySensing,
	cluster::kIasZone,
};

bool preferred(uint16_t id) {
	for (uint16_t p : kPreferred) {
		if (p == id) {
			return true;
		}
	}
	return false;
}

bool contains(const uint16_t* list, size_t count, uint16_t id) {
	for (size_t i = 0; i < count; ++i) {
		if (list[i] == id) {
			return true;
		}
	}
	return false;
}

/// Fills `out` from `in`: preferred clusters first, then the rest, skipping duplicates. Returns the
/// number of clusters dropped.
size_t select(const uint16_t* in, size_t total, uint16_t* out, uint8_t& count) {
	count = 0;
	size_t dropped = 0;
	for (int pass = 0; pass < 2; ++pass) {
		for (size_t i = 0; i < total; ++i) {
			uint16_t id = in[i];
			if (preferred(id) != (pass == 0) || contains(out, count, id)) {
				continue;
			}
			if (count < ZigbeeEndpoint::kMaxClusters) {
				out[count++] = id;
			} else {
				++dropped;
			}
		}
	}
	return dropped;
}

void fail(const char** error, const char* reason) {
	if (error != nullptr) {
		*error = reason;
	}
}

bool read_uint(const JsonSpan& object, const char* name, uint32_t max, uint32_t& out, bool required) {
	JsonSpan span;
	if (!json_member(object, name, span)) {
		return !required;
	}
	double number = 0;
	if (!json_number(span, number) || number < 0 || number > max || std::floor(number) != number) {
		return false;
	}
	out = static_cast<uint32_t>(number);
	return true;
}

bool read_text(const JsonSpan& object, const char* name, char* field, size_t size) {
	JsonSpan span;
	if (!json_member(object, name, span)) {
		field[0] = '\0';
		return true;
	}
	return json_string(span, field, size);
}

bool read_clusters(const JsonSpan& endpoint, const char* name, uint16_t* list, uint8_t& count) {
	count = 0;
	JsonSpan array;
	if (!json_member(endpoint, name, array)) {
		return true;
	}
	if (array.kind != JsonSpan::Kind::array) {
		return false;
	}
	size_t position = 0;
	JsonSpan item;
	while (json_next_item(array, position, item)) {
		double number = 0;
		if (!json_number(item, number) || number < 0 || number > 0xFFFF || std::floor(number) != number ||
			count >= ZigbeeEndpoint::kMaxClusters) {
			return false;
		}
		list[count++] = static_cast<uint16_t>(number);
	}
	return true;
}

bool read_endpoint(const JsonSpan& span, ZigbeeEndpoint& out, const char** error) {
	if (span.kind != JsonSpan::Kind::object) {
		fail(error, "an endpoint is not an object");
		return false;
	}
	uint32_t id = 0;
	uint32_t profile = 0;
	uint32_t device = 0;
	uint32_t zone = ZigbeeEndpoint::kNoZoneType;
	if (!read_uint(span, "id", 240, id, true) || id == 0) {
		fail(error, "endpoint id missing or outside 1..240");
		return false;
	}
	if (!read_uint(span, "profile", 0xFFFF, profile, false) || !read_uint(span, "device", 0xFFFF, device, false) ||
		!read_uint(span, "zone_type", 0xFFFF, zone, false)) {
		fail(error, "endpoint profile, device or zone_type is not a number 0..65535");
		return false;
	}
	out = ZigbeeEndpoint();
	out.id = static_cast<uint8_t>(id);
	out.profile = static_cast<uint16_t>(profile);
	out.device = static_cast<uint16_t>(device);
	out.zone_type = static_cast<uint16_t>(zone);
	if (!read_clusters(span, "in", out.in, out.in_count) || !read_clusters(span, "out", out.out, out.out_count)) {
		fail(error, "endpoint cluster list invalid (numbers 0..65535, at most 12)");
		return false;
	}
	return true;
}

bool read_device(const JsonSpan& span, ZigbeeDevice& out, const char** error) {
	if (span.kind != JsonSpan::Kind::object) {
		fail(error, "a device is not an object");
		return false;
	}
	out = ZigbeeDevice();
	JsonSpan member;
	char text[24];
	if (!json_member(span, "ieee", member) || !json_string(member, text, sizeof(text)) || !parse_ieee(text, out.ieee)) {
		fail(error, "device ieee missing or not 16 hex digits");
		return false;
	}
	uint32_t short_addr = kNoShortAddress;
	if (!read_uint(span, "short", 0xFFFF, short_addr, false)) {
		fail(error, "device short address is not a number 0..65535");
		return false;
	}
	out.short_addr = static_cast<uint16_t>(short_addr);
	if (!read_text(span, "manufacturer", out.manufacturer, sizeof(out.manufacturer)) ||
		!read_text(span, "model", out.model, sizeof(out.model)) || !read_text(span, "label", out.label, sizeof(out.label))) {
		fail(error, "device manufacturer, model or label is not a string of at most 32 bytes");
		return false;
	}
	if (json_member(span, "interviewed", member) && !json_bool(member, out.interviewed)) {
		fail(error, "device interviewed is not a boolean");
		return false;
	}
	if (json_member(span, "last_seen", member)) {
		double seconds = 0;
		if (!json_number(member, seconds) || seconds < 0 || seconds > 1e12 || std::floor(seconds) != seconds) {
			fail(error, "device last_seen is not a time in seconds");
			return false;
		}
		out.last_seen_s = static_cast<int64_t>(seconds);
	}
	if (json_member(span, "endpoints", member)) {
		if (member.kind != JsonSpan::Kind::array) {
			fail(error, "device endpoints is not an array");
			return false;
		}
		size_t position = 0;
		JsonSpan item;
		while (json_next_item(member, position, item)) {
			if (out.endpoint_count >= ZigbeeDevice::kMaxEndpoints) {
				fail(error, "device has more than 4 endpoints");
				return false;
			}
			ZigbeeEndpoint& endpoint = out.endpoints[out.endpoint_count];
			if (!read_endpoint(item, endpoint, error)) {
				return false;
			}
			if (out.endpoint(endpoint.id) != nullptr) {
				fail(error, "device lists an endpoint twice");
				return false;
			}
			++out.endpoint_count;
		}
	}
	return true;
}

} // namespace

bool ZigbeeEndpoint::has_in(uint16_t cluster) const {
	return contains(in, in_count, cluster);
}

bool ZigbeeEndpoint::has_out(uint16_t cluster) const {
	return contains(out, out_count, cluster);
}

size_t ZigbeeEndpoint::set_clusters(const uint16_t* in_clusters, size_t in_total, const uint16_t* out_clusters,
	size_t out_total) {
	return select(in_clusters, in_total, in, in_count) + select(out_clusters, out_total, out, out_count);
}

const ZigbeeEndpoint* ZigbeeDevice::endpoint(uint8_t id) const {
	for (size_t i = 0; i < endpoint_count; ++i) {
		if (endpoints[i].id == id) {
			return &endpoints[i];
		}
	}
	return nullptr;
}

ZigbeeEndpoint* ZigbeeDevice::endpoint(uint8_t id) {
	return const_cast<ZigbeeEndpoint*>(static_cast<const ZigbeeDevice*>(this)->endpoint(id));
}

void copy_text(char* field, size_t size, const char* text) {
	if (size == 0) {
		return;
	}
	size_t length = text != nullptr ? std::strlen(text) : 0;
	if (length >= size) {
		length = size - 1;
		// Do not cut a UTF-8 sequence: drop continuation bytes and the lead byte they belong to.
		while (length > 0 && (static_cast<uint8_t>(text[length]) & 0xC0) == 0x80) {
			--length;
		}
	}
	if (length > 0) {
		std::memcpy(field, text, length);
	}
	field[length] = '\0';
}

DeviceDatabase::DeviceDatabase(BlobStore* store) : store_(store) {}

ZigbeeDevice* DeviceDatabase::find(Ieee ieee) {
	for (size_t i = 0; i < count_; ++i) {
		if (devices_[i].ieee == ieee) {
			return &devices_[i];
		}
	}
	return nullptr;
}

const ZigbeeDevice* DeviceDatabase::find(Ieee ieee) const {
	return const_cast<DeviceDatabase*>(this)->find(ieee);
}

ZigbeeDevice* DeviceDatabase::find_short(uint16_t short_addr) {
	if (short_addr == kNoShortAddress) {
		return nullptr;
	}
	for (size_t i = 0; i < count_; ++i) {
		if (devices_[i].short_addr == short_addr) {
			return &devices_[i];
		}
	}
	return nullptr;
}

ZigbeeDevice* DeviceDatabase::add(Ieee ieee, uint16_t short_addr, const char** error) {
	if (ieee == 0 || ieee == ~static_cast<Ieee>(0)) {
		fail(error, "reserved IEEE address");
		return nullptr;
	}
	ZigbeeDevice* device = find(ieee);
	if (device == nullptr) {
		if (count_ >= kMaxDevices) {
			fail(error, "the device database is full (32 devices)");
			return nullptr;
		}
		device = &devices_[count_++];
		*device = ZigbeeDevice();
		device->ieee = ieee;
	}
	if (short_addr != kNoShortAddress) {
		// A short address belongs to one device at a time: another entry holding it is outdated.
		for (size_t i = 0; i < count_; ++i) {
			if (&devices_[i] != device && devices_[i].short_addr == short_addr) {
				devices_[i].short_addr = kNoShortAddress;
			}
		}
		device->short_addr = short_addr;
	}
	return device;
}

bool DeviceDatabase::remove(Ieee ieee) {
	for (size_t i = 0; i < count_; ++i) {
		if (devices_[i].ieee == ieee) {
			for (size_t j = i + 1; j < count_; ++j) {
				devices_[j - 1] = devices_[j];
			}
			--count_;
			return true;
		}
	}
	return false;
}

void DeviceDatabase::write_json(JsonWriter& json) const {
	json.begin_object();
	json.member("format", static_cast<int32_t>(kFormat));
	json.key("devices").begin_array();
	for (size_t i = 0; i < count_; ++i) {
		const ZigbeeDevice& device = devices_[i];
		char ieee[kIeeeText];
		format_ieee(device.ieee, ieee);
		json.begin_object();
		json.member("ieee", ieee);
		if (device.short_addr != kNoShortAddress) {
			json.member("short", static_cast<uint32_t>(device.short_addr));
		}
		json.member("manufacturer", device.manufacturer);
		json.member("model", device.model);
		json.member("label", device.label);
		json.member("interviewed", device.interviewed);
		if (device.last_seen_s > 0) {
			json.member("last_seen", static_cast<int64_t>(device.last_seen_s));
		}
		json.key("endpoints").begin_array();
		for (size_t e = 0; e < device.endpoint_count; ++e) {
			const ZigbeeEndpoint& endpoint = device.endpoints[e];
			json.begin_object();
			json.member("id", static_cast<uint32_t>(endpoint.id));
			json.member("profile", static_cast<uint32_t>(endpoint.profile));
			json.member("device", static_cast<uint32_t>(endpoint.device));
			json.key("in").begin_array();
			for (size_t c = 0; c < endpoint.in_count; ++c) {
				json.number(static_cast<uint32_t>(endpoint.in[c]));
			}
			json.end_array();
			json.key("out").begin_array();
			for (size_t c = 0; c < endpoint.out_count; ++c) {
				json.number(static_cast<uint32_t>(endpoint.out[c]));
			}
			json.end_array();
			if (endpoint.zone_type != ZigbeeEndpoint::kNoZoneType) {
				json.member("zone_type", static_cast<uint32_t>(endpoint.zone_type));
			}
			json.end_object();
		}
		json.end_array();
		json.end_object();
	}
	json.end_array();
	json.end_object();
}

bool DeviceDatabase::read_json(const char* text, size_t length, const char** error) {
	JsonSpan root;
	if (!json_parse(text, length, root, error)) {
		return false;
	}
	if (root.kind != JsonSpan::Kind::object) {
		fail(error, "the database is not a JSON object");
		return false;
	}
	JsonSpan member;
	double format = 0;
	if (!json_member(root, "format", member) || !json_number(member, format) || format != kFormat) {
		fail(error, "the database format is not 1");
		return false;
	}
	JsonSpan devices;
	if (!json_member(root, "devices", devices) || devices.kind != JsonSpan::Kind::array) {
		fail(error, "the database has no devices array");
		return false;
	}
	// Two passes over the text, so that a refusal changes nothing without a temporary table: the
	// first checks every device (one scratch entry on the stack, the addresses for the duplicate
	// check), the second parses the checked devices straight into the table.
	Ieee seen[kMaxDevices];
	size_t count = 0;
	size_t position = 0;
	JsonSpan item;
	{
		ZigbeeDevice scratch;
		while (json_next_item(devices, position, item)) {
			if (count >= kMaxDevices) {
				fail(error, "more than 32 devices");
				return false;
			}
			if (!read_device(item, scratch, error)) {
				return false;
			}
			for (size_t i = 0; i < count; ++i) {
				if (seen[i] == scratch.ieee) {
					fail(error, "a device is listed twice");
					return false;
				}
			}
			seen[count++] = scratch.ieee;
		}
	}
	position = 0;
	size_t parsed = 0;
	while (parsed < count && json_next_item(devices, position, item) && read_device(item, devices_[parsed], nullptr)) {
		++parsed;
	}
	count_ = parsed; // == count: the same text passed the first pass
	return true;
}

bool DeviceDatabase::load(const char** error) {
	if (store_ == nullptr) {
		return true;
	}
	size_t size = 0;
	BlobStatus status = store_->size(kBlobName, size);
	if (status == BlobStatus::not_found) {
		return true;
	}
	if (status != BlobStatus::ok) {
		fail(error, blob_status_name(status));
		return false;
	}
	char* buffer = new (std::nothrow) char[size + 1];
	if (buffer == nullptr) {
		fail(error, "out of memory");
		return false;
	}
	size_t length = 0;
	status = store_->read(kBlobName, 0, buffer, size, length);
	bool ok = false;
	if (status != BlobStatus::ok) {
		fail(error, blob_status_name(status));
	} else if (length != size) {
		fail(error, "the blob is shorter than its size");
	} else {
		ok = read_json(buffer, length, error);
	}
	delete[] buffer;
	return ok;
}

bool DeviceDatabase::save(const char** error) {
	if (store_ == nullptr) {
		fail(error, "no blob store");
		return false;
	}
	// Measured first, so that the buffer is exactly the JSON (with its NUL): a save of a full
	// database needs no block larger than its text.
	JsonWriter counter = JsonWriter::counter();
	write_json(counter);
	size_t length = counter.length();
	if (length > BlobStore::kMaxBlobSize) {
		fail(error, "the database does not fit a blob (20 KB)");
		return false;
	}
	char* buffer = new (std::nothrow) char[length + 1];
	if (buffer == nullptr) {
		fail(error, "out of memory");
		return false;
	}
	JsonWriter json(buffer, length + 1);
	write_json(json);
	bool ok = false;
	if (json.overflowed() || json.length() != length) {
		fail(error, "the database changed while it was written"); // cannot happen: one thread
	} else {
		BlobChunk chunk{json.data(), json.length()};
		BlobStatus status = store_->write(kBlobName, &chunk, 1);
		if (status != BlobStatus::ok) {
			fail(error, blob_status_name(status));
		} else {
			ok = true;
		}
	}
	delete[] buffer;
	return ok;
}

} // namespace zigbee
} // namespace hc
