/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "esp_ws_client.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

#include "sdkconfig.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hc/ehome_link.h"
#include "hc/live_feed.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#if !CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK
#error "the E-Home client sends from the event loop while the client task waits for it: CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK"
#endif

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.ws";

/// The library pings by itself every ping_interval_sec; the core pings instead (EhomeLink::kPingMs),
/// so the library's interval is set beyond any session (the product with 1000 still fits 32 bits).
constexpr size_t kNoPingS = 1000000;

/// How long the client task waits in one slice for the loop to consume an event (it rechecks
/// whether the session was abandoned between the slices).
constexpr TickType_t kHandoverSlice = pdMS_TO_TICKS(1000);

enum class Kind : uint8_t { opened, text, binary, pong };

// The queue takes one snapshot chunk beside the reserve EhomeLink keeps free of the live feed's bulk
// messages, and the largest single message (a response) whole.
static_assert(EspWebSocketClient::kSendQueueBytes >=
	EhomeLink::kBulkReserve + LiveFeed::kMessageSize + 2 * EspWebSocketClient::kQueueHeader,
	"the send queue must hold a snapshot chunk beside the E-Home reserve");
static_assert(EhomeLink::kStreamPiece < 0x10000 && EhomeLink::kOutBytes < 0x10000,
	"a queued message's length is 16 bits");

} // namespace

struct EspWebSocketClient::Session {
	EspWebSocketClient* owner = nullptr;
	uint32_t generation = 0;
	esp_websocket_client_handle_t client = nullptr;
	SemaphoreHandle_t consumed = nullptr;
	StaticSemaphore_t consumed_storage;
	/// Set by the loop when it let go of the session: the client task delivers nothing more.
	std::atomic<bool> abandoned{false};
	// Close request for the reaper.
	uint16_t close_code = 1000;
	char close_reason[64] = {};

	/// The client task (from its BEGIN event).
	std::atomic<TaskHandle_t> task{nullptr};
	/// The sender task: drains the send queue, then tears the session down.
	TaskHandle_t sender = nullptr;

	/// The end was reported (by the client task, or by the sender when a write failed): once.
	std::atomic<bool> closed_reported{false};
	char closed_reason[128] = {}; ///< Written by the reporter before it posts on_closed.

	// Client task only.
	uint8_t message_opcode = 0;  ///< The data message in progress (text or binary).
	size_t text_length = 0;
	bool text_overflow = false;
	uint16_t server_close_code = 0;

	// The slot handed to the loop (written by the client task before posting, read by the loop).
	Kind kind = Kind::opened;
	const uint8_t* data = nullptr;
	size_t length = 0;
	bool last = false;
	char text[kMaxText + 1] = {};

	// The send queue (kSendQueueBytes): the loop writes at `in`, the sender reads at `out`; `used`
	// hands the bytes over (release on either side).
	uint8_t* queue = nullptr;
	std::atomic<uint32_t> used{0};
	uint32_t in = 0;          ///< Loop only.
	uint32_t out = 0;         ///< Sender only.
	std::atomic<uint32_t> sent{0};      ///< Messages the sender finished.
	uint32_t sent_seen = 0;             ///< Loop only: `sent` at the last room check.
	std::atomic<bool> writable_wanted{false};
	bool failed = false;                ///< Sender only: a write failed, the session ends.

	bool deliver(Kind what);
	void post_closed();
	void report_closed(const esp_websocket_event_data_t* event, bool clean);
	bool transmit(uint8_t opcode, uint32_t at, uint32_t length);
	static void handler(void* arg, esp_event_base_t base, int32_t id, void* event_data);
};

// ---- The client task's side -------------------------------------------------------------------

bool EspWebSocketClient::Session::deliver(Kind what) {
	if (abandoned.load()) {
		return false;
	}
	kind = what;
	// Post until the queue takes it (it is full only for moments) or the loop let go.
	while (!owner->queue_.post(&EspWebSocketClient::on_event, owner, static_cast<int32_t>(generation), nullptr,
		"E-Home socket event")) {
		if (abandoned.load()) {
			return false;
		}
		vTaskDelay(pdMS_TO_TICKS(20));
	}
	while (xSemaphoreTake(consumed, kHandoverSlice) != pdTRUE) {
		if (abandoned.load()) {
			return false;
		}
	}
	return !abandoned.load();
}

