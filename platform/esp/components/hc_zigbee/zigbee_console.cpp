/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hc/zigbee/adapter.h"
#include "hc/zigbee/zcl.h"
#include "zigbee_service.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kUsage = "zigbee status | devices | permit <seconds> | remove <ieee> | identify <ieee> | "
							   "label <ieee> [<text>] | interview <ieee>";

int usage() {
	std::printf("usage: %s\n", kUsage);
	return 1;
}

int refused(const char* error) {
	std::printf("error: %s\n", error != nullptr ? error : "refused");
	return 1;
}

int status(ZigbeeService& service, zigbee::ZigbeeAdapter& adapter) {
	const zigbee::NetworkStatus& network = adapter.network();
	if (!network.up) {
		std::printf("network   forming or restoring\n");
	} else {
		char extended[zigbee::kIeeeText];
		zigbee::format_ieee(network.extended_pan, extended);
		std::printf("network   PAN 0x%04x, channel %u, extended PAN %s\n", static_cast<unsigned>(network.pan_id),
			static_cast<unsigned>(network.channel), extended);
	}
	uint64_t now = service.controller().clock().monotonic_ms();
	if (network.permit_until_ms > now) {
		std::printf("joining   open for %u s\n", static_cast<unsigned>((network.permit_until_ms - now + 999) / 1000));
	} else {
		std::printf("joining   closed (zigbee permit <seconds>)\n");
	}
	std::printf("devices   %u of %u, %u datapoints%s\n", static_cast<unsigned>(adapter.devices().size()),
		static_cast<unsigned>(zigbee::DeviceDatabase::kMaxDevices), static_cast<unsigned>(adapter.point_count()),
		adapter.loaded() ? "" : " (not loaded: the model holds datapoints of another owner)");
	if (adapter.interviewing() != 0) {
		char ieee[zigbee::kIeeeText];
		zigbee::format_ieee(adapter.interviewing(), ieee);
		std::printf("interview %s\n", ieee);
	}
	std::printf("events    %u dropped (ring to the event loop full)\n", static_cast<unsigned>(service.dropped()));
	return 0;
}

int devices(ZigbeeService& service, zigbee::ZigbeeAdapter& adapter) {
	const zigbee::DeviceDatabase& database = adapter.devices();
	if (database.size() == 0) {
		std::printf("no devices: zigbee permit 120, then reset the device into pairing mode\n");
		return 0;
	}
	for (size_t i = 0; i < database.size(); ++i) {
		const zigbee::ZigbeeDevice& device = database.at(i);
		char ieee[zigbee::kIeeeText];
		zigbee::format_ieee(device.ieee, ieee);
		std::printf("%s  short 0x%04x  %s / %s%s%s%s%s\n", ieee, static_cast<unsigned>(device.short_addr),
			device.manufacturer[0] != 0 ? device.manufacturer : "?", device.model[0] != 0 ? device.model : "?",
			device.label[0] != 0 ? "  \"" : "", device.label, device.label[0] != 0 ? "\"" : "",
			device.left            ? "  (left the network)"
				: !device.interviewed ? "  (not interviewed)"
				: device.stale        ? "  (stale)"
									  : "");
		for (size_t p = 0; adapter.loaded() && p < adapter.point_count(); ++p) {
			if (adapter.point(p).ieee != device.ieee) {
				continue;
			}
			const Datapoint* dp = service.model().get(static_cast<uint16_t>(p));
			if (dp == nullptr) {
				continue;
			}
			char value[32];
			std::printf("    %-40s %s (%s)\n", dp->name(), dp->value().format(value, sizeof(value)),
				quality_name(dp->quality()));
		}
	}
	return 0;
}

bool device_argument(const char* text, zigbee::Ieee& ieee) {
	if (!zigbee::parse_ieee(text, ieee)) {
		std::printf("error: '%s' is no IEEE address (16 hex digits, colons allowed)\n", text);
		return false;
	}
	return true;
}

} // namespace

int zigbee_console_command(ZigbeeService& service, int argc, char** argv) {
	if (argc < 2) {
		return usage();
	}
	const char* command = argv[1];
	zigbee::ZigbeeAdapter* adapter = service.adapter();
	if (adapter == nullptr) {
		return refused(service.unavailable_reason());
	}
	if (std::strcmp(command, "status") == 0 && argc == 2) {
		return status(service, *adapter);
	}
	if (std::strcmp(command, "devices") == 0 && argc == 2) {
		return devices(service, *adapter);
	}
	const char* error = nullptr;
	if (std::strcmp(command, "permit") == 0 && argc == 3) {
		char* end = nullptr;
		unsigned long seconds = std::strtoul(argv[2], &end, 10);
		if (end == argv[2] || *end != 0 || seconds > 0xFFFFFFFFUL) {
			return refused("permit takes the seconds as a number (0 closes)");
		}
		if (!service.permit_join(static_cast<uint32_t>(seconds), &error)) {
			return refused(error);
		}
		std::printf(seconds == 0 ? "closing the network\n" : "opening the network for %lu s\n", seconds);
		return 0;
	}
	if (argc < 3) {
		return usage();
	}
	zigbee::Ieee ieee = 0;
	if (std::strcmp(command, "remove") == 0 && argc == 3) {
		if (!device_argument(argv[2], ieee)) {
			return 1;
		}
		return adapter->remove(ieee, &error) ? 0 : refused(error);
	}
	if (std::strcmp(command, "identify") == 0 && argc == 3) {
		if (!device_argument(argv[2], ieee)) {
			return 1;
		}
		return adapter->identify(ieee, &error) ? 0 : refused(error);
	}
	if (std::strcmp(command, "interview") == 0 && argc == 3) {
		if (!device_argument(argv[2], ieee)) {
			return 1;
		}
		return adapter->interview(ieee, &error) ? 0 : refused(error);
	}
	if (std::strcmp(command, "label") == 0) {
		if (!device_argument(argv[2], ieee)) {
			return 1;
		}
		// The words after the address, joined by single spaces (the REPL splits at spaces).
		char label[zigbee::ZigbeeDevice::kMaxText + 2] = {};
		size_t used = 0;
		for (int i = 3; i < argc; ++i) {
			size_t length = std::strlen(argv[i]);
			size_t needed = length + (i > 3 ? 1 : 0);
			if (used + needed > zigbee::ZigbeeDevice::kMaxText) {
				return refused("a label has at most 32 bytes");
			}
			if (i > 3) {
				label[used++] = ' ';
			}
			std::memcpy(label + used, argv[i], length);
			used += length;
		}
		label[used] = 0;
		return adapter->set_label(ieee, label, &error) ? 0 : refused(error);
	}
	return usage();
}

} // namespace esp
} // namespace hc
