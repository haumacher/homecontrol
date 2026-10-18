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

enum class Kind : uint8_t { opened, text, binary, pong, closed };

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

	// Client task only.
	bool closed_reported = false;
	uint8_t message_opcode = 0;  ///< The data message in progress (text or binary).
	size_t text_length = 0;
	bool text_overflow = false;
	uint16_t server_close_code = 0;

	// The slot handed to the loop (written by the client task before posting, read by the loop).
	Kind kind = Kind::opened;
	const uint8_t* data = nullptr;
	size_t length = 0;
	bool last = false;
	char reason[128] = {};
	char text[kMaxText + 1] = {};

	bool deliver(Kind what);
	void report_closed(const esp_websocket_event_data_t* event, bool clean);
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
	if (what == Kind::closed) {
		return true; // the last event: the loop tears the session down, nobody waits
	}
	while (xSemaphoreTake(consumed, kHandoverSlice) != pdTRUE) {
		if (abandoned.load()) {
			return false;
		}
	}
	return !abandoned.load();
}

void EspWebSocketClient::Session::report_closed(const esp_websocket_event_data_t* event, bool clean) {
	if (closed_reported) {
		return;
	}
	closed_reported = true;
	const esp_websocket_error_codes_t& e = event->error_handle;
	if (clean || server_close_code != 0 || e.error_type == WEBSOCKET_ERROR_TYPE_SERVER_CLOSE) {
		std::snprintf(reason, sizeof(reason), "closed by the server (%u)", static_cast<unsigned>(server_close_code));
	} else if (e.esp_ws_handshake_status_code != 0 && e.esp_ws_handshake_status_code != 101) {
		std::snprintf(reason, sizeof(reason), "the server refused the upgrade: HTTP %d", e.esp_ws_handshake_status_code);
	} else if (e.esp_tls_cert_verify_flags != 0) {
		std::snprintf(reason, sizeof(reason), "cannot connect: the server's certificate is not trusted (flags 0x%x)",
			static_cast<unsigned>(e.esp_tls_cert_verify_flags));
	} else if (e.esp_tls_last_esp_err != ESP_OK) {
		std::snprintf(reason, sizeof(reason), "cannot connect: %s (0x%x)", esp_err_to_name(e.esp_tls_last_esp_err),
			static_cast<unsigned>(-e.esp_tls_stack_err));
	} else if (e.esp_transport_sock_errno != 0) {
		std::snprintf(reason, sizeof(reason), "connection lost: %s", std::strerror(e.esp_transport_sock_errno));
	} else if (e.error_type == WEBSOCKET_ERROR_TYPE_PONG_TIMEOUT) {
		std::snprintf(reason, sizeof(reason), "no pong from the server");
	} else {
		std::snprintf(reason, sizeof(reason), "connection lost");
	}
	deliver(Kind::closed);
}

void EspWebSocketClient::Session::handler(void* arg, esp_event_base_t, int32_t id, void* event_data) {
	Session& s = *static_cast<Session*>(arg);
	const esp_websocket_event_data_t* event = static_cast<const esp_websocket_event_data_t*>(event_data);
	if (id == WEBSOCKET_EVENT_BEGIN) {
		s.task.store(xTaskGetCurrentTaskHandle());
		return;
	}
	if (xTaskGetCurrentTaskHandle() != s.task.load() && id != WEBSOCKET_EVENT_DISCONNECTED) {
		// A failed send aborts the connection on the sending task (the event loop), and the library
		// runs its pending events there: nothing may wait for the loop on the loop. The connection
		// ends; only its DISCONNECTED event matters (delivering "closed" does not wait).
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
						s.closed_reported = true;
						std::snprintf(s.reason, sizeof(s.reason), "the server sent a text message longer than %u bytes",
							static_cast<unsigned>(kMaxText));
						s.deliver(Kind::closed);
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
	if (s == nullptr) {
		*error = "no memory for the connection (5 KB)";
		return false;
	}
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
		delete s;
		*error = "WebSocket client not created (memory)";
		return false;
	}
	esp_websocket_register_events(s->client, WEBSOCKET_EVENT_ANY, &Session::handler, s);
	listener_ = &listener;
	session_ = s;
	if (esp_websocket_client_start(s->client) != ESP_OK) {
		session_ = nullptr;
		esp_websocket_client_destroy(s->client);
		delete s;
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
		case Kind::closed: {
			char reason[sizeof(s->reason)];
			std::memcpy(reason, s->reason, sizeof(reason));
			self.end(s, 1000, nullptr);
			listener.ws_client_closed(reason);
			return;
		}
	}
	if (self.session_ == s) {
		xSemaphoreGive(s->consumed); // the client task goes on (unless the listener closed meanwhile)
	}
}

bool EspWebSocketClient::send(int opcode, const void* data, size_t length) {
	Session* s = session_;
	if (s == nullptr || !esp_websocket_client_is_connected(s->client)) {
		return false;
	}
	int n = esp_websocket_client_send_with_opcode(s->client, static_cast<ws_transport_opcodes_t>(opcode),
		static_cast<const uint8_t*>(data), static_cast<int>(length), pdMS_TO_TICKS(kSendTimeoutMs));
	return n >= 0 && static_cast<size_t>(n) == length;
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
	if (xTaskCreate(&EspWebSocketClient::reaper, "hc_ehome_reap", kReaperStack, s, kTaskPriority, nullptr) != pdPASS) {
		// No memory for the helper: tear down here (the loop waits until the client task stopped,
		// about a second).
		ESP_LOGW(kTag, "no memory for the teardown task: closing the WebSocket on the event loop");
		esp_websocket_client_destroy(s->client);
		delete s;
	}
}

void EspWebSocketClient::reaper(void* context) {
	Session* s = static_cast<Session*>(context);
	if (esp_websocket_client_is_connected(s->client)) {
		// Sends the close frame and waits (bounded) for the server's answer and the task's end.
		esp_websocket_client_close_with_code(s->client, s->close_code, s->close_reason,
			static_cast<int>(std::strlen(s->close_reason)), pdMS_TO_TICKS(kSendTimeoutMs));
	}
	esp_websocket_client_destroy(s->client); // stops the task first if it still runs
	delete s;
	vTaskDelete(nullptr);
}

} // namespace esp
} // namespace hc
