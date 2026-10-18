/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "esp_mqtt_client.h"

#include <cstdio>
#include <cstring>
#include <new>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "hc/log.h"
#include "hc/mqtt_codec.h"
#include "mqtt_client.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.mqtt";

// The kind of a loop event, in the top bits of LoopMessage::number.
constexpr int32_t kConnected = 1;
constexpr int32_t kDisconnected = 2;
constexpr int32_t kMessage = 3;
constexpr int32_t kKindShift = 28;
constexpr int32_t kValueMask = (1 << kKindShift) - 1;

int32_t pack(int32_t kind, uint32_t value) { return (kind << kKindShift) | static_cast<int32_t>(value & kValueMask); }

} // namespace

EspMqttClient::EspMqttClient(LoopQueue& queue) : queue_(queue) {
	for (auto& busy : slot_busy_) {
		busy.store(false);
	}
	free_slots_ = xSemaphoreCreateCountingStatic(kInboundSlots, kInboundSlots, &free_slots_storage_);
}

EspMqttClient::~EspMqttClient() {
	destroy();
	delete[] slots_;
}

void EspMqttClient::release_slots_if_idle() {
	if (handle_ != nullptr || slots_ == nullptr) {
		return;
	}
	for (auto& busy : slot_busy_) {
		if (busy.load()) {
			return; // its message is still queued for the loop, which frees the slots after it
		}
	}
	delete[] slots_;
	slots_ = nullptr;
}

void EspMqttClient::destroy() {
	if (handle_ == nullptr) {
		return;
	}
	auto client = static_cast<esp_mqtt_client_handle_t>(handle_);
	handle_ = nullptr;
	esp_mqtt_client_stop(client); // sends DISCONNECT when connected; waits for the task to end
	esp_mqtt_client_destroy(client);
}

bool EspMqttClient::connect(const MqttConnectOptions& options, MqttClientListener& listener, const char** error) {
	destroy();
	generation_.fetch_add(1);
	connected_ = false;
	listener_ = &listener;
	reason_[0] = '\0';
	mqtt::Broker broker;
	const char* reason = nullptr;
	if (!mqtt::parse_broker(options.broker, broker, &reason)) {
		*error = reason;
		return false;
	}
	if (slots_ == nullptr) {
		slots_ = new (std::nothrow) Slot[kInboundSlots];
		if (slots_ == nullptr) {
			*error = "no memory for the inbound message slots";
			return false;
		}
	}
	esp_mqtt_client_config_t config = {};
	config.broker.address.uri = options.broker;
	if (broker.tls) {
		config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
	}
	config.credentials.client_id = options.client_id;
	if (options.username != nullptr && options.username[0] != '\0') {
		config.credentials.username = options.username;
		if (options.password != nullptr && options.password[0] != '\0') {
			config.credentials.authentication.password = options.password;
		}
	}
	if (options.will_topic != nullptr && options.will_topic[0] != '\0') {
		config.session.last_will.topic = options.will_topic;
		config.session.last_will.msg = options.will_payload;
		config.session.last_will.msg_len = options.will_payload != nullptr ? static_cast<int>(std::strlen(options.will_payload)) : 0;
		config.session.last_will.qos = options.will_qos;
		config.session.last_will.retain = options.will_retain ? 1 : 0;
	}
	config.session.keepalive = options.keepalive_s;
	config.session.disable_clean_session = false;
	config.network.disable_auto_reconnect = true;
	config.network.timeout_ms = kNetworkTimeoutMs;
	config.buffer.size = kBufferBytes;
	config.buffer.out_size = kBufferBytes;
	config.outbox.limit = kOutboxLimit;
	config.task.stack_size = kTaskStack;
	// esp_mqtt_client_init copies the strings.
	esp_mqtt_client_handle_t client = esp_mqtt_client_init(&config);
	if (client == nullptr) {
		*error = "esp-mqtt: no memory for the client";
		release_slots_if_idle();
		return false;
	}
	esp_err_t err = esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, &EspMqttClient::on_event, this);
	if (err == ESP_OK) {
		err = esp_mqtt_client_start(client);
	}
	if (err != ESP_OK) {
		esp_mqtt_client_destroy(client);
		*error = "esp-mqtt: cannot start the client task";
		Log::error(kTag, "esp_mqtt_client_start: %s", esp_err_to_name(err));
		release_slots_if_idle();
		return false;
	}
	handle_ = client;
	return true;
}

