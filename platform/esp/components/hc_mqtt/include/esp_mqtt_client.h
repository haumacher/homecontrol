/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_MQTT_CLIENT_H
#define HC_ESP_MQTT_CLIENT_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hc/platform/mqtt_client.h"
#include "loop_queue.h"

namespace hc {
namespace esp {

/// The device's MqttClient (doc/mqtt.md): esp-mqtt (ESP-IDF's `mqtt` component) with its own task,
/// automatic reconnection off (the core decides), TLS for `mqtts://` through the IDF certificate
/// bundle. Every publication is enqueued into esp-mqtt's outbox (esp_mqtt_client_enqueue: the loop
/// never waits for the network); the outbox is bounded by kOutboxLimit bytes, beyond which
/// publish() refuses and the core retries (it keeps the latest state of every datapoint, so
/// nothing newer is lost). Subscriptions are sent synchronously by esp-mqtt (a few bytes, on the
/// loop).
///
/// Events reach the loop through the LoopQueue: connected and disconnected carry the session's
/// generation (events of an ended session are dropped); a message is copied into one of
/// kInboundSlots slots (topic up to MqttClient::kMaxTopic, payload up to kMaxInboundPayload: a
/// longer one is delivered truncated, its later chunks skipped). When every slot is taken the
/// esp-mqtt task waits for one up to kSlotWaitMs (backpressure on the broker), then drops the
/// message and counts it.
class EspMqttClient : public MqttClient {
public:
	/// Bytes esp-mqtt's outbox may hold before publish() refuses. A state message costs about 150
	/// bytes there (topic, payload, the outbox record), a discovery config about 1.1 KB.
	static constexpr size_t kOutboxLimit = 16384;
	/// esp-mqtt's receive and send buffer: a discovery config (about 1 KB) goes out in one piece.
	static constexpr int kBufferBytes = 2048;
	static constexpr int kTaskStack = 6144;
	static constexpr int kNetworkTimeoutMs = 10000;
	static constexpr size_t kInboundSlots = 4;
	static constexpr size_t kMaxInboundPayload = 512;
	static constexpr uint32_t kSlotWaitMs = 2000;

	explicit EspMqttClient(LoopQueue& queue);
	~EspMqttClient();

	EspMqttClient(const EspMqttClient&) = delete;
	EspMqttClient& operator=(const EspMqttClient&) = delete;

	bool connect(const MqttConnectOptions& options, MqttClientListener& listener, const char** error) override;
	void disconnect() override;
	bool publish(const char* topic, const char* payload, size_t length, bool retain, uint8_t qos,
		const char** error) override;
	bool subscribe(const char* filter, uint8_t qos, const char** error) override;
	bool unsubscribe(const char* filter, const char** error) override;
	/// esp-mqtt's outbox in bytes (0: everything sent and, for QoS 1, acknowledged).
	size_t pending() const override;

	/// Messages dropped because no inbound slot became free (since boot).
	uint32_t dropped() const { return dropped_.load(); }

private:
	struct Slot {
		uint32_t generation;
		bool retained;
		bool truncated;
		size_t length;
		char topic[kMaxTopic + 1];
		char payload[kMaxInboundPayload];
	};

	static void on_event(void* context, const char* base, int32_t id, void* data);
	static void on_loop_event(void* context, const LoopMessage& message);
	void handle(void* event);
	void post_state(int32_t number, const char* what);
	void destroy();

	LoopQueue& queue_;
	MqttClientListener* listener_ = nullptr;
	void* handle_ = nullptr; ///< esp_mqtt_client_handle_t
	std::atomic<uint32_t> generation_{0};
	bool connected_ = false; ///< Loop thread: the current session delivered mqtt_connected().

	// Written by the esp-mqtt task, read on the loop after the posted event.
	char reason_[96] = {};
	bool skipping_ = false; ///< The later chunks of a message that did not fit are skipped.

	Slot slots_[kInboundSlots];
	std::atomic<bool> slot_busy_[kInboundSlots];
	SemaphoreHandle_t free_slots_ = nullptr;
	StaticSemaphore_t free_slots_storage_;
	std::atomic<uint32_t> dropped_{0};
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_MQTT_CLIENT_H
