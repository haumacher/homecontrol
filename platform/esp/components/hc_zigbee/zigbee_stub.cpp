/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The ZigbeeService on a chip without an 802.15.4 radio (the ESP32-S3) or with HC_ZIGBEE_ENABLED
// off: the same interface, every operation refused with the reason.
#include <cstdio>

#include "sdkconfig.h"
#include "zigbee_service.h"

namespace hc {
namespace esp {

namespace {

#if CONFIG_SOC_IEEE802154_SUPPORTED
constexpr const char* kReason = "Zigbee is off (menuconfig: HomeControl Zigbee, HC_ZIGBEE_ENABLED)";
#else
constexpr const char* kReason = "this chip has no 802.15.4 radio (Zigbee needs an ESP32-C6 or ESP32-H2)";
#endif

} // namespace

ZigbeeService::ZigbeeService(Controller& controller, LoopQueue& queue, DatapointModel& model)
	: controller_(controller), queue_(queue), model_(model) {}

void ZigbeeService::init(BlobStore*) {}

void ZigbeeService::init(image::ImageService&, BlobStore*) {}

bool ZigbeeService::available() const {
	return false;
}

const char* ZigbeeService::unavailable_reason() const {
	return kReason;
}

bool ZigbeeService::permit_join(uint32_t, const char** error) {
	if (error != nullptr) {
		*error = kReason;
	}
	return false;
}

zigbee::ZigbeeAdapter* ZigbeeService::adapter() {
	return nullptr;
}

void ZigbeeService::write_json(JsonWriter& json) {
	json.begin_object().member("available", false).member("reason", kReason).end_object();
}

uint32_t ZigbeeService::dropped() const {
	return 0;
}

int zigbee_console_command(ZigbeeService& service, int, char**) {
	std::printf("error: %s\n", service.unavailable_reason());
	return 1;
}

} // namespace esp
} // namespace hc
