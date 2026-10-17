/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/knx/knx_settings.h"

#include <cstdio>
#include <cstring>

#include "hc/log.h"
#include "hc/network_settings.h"

namespace hc {
namespace knx {

namespace {

constexpr const char* kTag = "hc.knx";

std::optional<GatewayAddress> refuse(const char** error, const char* reason) {
	if (error != nullptr) {
		*error = reason;
	}
	return std::nullopt;
}

} // namespace

std::optional<GatewayAddress> GatewayAddress::parse(const char* text, const char** error) {
	if (text == nullptr || text[0] == '\0') {
		return refuse(error, "gateway is empty");
	}
	size_t length = std::strlen(text);
	if (length > Settings::kMaxStringLength) {
		return refuse(error, "gateway is longer than 127 characters");
	}
	const char* colon = std::strchr(text, ':');
	if (colon != nullptr && std::strchr(colon + 1, ':') != nullptr) {
		return refuse(error, "gateway has more than one ':' (IPv6 is not supported)");
	}
	GatewayAddress result;
	size_t host_length = colon != nullptr ? static_cast<size_t>(colon - text) : length;
	std::memcpy(result.host, text, host_length);
	result.host[host_length] = '\0';
	const char* host_error = nullptr;
	if (!host_name_valid(result.host, &host_error)) {
		return refuse(error, host_error);
	}
	if (colon != nullptr) {
		const char* p = colon + 1;
		uint32_t port = 0;
		size_t digits = 0;
		while (*p >= '0' && *p <= '9') {
			if (++digits > 5) {
				return refuse(error, "gateway port must be 1..65535");
			}
			port = port * 10 + static_cast<uint32_t>(*p - '0');
			++p;
		}
		if (digits == 0 || *p != '\0') {
			return refuse(error, "gateway port is not a number");
		}
		if (port == 0 || port > 65535) {
			return refuse(error, "gateway port must be 1..65535");
		}
		result.port = static_cast<uint16_t>(port);
	}
	return result;
}

const char* GatewayAddress::format(char* buffer, size_t size) const {
	std::snprintf(buffer, size, "%s:%u", host, static_cast<unsigned>(port));
	return buffer;
}

void read_gateway_settings(const Settings& settings, GatewayAddress& gateway, bool& nat) {
	gateway = GatewayAddress{};
	nat = false;
	char text[Settings::kMaxStringLength + 1];
	SettingsStatus status = settings.get_string(setting::kGateway, text, sizeof(text));
	if (status == SettingsStatus::ok) {
		const char* error = nullptr;
		std::optional<GatewayAddress> parsed = GatewayAddress::parse(text, &error);
		if (parsed) {
			gateway = *parsed;
		} else {
			Log::error(kTag, "setting %s is invalid (%s): no KNX gateway", setting::kGateway, error);
		}
	} else if (status != SettingsStatus::not_found) {
		Log::error(kTag, "setting %s unreadable (%s): no KNX gateway", setting::kGateway, settings_status_name(status));
	}
	bool value = false;
	status = settings.get_bool(setting::kNat, value);
	if (status == SettingsStatus::ok) {
		nat = value;
	} else if (status != SettingsStatus::not_found) {
		Log::error(kTag, "setting %s unreadable (%s): NAT mode off", setting::kNat, settings_status_name(status));
	}
}

bool store_gateway_setting(Settings& settings, const char* text, const char** error) {
	if (!GatewayAddress::parse(text, error)) {
		return false;
	}
	SettingsStatus status = settings.set_string(setting::kGateway, text);
	if (status != SettingsStatus::ok) {
		if (error != nullptr) {
			*error = settings_status_name(status);
		}
		return false;
	}
	return true;
}

} // namespace knx
} // namespace hc