void EspWebSocketClient::Session::post_closed() {
	// The last event: the loop tears the session down, nobody waits. Its own message, not the slot,
	// which the client task may be using when the sender reports a failed write.
	while (!abandoned.load() && !owner->queue_.post(&EspWebSocketClient::on_closed, owner,
		static_cast<int32_t>(generation), nullptr, "E-Home socket closed")) {
		vTaskDelay(pdMS_TO_TICKS(20));
	}
}

void EspWebSocketClient::Session::report_closed(const esp_websocket_event_data_t* event, bool clean) {
	if (closed_reported.exchange(true)) {
		return;
	}
	char* reason = closed_reason;
	const esp_websocket_error_codes_t& e = event->error_handle;
	if (clean || server_close_code != 0 || e.error_type == WEBSOCKET_ERROR_TYPE_SERVER_CLOSE) {
		std::snprintf(reason, sizeof(closed_reason), "closed by the server (%u)", static_cast<unsigned>(server_close_code));
	} else if (e.esp_ws_handshake_status_code != 0 && e.esp_ws_handshake_status_code != 101) {
		std::snprintf(reason, sizeof(closed_reason), "the server refused the upgrade: HTTP %d", e.esp_ws_handshake_status_code);
	} else if (e.esp_tls_cert_verify_flags != 0) {
		std::snprintf(reason, sizeof(closed_reason), "cannot connect: the server's certificate is not trusted (flags 0x%x)",
			static_cast<unsigned>(e.esp_tls_cert_verify_flags));
	} else if (e.esp_tls_last_esp_err != ESP_OK) {
		std::snprintf(reason, sizeof(closed_reason), "cannot connect: %s (0x%x)", esp_err_to_name(e.esp_tls_last_esp_err),
			static_cast<unsigned>(-e.esp_tls_stack_err));
	} else if (e.esp_transport_sock_errno != 0) {
		std::snprintf(reason, sizeof(closed_reason), "connection lost: %s", std::strerror(e.esp_transport_sock_errno));
	} else if (e.error_type == WEBSOCKET_ERROR_TYPE_PONG_TIMEOUT) {
		std::snprintf(reason, sizeof(closed_reason), "no pong from the server");
	} else {
		std::snprintf(reason, sizeof(closed_reason), "connection lost");
	}
	post_closed();
}

void EspWebSocketClient::Session::handler(void* arg, esp_event_base_t, int32_t id, void* event_data) {
	Session& s = *static_cast<Session*>(arg);
	const esp_websocket_event_data_t* event = static_cast<const esp_websocket_event_data_t*>(event_data);
	if (id == WEBSOCKET_EVENT_BEGIN) {
		s.task.store(xTaskGetCurrentTaskHandle());
		return;
	}
	if (xTaskGetCurrentTaskHandle() != s.task.load() && id != WEBSOCKET_EVENT_DISCONNECTED) {
		// A failed send aborts the connection on the sending task (the sender), and the library runs
		// its pending events there, as it does during the sender's teardown. The connection ends;
		// only its DISCONNECTED event matters (reporting "closed" does not wait for the loop).
		return;
	}
	switch (id) {
		case WEBSOCKET_EVENT_CONNECTED:
			s.message_opcode = 0;
			s.text_length = 0;
			s.text_overflow = false;
			s.deliver(Kind::opened);
			break;
		case WEBSOCKET_EVENT_DISCONNECTED:
			s.report_closed(event, false);
			break;
		case WEBSOCKET_EVENT_CLOSED:
			s.report_closed(event, true);
			break;
		case WEBSOCKET_EVENT_DATA: {
			uint8_t opcode = event->op_code & 0x0F;
			if (opcode == 0x0A) {
				s.deliver(Kind::pong);
				break;
			}
			if (opcode == 0x08) {
				s.server_close_code = event->close_status_code > 0 ? static_cast<uint16_t>(event->close_status_code) : 0;
				if (s.server_close_code == 0 && event->data_len >= 2) {
					const uint8_t* p = reinterpret_cast<const uint8_t*>(event->data_ptr);
					s.server_close_code = static_cast<uint16_t>(p[0] << 8 | p[1]);
				}
				break;
			}
			if (opcode == 0x09) {
				break; // the library answers pings
			}
			if (opcode == 0x01 || opcode == 0x02) {
				if (event->payload_offset == 0) {
					s.message_opcode = opcode; // a new message (its first frame)
					s.text_length = 0;
					s.text_overflow = false;
				}
			} else if (opcode != 0x00) {
				break;
			}
			bool frame_done = event->payload_offset + event->data_len >= event->payload_len;
			bool message_done = frame_done && event->fin;
			if (s.message_opcode == 0x02) {
				if (event->data_len > 0 || message_done) {
					s.data = reinterpret_cast<const uint8_t*>(event->data_ptr);
					s.length = static_cast<size_t>(event->data_len);
					s.last = message_done;
					s.deliver(Kind::binary);
				}
			} else if (s.message_opcode == 0x01) {
				size_t n = static_cast<size_t>(event->data_len);
				if (s.text_length + n > kMaxText) {
					s.text_overflow = true;
				} else if (n > 0) {
					std::memcpy(s.text + s.text_length, event->data_ptr, n);
					s.text_length += n;
				}
				if (message_done) {
					if (s.text_overflow) {
						if (!s.closed_reported.exchange(true)) {
							std::snprintf(s.closed_reason, sizeof(s.closed_reason),
								"the server sent a text message longer than %u bytes", static_cast<unsigned>(kMaxText));
							s.post_closed();
						}
					} else {
						s.text[s.text_length] = '\0';
						s.length = s.text_length;
						s.deliver(Kind::text);
					}
					s.text_length = 0;
				}
			}
			break;
		}
		default:
			break;
	}
}

