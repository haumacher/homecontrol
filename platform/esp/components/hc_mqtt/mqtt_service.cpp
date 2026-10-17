/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "mqtt_service.h"

#include <cstdio>
#include <cstring>

#include "hc/version.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kHint = "status | publish-discovery | set password <pw> | clear password";
constexpr const char* kHelp =
	"MQTT (doc/mqtt.md): the connection to the broker the runtime image names, Home Assistant discovery, the "
	"broker password (a device setting, never in the image)";

MqttIdentity identity_of(const char* node_id) {
	MqttIdentity identity;
	identity.node_id = node_id;
	identity.sw_version = kVersion;
	return identity;
}

} // namespace

MqttService::MqttService(Controller& controller, LoopQueue& queue, DatapointModel& model, const History* history,
	const char* node_id)
	: controller_(controller),
	  client_(queue),
	  mqtt_(controller.loop(), controller.clock(), controller.settings(), controller.connectivity(), model, client_,
		  identity_of(node_id), history),
	  console_command_{"mqtt", kHint, kHelp, &MqttService::console_entry, this} {}

int MqttService::console_entry(void* context, int argc, char** argv) {
	return static_cast<MqttService*>(context)->run_command(argc, argv);
}

int MqttService::run_command(int argc, char** argv) {
	if (argc == 1 || (argc == 2 && std::strcmp(argv[1], "status") == 0)) {
		print_status();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "publish-discovery") == 0) {
		const char* error = nullptr;
		if (!mqtt_.publish_discovery(&error)) {
			std::printf("mqtt publish-discovery refused: %s\n", error);
			return 1;
		}
		std::printf("publishing the discovery configs again; removed functions are cleared within %u s\n",
			static_cast<unsigned>(Mqtt::kSweepMs / 1000));
		return 0;
	}
	if (argc == 4 && std::strcmp(argv[1], "set") == 0 && std::strcmp(argv[2], "password") == 0) {
		SettingsStatus status = Settings::check_string(argv[3]);
		if (status == SettingsStatus::ok) {
			status = controller_.settings().set_string(setting::kMqttPassword, argv[3]);
		}
		std::memset(argv[3], 0, std::strlen(argv[3]));
		if (status != SettingsStatus::ok) {
			std::printf("mqtt set password refused: %s\n", settings_status_name(status));
			return 1;
		}
		std::printf("broker password stored (setting %s); reconnecting\n", setting::kMqttPassword);
		mqtt_.reconnect("the broker password changed");
		return 0;
	}
	if (argc == 3 && std::strcmp(argv[1], "clear") == 0 && std::strcmp(argv[2], "password") == 0) {
		SettingsStatus status = controller_.settings().erase(setting::kMqttPassword);
		if (status != SettingsStatus::ok && status != SettingsStatus::not_found) {
			std::printf("mqtt clear password refused: %s\n", settings_status_name(status));
			return 1;
		}
		std::printf("broker password removed; reconnecting\n");
		mqtt_.reconnect("the broker password was removed");
		return 0;
	}
	std::printf("usage: mqtt %s\n", kHint);
	return 1;
}

void MqttService::print_status() const {
	MqttStatus s;
	mqtt_.status(s);
	if (s.broker[0] == '\0') {
		std::printf("mqtt: not configured (the runtime image's mqtt member configures it, spec/mqtt.md)\n");
		return;
	}
	uint64_t now = controller_.clock().monotonic_ms();
	std::printf("state      %s\n", mqtt_state_name(s.state));
	std::printf("broker     %s\n", s.broker);
	std::printf("user       %s, password %s\n", s.username[0] != '\0' ? s.username : "(setting mqtt.user or none)",
		s.password_set ? "set" : "not set");
	std::printf("base topic %s\n", s.base_topic);
	std::printf("discovery  %s, %u function(s)\n", s.discovery ? s.discovery_prefix : "off",
		static_cast<unsigned>(s.functions));
	if (s.history) {
		std::printf("history    every %u min, %lu message(s)\n", static_cast<unsigned>(s.history_interval_min),
			static_cast<unsigned long>(s.history_messages));
	} else {
		std::printf("history    off\n");
	}
	std::printf("traffic    %lu published, %lu command(s), %lu refused, %u state(s) waiting, outbox %u B, %lu inbound "
				"dropped\n",
		static_cast<unsigned long>(s.published), static_cast<unsigned long>(s.commands),
		static_cast<unsigned long>(s.refusals), static_cast<unsigned>(s.dirty), static_cast<unsigned>(s.pending),
		static_cast<unsigned long>(client_.dropped()));
	std::printf("connects   %lu, %lu failure(s) in a row\n", static_cast<unsigned long>(s.connects),
		static_cast<unsigned long>(s.failures));
	if (s.next_attempt_ms != 0) {
		std::printf("next       in %llu s\n",
			static_cast<unsigned long long>(s.next_attempt_ms > now ? (s.next_attempt_ms - now) / 1000 : 0));
	}
	if (s.last_error[0] != '\0') {
		std::printf("last error %s\n", s.last_error);
	}
}

} // namespace esp
} // namespace hc
