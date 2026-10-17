/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_MQTT_SERVICE_H
#define HC_ESP_MQTT_SERVICE_H

#include "console.h"
#include "esp_mqtt_client.h"
#include "hc/controller.h"
#include "hc/datapoint.h"
#include "hc/history.h"
#include "hc/mqtt.h"
#include "loop_queue.h"

namespace hc {
namespace esp {

/// The MQTT client on the device (issue #18, doc/mqtt.md): the core's Mqtt over the EspMqttClient,
/// wired to the controller's loop, clock, settings (mqtt.password, mqtt.user) and network, to the
/// datapoint model and to the image's history. The runtime image's `mqtt` member configures it
/// (mqtt().configure_image(), from the image service); without one it does nothing.
///
/// It carries the console command `mqtt` (console_command(), for the console's extension table).
class MqttService {
public:
	/// `node_id` is the mDNS host name ("homecontrol-a1b2c3"); it must outlive the service.
	MqttService(Controller& controller, LoopQueue& queue, DatapointModel& model, const History* history,
		const char* node_id);

	MqttService(const MqttService&) = delete;
	MqttService& operator=(const MqttService&) = delete;

	Mqtt& mqtt() { return mqtt_; }

	/// The `mqtt` command for ConsoleContext::extensions.
	const ConsoleCommand* console_command() const { return &console_command_; }

	/// The body of `mqtt status | publish-discovery | set password <pw> | clear password` (runs on
	/// the event loop). Prints to stdout; 0 on success, 1 on a refusal (printed with its reason).
	int run_command(int argc, char** argv);

private:
	static int console_entry(void* context, int argc, char** argv);
	void print_status() const;

	Controller& controller_;
	EspMqttClient client_;
	Mqtt mqtt_;
	ConsoleCommand console_command_;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_MQTT_SERVICE_H