// ---- The sender task --------------------------------------------------------------------------

bool EspWebSocketClient::Session::transmit(uint8_t opcode, uint32_t at, uint32_t length) {
	TickType_t timeout = pdMS_TO_TICKS(kSendTimeoutMs);
	const uint8_t* data = queue + at;
	if (at + length <= kSendQueueBytes) {
		int n = esp_websocket_client_send_with_opcode(client, static_cast<ws_transport_opcodes_t>(opcode), data,
			static_cast<int>(length), timeout);
		return n >= 0 && static_cast<uint32_t>(n) == length;
	}
	// Wrapped around the ring's end: one message in two fragments (the library fragments at
	// kBufferSize anyway), the first without FIN, the second a continuation with FIN.
	int first = static_cast<int>(kSendQueueBytes - at);
	int second = static_cast<int>(length) - first;
	int n = opcode == WS_TRANSPORT_OPCODES_BINARY
		? esp_websocket_client_send_bin_partial(client, reinterpret_cast<const char*>(data), first, timeout)
		: esp_websocket_client_send_text_partial(client, reinterpret_cast<const char*>(data), first, timeout);
	if (n != first) {
		return false;
	}
	n = esp_websocket_client_send_with_opcode(client, WS_TRANSPORT_OPCODES_CONT, queue, second, timeout);
	return n == second;
}

void EspWebSocketClient::sender(void* context) {
	Session* s = static_cast<Session*>(context);
	while (!s->abandoned.load()) {
		uint32_t used = s->used.load(std::memory_order_acquire);
		if (used == 0) {
			ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // the loop queued a message or let go
			continue;
		}
		uint8_t header[kQueueHeader];
		for (size_t i = 0; i < kQueueHeader; ++i) {
			header[i] = s->queue[(s->out + i) % kSendQueueBytes];
		}
		uint32_t length = static_cast<uint32_t>(header[2]) | static_cast<uint32_t>(header[3]) << 8;
		uint32_t at = static_cast<uint32_t>((s->out + kQueueHeader) % kSendQueueBytes);
		if (!s->failed && esp_websocket_client_is_connected(s->client)) {
			// A failed write aborts the connection here; its DISCONNECTED event reports the end.
			s->failed = !s->transmit(header[0], at, length);
		}
		s->out = static_cast<uint32_t>((at + length) % kSendQueueBytes);
		s->used.fetch_sub(static_cast<uint32_t>(kQueueHeader + length), std::memory_order_release);
		s->sent.fetch_add(1);
		if (s->writable_wanted.exchange(false) &&
			!s->owner->queue_.post(&EspWebSocketClient::on_writable, s->owner, static_cast<int32_t>(s->generation),
				nullptr, "E-Home socket writable")) {
			s->writable_wanted.store(true); // the next message tries again; the core's ticks retry anyway
		}
	}
	// The loop let go: what is still queued is dropped. The close frame, then the teardown.
	if (esp_websocket_client_is_connected(s->client)) {
		// Sends the close frame and waits (bounded) for the server's answer and the task's end.
		esp_websocket_client_close_with_code(s->client, s->close_code, s->close_reason,
			static_cast<int>(std::strlen(s->close_reason)), pdMS_TO_TICKS(kSendTimeoutMs));
	}
	esp_websocket_client_destroy(s->client); // stops the client task first if it still runs
	ESP_LOGI(kTag, "WebSocket session ended: sender task stack %u of %u B unused",
		static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)),
		static_cast<unsigned>(kSenderStack));
	delete[] s->queue;
	delete s;
	vTaskDelete(nullptr);
}