void EspMqttClient::disconnect() {
	generation_.fetch_add(1);
	connected_ = false;
	destroy(); // the esp-mqtt task has ended: no slot is claimed any more
	release_slots_if_idle();
}

bool EspMqttClient::publish(const char* topic, const char* payload, size_t length, bool retain, uint8_t qos,
	const char** error) {
	if (handle_ == nullptr || !connected_) {
		*error = "not connected";
		return false;
	}
	auto client = static_cast<esp_mqtt_client_handle_t>(handle_);
	if (esp_mqtt_client_get_outbox_size(client) >= static_cast<int>(kOutboxLimit)) {
		*error = "outbox full";
		return false;
	}
	int id = esp_mqtt_client_enqueue(client, topic, payload, static_cast<int>(length), qos, retain ? 1 : 0, true);
	if (id == -2) {
		*error = "outbox full";
		return false;
	}
	if (id < 0) {
		*error = "esp-mqtt refused the message";
		return false;
	}
	return true;
}

bool EspMqttClient::subscribe(const char* filter, uint8_t qos, const char** error) {
	if (handle_ == nullptr || !connected_) {
		*error = "not connected";
		return false;
	}
	if (esp_mqtt_client_subscribe_single(static_cast<esp_mqtt_client_handle_t>(handle_), filter, qos) < 0) {
		*error = "esp-mqtt could not send the subscription";
		return false;
	}
	return true;
}

bool EspMqttClient::unsubscribe(const char* filter, const char** error) {
	if (handle_ == nullptr || !connected_) {
		*error = "not connected";
		return false;
	}
	if (esp_mqtt_client_unsubscribe(static_cast<esp_mqtt_client_handle_t>(handle_), filter) < 0) {
		*error = "esp-mqtt could not send the request";
		return false;
	}
	return true;
}

size_t EspMqttClient::pending() const {
	if (handle_ == nullptr) {
		return 0;
	}
	int size = esp_mqtt_client_get_outbox_size(static_cast<esp_mqtt_client_handle_t>(handle_));
	return size > 0 ? static_cast<size_t>(size) : 0;
}

// ---- esp-mqtt task -----------------------------------------------------------------------------

