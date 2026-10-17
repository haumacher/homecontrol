/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_ZIGBEE_SERVICE_H
#define HC_ESP_ZIGBEE_SERVICE_H

#include <cstdint>

#include "hc/controller.h"
#include "hc/datapoint.h"
#include "hc/json.h"
#include "hc/platform/blob_store.h"
#include "hc/zigbee/adapter.h"
#include "loop_queue.h"

namespace hc {
namespace esp {

/// The Zigbee coordinator on the device (doc/zigbee.md): the esp-zigbee-lib in its own task
/// (`hc_zigbee`), the core's ZigbeeAdapter on the event loop, the driver between them, and the
/// device database in the zb_db NVS partition. Answers and frames of the stack are copied into a
/// ring and handed to the adapter through the LoopQueue; the adapter's requests take the stack's
/// lock and call it directly from the loop task.
///
/// On a chip without an 802.15.4 radio (the ESP32-S3) or with HC_ZIGBEE_ENABLED off, the same class
/// is a stub: available() is false and every operation is refused with the reason.
///
/// Threading: every method on the event loop's task.
class ZigbeeService {
public:
	/// `model` is the controller's datapoint model; until runtime images exist (issue #8) the
	/// adapter loads the Zigbee datapoints into it alone (refused while it holds other datapoints).
	ZigbeeService(Controller& controller, LoopQueue& queue, DatapointModel& model);

	ZigbeeService(const ZigbeeService&) = delete;
	ZigbeeService& operator=(const ZigbeeService&) = delete;

	/// Opens the device database (`store`, or the zb_db partition when nullptr), starts the adapter
	/// and the Zigbee task (network formation on the first start). Call once, on the event loop's
	/// task, after the WiFi driver is initialized. Logs and stays unavailable when the partitions
	/// zb_storage/zb_fct are missing (an OTA-updated device with the old partition table).
	void init(BlobStore* store = nullptr);

	/// Whether the coordinator runs (compiled in, partitions present, task created).
	bool available() const;

	/// Why it is not available ("" when it is).
	const char* unavailable_reason() const;

	/// Opens the network for joining for `seconds` (1..254), 0 closes it: the hook for E-Home's
	/// pairing through the API (issue #16) and the console. False with `error`.
	bool permit_join(uint32_t seconds, const char** error);

	/// The adapter (devices, datapoints, operations); nullptr when not available.
	zigbee::ZigbeeAdapter* adapter();

	/// The adapter's JSON view (ZigbeeAdapter::write_json()), or {"available":false,"reason":...}.
	void write_json(JsonWriter& json);

	/// Frames of the stack dropped because the ring to the loop was full, since boot.
	uint32_t dropped() const;

	Controller& controller() { return controller_; }
	LoopQueue& queue() { return queue_; }
	DatapointModel& model() { return model_; }

private:
	Controller& controller_;
	LoopQueue& queue_;
	DatapointModel& model_;
};

/// The body of the console command `zigbee` (runs on the event loop, like every console command):
///
///   zigbee status | zigbee devices | zigbee permit <seconds> | zigbee remove <ieee>
///   zigbee identify <ieee> | zigbee label <ieee> [<text>] | zigbee interview <ieee>
///
/// Prints to stdout; returns 0 on success, 1 on a refusal (printed with its reason).
int zigbee_console_command(ZigbeeService& service, int argc, char** argv);

/// For the console's command table.
constexpr const char* kZigbeeCommandHint =
	"status | devices | permit <seconds> | remove <ieee> | identify <ieee> | label <ieee> [<text>] | interview <ieee>";
constexpr const char* kZigbeeCommandHelp =
	"Zigbee coordinator (doc/zigbee.md): network state, the joined devices and their datapoints, open the network "
	"for joining (0 closes), remove a device, make it blink, name it, interview it again";

} // namespace esp
} // namespace hc

#endif // HC_ESP_ZIGBEE_SERVICE_H