// ---- The loop's side --------------------------------------------------------------------------

EspWebSocketClient::EspWebSocketClient(LoopQueue& queue) : queue_(queue) {}

bool EspWebSocketClient::connect(const char* url, WebSocketClientListener& listener, const char** error) {
	if (session_ != nullptr) {
		*error = "a connection is open";
		return false;
	}
	size_t length = std::strlen(url);
	bool tls = std::strncmp(url, "wss://", 6) == 0;
	if (length > kMaxUrl || (!tls && std::strncmp(url, "ws://", 5) != 0)) {
		*error = "not a ws:// or wss:// address of at most 255 bytes";
		return false;
	}
#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
	if (tls) {
		*error = "wss:// needs the certificate bundle (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)";
		return false;
	}
#endif
	Session* s = new (std::nothrow) Session();
	uint8_t* queue = new (std::nothrow) uint8_t[kSendQueueBytes];
	if (s == nullptr || queue == nullptr) {
		delete s;
		delete[] queue;
		*error = "no memory for the connection (4.5 KB and the 6 KB send queue)";
		return false;
	}
	s->queue = queue;
	s->owner = this;
	s->generation = ++generation_;
	s->consumed = xSemaphoreCreateBinaryStatic(&s->consumed_storage);

	esp_websocket_client_config_t config = {};
	config.uri = url;
	config.disable_auto_reconnect = true;
	config.task_prio = kTaskPriority;
	config.task_name = "hc_ehome_ws";
	config.task_stack = kTaskStack;
	config.buffer_size = kBufferSize;
	config.network_timeout_ms = kNetworkTimeoutMs;
	config.ping_interval_sec = kNoPingS;
	config.disable_pingpong_discon = true;
	config.task_core_id = tskNO_AFFINITY;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
	if (tls) {
		config.crt_bundle_attach = esp_crt_bundle_attach;
	}
#endif
	s->client = esp_websocket_client_init(&config);
	if (s->client == nullptr) {
		delete[] s->queue;
		delete s;
		*error = "WebSocket client not created (memory)";
		return false;
	}
	esp_websocket_register_events(s->client, WEBSOCKET_EVENT_ANY, &Session::handler, s);
	if (xTaskCreate(&EspWebSocketClient::sender, "hc_ehome_tx", kSenderStack, s, kSenderPriority, &s->sender) !=
		pdPASS) {
		esp_websocket_client_destroy(s->client); // not started: no task to stop
		delete[] s->queue;
		delete s;
		*error = "cannot create the WebSocket sender task (memory)";
		return false;
	}
	listener_ = &listener;
	session_ = s;
	if (esp_websocket_client_start(s->client) != ESP_OK) {
		end(s, 1000, nullptr); // the sender tears it down
		*error = "cannot create the WebSocket task (memory)";
		return false;
	}
	return true;
}

void EspWebSocketClient::on_event(void* context, const LoopMessage& message) {
	EspWebSocketClient& self = *static_cast<EspWebSocketClient*>(context);
	Session* s = self.session_;
	if (s == nullptr || s->generation != static_cast<uint32_t>(message.number)) {
		return; // an ended session's event: its task was released when it ended
	}
	WebSocketClientListener& listener = *self.listener_;
	switch (s->kind) {
		case Kind::opened:
			// The TLS handshake is the deepest the client task's stack goes: the figures of the design.
			ESP_LOGI(kTag, "WebSocket open: client task stack %u of %u B unused, free heap %u B (minimum %u B)",
				static_cast<unsigned>(self.stack_free()), static_cast<unsigned>(kTaskStack),
				static_cast<unsigned>(esp_get_free_heap_size()), static_cast<unsigned>(esp_get_minimum_free_heap_size()));
			listener.ws_client_opened();
			break;
		case Kind::text:
			listener.ws_client_text(s->text, s->length);
			break;
		case Kind::binary:
			listener.ws_client_binary(s->data, s->length, s->last);
			break;
		case Kind::pong:
			listener.ws_client_pong();
			break;
	}
	if (self.session_ == s) {
		xSemaphoreGive(s->consumed); // the client task goes on (unless the listener closed meanwhile)
	}
}