void EspMqttClient::post_state(int32_t number, const char* what) {
	// A lost connected or disconnected would leave the core in the wrong state: retry a full queue
	// for up to a second (the loop drains it in milliseconds).
	for (int i = 0; i < 100; ++i) {
		if (queue_.post(&EspMqttClient::on_loop_event, this, number, nullptr, what)) {
			return;
		}
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

void EspMqttClient::on_event(void* context, const char*, int32_t, void* data) {
	static_cast<EspMqttClient*>(context)->handle(data);
}

void EspMqttClient::handle(void* data) {
	auto event = static_cast<esp_mqtt_event_handle_t>(data);
	uint32_t generation = generation_.load();
	switch (event->event_id) {
		case MQTT_EVENT_BEFORE_CONNECT:
			reason_[0] = '\0';
			break;
		case MQTT_EVENT_ERROR: {
			const esp_mqtt_error_codes_t* e = event->error_handle;
			if (e == nullptr) {
				break;
			}
			if (e->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
				std::snprintf(reason_, sizeof(reason_), "connection refused: %s",
					mqtt::connack_reason(static_cast<uint8_t>(e->connect_return_code)));
			} else if (e->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
				std::snprintf(reason_, sizeof(reason_), "transport: %s (tls 0x%x, errno %d)",
					esp_err_to_name(e->esp_tls_last_esp_err), static_cast<unsigned>(e->esp_tls_stack_err),
					e->esp_transport_sock_errno);
			} else if (e->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED) {
				Log::warn(kTag, "the broker refused a subscription");
			}
			break;
		}
		case MQTT_EVENT_CONNECTED:
			post_state(pack(kConnected, generation), "mqtt connected");
			break;
		case MQTT_EVENT_DISCONNECTED:
			if (reason_[0] == '\0') {
				std::snprintf(reason_, sizeof(reason_), "connection closed");
			}
			post_state(pack(kDisconnected, generation), "mqtt disconnected");
			break;
		case MQTT_EVENT_DATA: {
			if (event->current_data_offset != 0) {
				break; // a later chunk of a message longer than the buffer: the first was delivered truncated
			}
			if (event->topic_len <= 0 || static_cast<size_t>(event->topic_len) > kMaxTopic) {
				dropped_.fetch_add(1);
				break;
			}
			if (xSemaphoreTake(free_slots_, pdMS_TO_TICKS(kSlotWaitMs)) != pdTRUE) {
				dropped_.fetch_add(1);
				Log::warn(kTag, "inbound message on %.*s dropped: the event loop is behind", event->topic_len,
					event->topic);
				break;
			}
			size_t index = 0;
			while (index < kInboundSlots && slot_busy_[index].exchange(true)) {
				++index;
			}
			if (index == kInboundSlots) { // cannot happen: the semaphore counts the free slots
				xSemaphoreGive(free_slots_);
				dropped_.fetch_add(1);
				break;
			}
			Slot& slot = slots_[index]; // allocated before the task started; freed only after it ended
			slot.generation = generation;
			slot.retained = event->retain;
			std::memcpy(slot.topic, event->topic, static_cast<size_t>(event->topic_len));
			slot.topic[event->topic_len] = '\0';
			size_t length = event->data_len > 0 ? static_cast<size_t>(event->data_len) : 0;
			if (length > kMaxInboundPayload) {
				length = kMaxInboundPayload;
			}
			std::memcpy(slot.payload, event->data, length);
			slot.length = length;
			slot.truncated = static_cast<size_t>(event->total_data_len) > length;
			if (!queue_.post(&EspMqttClient::on_loop_event, this, pack(kMessage, static_cast<uint32_t>(index)), nullptr,
					"mqtt message")) {
				slot_busy_[index].store(false);
				xSemaphoreGive(free_slots_);
				dropped_.fetch_add(1);
			}
			break;
		}
		default:
			break;
	}
}

// ---- Event loop --------------------------------------------------------------------------------

void EspMqttClient::on_loop_event(void* context, const LoopMessage& message) {
	auto self = static_cast<EspMqttClient*>(context);
	int32_t kind = message.number >> kKindShift;
	uint32_t value = static_cast<uint32_t>(message.number & kValueMask);
	uint32_t current = self->generation_.load() & kValueMask;
	switch (kind) {
		case kConnected:
			if (value == current && self->listener_ != nullptr) {
				self->connected_ = true;
				self->listener_->mqtt_connected();
			}
			break;
		case kDisconnected:
			if (value == current && self->listener_ != nullptr) {
				self->connected_ = false;
				self->listener_->mqtt_disconnected(self->reason_);
			}
			break;
		case kMessage: {
			if (value >= kInboundSlots || self->slots_ == nullptr) {
				break;
			}
			Slot& slot = self->slots_[value];
			if ((slot.generation & kValueMask) == current && self->listener_ != nullptr && self->connected_) {
				MqttMessage m{slot.topic, slot.payload, slot.length, slot.retained, slot.truncated};
				self->listener_->mqtt_message(m);
			}
			self->slot_busy_[value].store(false);
			xSemaphoreGive(self->free_slots_);
			self->release_slots_if_idle();
			break;
		}
		default:
			break;
	}
}

} // namespace esp
} // namespace hc