void EspWebSocketClient::on_closed(void* context, const LoopMessage& message) {
	EspWebSocketClient& self = *static_cast<EspWebSocketClient*>(context);
	Session* s = self.session_;
	if (s == nullptr || s->generation != static_cast<uint32_t>(message.number)) {
		return;
	}
	char reason[sizeof(s->closed_reason)];
	std::memcpy(reason, s->closed_reason, sizeof(reason));
	self.end(s, 1000, nullptr);
	self.listener_->ws_client_closed(reason);
}

void EspWebSocketClient::on_writable(void* context, const LoopMessage& message) {
	EspWebSocketClient& self = *static_cast<EspWebSocketClient*>(context);
	Session* s = self.session_;
	if (s != nullptr && s->generation == static_cast<uint32_t>(message.number)) {
		self.listener_->ws_client_writable();
	}
}

size_t EspWebSocketClient::queue_free(Session& s) {
	s.sent_seen = s.sent.load();
	return kSendQueueBytes - s.used.load(std::memory_order_acquire);
}

bool EspWebSocketClient::send(int opcode, const void* data, size_t length) {
	Session* s = session_;
	if (s == nullptr || !esp_websocket_client_is_connected(s->client) || length > 0xFFFF ||
		kQueueHeader + length > queue_free(*s)) {
		return false;
	}
	uint8_t header[kQueueHeader] = {static_cast<uint8_t>(opcode), 0, static_cast<uint8_t>(length & 0xFF),
		static_cast<uint8_t>(length >> 8)};
	const uint8_t* bytes = static_cast<const uint8_t*>(data);
	uint32_t at = s->in;
	for (size_t i = 0; i < kQueueHeader; ++i) {
		s->queue[(at + i) % kSendQueueBytes] = header[i];
	}
	at = static_cast<uint32_t>((at + kQueueHeader) % kSendQueueBytes);
	size_t first = length < kSendQueueBytes - at ? length : kSendQueueBytes - at;
	if (first > 0) {
		std::memcpy(s->queue + at, bytes, first);
	}
	if (length > first) {
		std::memcpy(s->queue, bytes + first, length - first);
	}
	s->in = static_cast<uint32_t>((at + length) % kSendQueueBytes);
	s->used.fetch_add(static_cast<uint32_t>(kQueueHeader + length), std::memory_order_release);
	xTaskNotifyGive(s->sender);
	return true;
}

size_t EspWebSocketClient::send_room() {
	Session* s = session_;
	if (s == nullptr || !esp_websocket_client_is_connected(s->client)) {
		return 0;
	}
	size_t free = queue_free(*s);
	return free > kQueueHeader ? free - kQueueHeader : 0;
}

void EspWebSocketClient::want_writable() {
	Session* s = session_;
	if (s == nullptr) {
		return;
	}
	s->writable_wanted.store(true);
	// The sender finished a message since the loop looked at the room: it may have missed the flag.
	if (s->sent.load() != s->sent_seen && s->writable_wanted.exchange(false)) {
		if (!queue_.post(&EspWebSocketClient::on_writable, this, static_cast<int32_t>(s->generation), nullptr,
			"E-Home socket writable")) {
			s->writable_wanted.store(true);
		}
	}
}

bool EspWebSocketClient::send_text(const char* text, size_t length) {
	return send(WS_TRANSPORT_OPCODES_TEXT, text, length);
}

bool EspWebSocketClient::send_binary(const uint8_t* data, size_t length) {
	return send(WS_TRANSPORT_OPCODES_BINARY, data, length);
}

bool EspWebSocketClient::send_ping() {
	return send(WS_TRANSPORT_OPCODES_PING, nullptr, 0);
}

void EspWebSocketClient::close(uint16_t code, const char* reason) {
	if (session_ != nullptr) {
		end(session_, code, reason);
	}
}

uint32_t EspWebSocketClient::stack_free() const {
	TaskHandle_t task = session_ != nullptr ? session_->task.load() : nullptr;
	return task != nullptr ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(task)) * sizeof(StackType_t) : 0;
}

void EspWebSocketClient::end(Session* s, uint16_t code, const char* reason) {
	session_ = nullptr;
	s->close_code = code;
	std::snprintf(s->close_reason, sizeof(s->close_reason), "%s", reason != nullptr ? reason : "");
	s->abandoned.store(true);
	xSemaphoreGive(s->consumed); // wake a client task waiting for the loop
	xTaskNotifyGive(s->sender);  // the sender drops the queue and tears the session down
}

} // namespace esp
} // namespace hc
