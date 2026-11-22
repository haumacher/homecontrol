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
#include "heap_watch.h"
#include "hc/ehome_link.h"
#include "hc/live_feed.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#if CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK
// With the separate send lock the client task closes the transport under a lock the send does not
// hold, while the sender task writes (homecontrol #106).
#error "the E-Home client needs CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK off (sdkconfig.defaults.app; delete the build directory's sdkconfig)"
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

/// The opcode of a queue entry the sender passes over: the rest of the queue before a claim that
/// would have wrapped around its end (claim_text()).
constexpr uint8_t kSkipEntry = 0xFF;

// The queue takes one snapshot chunk beside the reserve EhomeLink keeps free of the live feed's bulk
// messages, and the largest single message (a response) whole.
static_assert(EspWebSocketClient::kSendQueueBytes >=
	EhomeLink::kBulkReserve + LiveFeed::kMessageSize + 2 * EspWebSocketClient::kQueueHeader,
	"the send queue must hold a snapshot chunk beside the E-Home reserve");
static_assert(EhomeLink::kStreamPiece < 0x10000 && EhomeLink::kOutBytes < 0x10000,
	"a queued message's length is 16 bits");

#if defined(CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN)
// The library writes a frame's header and its payload in pieces of at most kBufferSize: each is one TLS
// record, whose buffer the dynamic TLS buffers allocate at OUT_CONTENT_LEN (sdkconfig.defaults.app).
static_assert(EspWebSocketClient::kBufferSize <= CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN,
	"a piece of a frame must fit one outgoing TLS record");
#endif

/// Appends " [heap ...]" (the heap at the end of a connection, homecontrol #110) to a reason of
/// `length` bytes; WebSocketClientListener::ws_client_closed() names the bracket as details that do not
/// make a failure a different one.
void append_heap(char* reason, size_t size, int length) {
	if (length <= 0 || static_cast<size_t>(length) + 4 >= size) {
		return;
	}
	size_t n = static_cast<size_t>(length);
	reason[n++] = ' ';
	reason[n++] = '[';
	heap_watch::describe_now(reason + n, size - n - 1);
	n += std::strlen(reason + n);
	reason[n++] = ']';
	reason[n] = '\0';
}

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

	/// Held by the sender around every write and taken by the client task when it ends
	/// (WEBSOCKET_EVENT_FINISH), right before the library closes the transport without its lock: that
	/// close waits for a write in flight, and no write starts after it (homecontrol #106).
	SemaphoreHandle_t io = nullptr;
	StaticSemaphore_t io_storage;
	bool transport_ended = false; ///< Under `io`: the client task ended, the transport is closed.

	/// The CONNECTED event came (client task): a later failure ends an open session (read by whoever
	/// reports the end).
	std::atomic<bool> connected{false};
	/// The end was reported (by the client task, or by the sender when a write failed): once.
	std::atomic<bool> closed_reported{false};
	/// Written by the reporter before it posts on_closed; the core prefixes it ("connection lost: ")
	/// and appends the retry within one log line of 256 bytes (EhomeLink's error_).
	char closed_reason[200] = {};

	// Client task only.
	uint8_t message_opcode = 0;  ///< The data message in progress (text or binary).
	size_t text_length = 0;
	bool text_overflow = false;
	uint16_t server_close_code = 0;
	char server_close_reason[48] = {}; ///< The reason text of the server's close frame (printable bytes).

	/// The library's own account of the first error (WEBSOCKET_EVENT_ERROR, on whichever task failed:
	/// "esp_transport_read() failed with -2, ..., errno=11"), written once, before `library_error_ready`.
	char library_error[112] = {};
	std::atomic<bool> library_error_taken{false};
	std::atomic<bool> library_error_ready{false};

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
	bool claimed = false;               ///< Loop only: a claim_text() is open at `in`.
	uint32_t claim_max = 0;             ///< Loop only: its largest length.

	bool deliver(Kind what);
	void post_closed();
	void report_closed(const esp_websocket_event_data_t* event, bool clean);
	void report_failed_send();
	bool write(uint8_t opcode, uint32_t at, uint32_t length);
	bool transmit(uint8_t opcode, uint32_t at, uint32_t length);
	void send_close_frame();
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
	// What ended the connection, in the terms of the layer that reported it (homecontrol #112), then
	// the heap at this moment in brackets (#110). The core names the phase ("cannot connect",
	// "connection lost").
	char* reason = closed_reason;
	size_t size = sizeof(closed_reason);
	const esp_websocket_error_codes_t& e = event->error_handle;
	int n;
	if (clean || server_close_code != 0 || e.error_type == WEBSOCKET_ERROR_TYPE_SERVER_CLOSE) {
		n = std::snprintf(reason, size, "closed by the server (%u%s%s)", static_cast<unsigned>(server_close_code),
			server_close_reason[0] != '\0' ? ": " : "", server_close_reason);
	} else if (e.esp_ws_handshake_status_code != 0 && e.esp_ws_handshake_status_code != 101) {
		n = std::snprintf(reason, size, "the server refused the upgrade: HTTP %d", e.esp_ws_handshake_status_code);
	} else if (e.esp_tls_cert_verify_flags != 0) {
		n = std::snprintf(reason, size, "the server's certificate is not trusted (flags 0x%x)",
			static_cast<unsigned>(e.esp_tls_cert_verify_flags));
	} else if (e.esp_tls_last_esp_err != ESP_OK || e.esp_tls_stack_err != 0) {
		// esp-tls keeps mbedTLS's code negated (a positive number): shown as mbedTLS writes it.
		int tls = e.esp_tls_stack_err < 0 ? -e.esp_tls_stack_err : e.esp_tls_stack_err;
		n = std::snprintf(reason, size, "%s %s (mbedTLS -0x%04x, errno %d)", connected.load() ? "TLS" : "TLS connect",
			esp_err_to_name(e.esp_tls_last_esp_err), static_cast<unsigned>(tls), e.esp_transport_sock_errno);
	} else if (e.esp_transport_sock_errno != 0) {
		n = std::snprintf(reason, size, "socket: %s (errno %d)", std::strerror(e.esp_transport_sock_errno),
			e.esp_transport_sock_errno);
	} else if (e.error_type == WEBSOCKET_ERROR_TYPE_PONG_TIMEOUT) {
		n = std::snprintf(reason, size, "no pong from the server");
	} else if (library_error_ready.load(std::memory_order_acquire)) {
		// A read or poll that failed without an error code (a frame whose rest did not come within the
		// network timeout, kNetworkTimeoutMs, reads "failed with -2 ... errno=11"): the library's words.
		n = std::snprintf(reason, size, "%s", library_error);
	} else {
		n = std::snprintf(reason, size, "the transport failed without an error code (type %d)",
			static_cast<int>(e.error_type));
	}
	append_heap(reason, size, n);
	post_closed();
}

void EspWebSocketClient::Session::handler(void* arg, esp_event_base_t, int32_t id, void* event_data) {
	Session& s = *static_cast<Session*>(arg);
	const esp_websocket_event_data_t* event = static_cast<const esp_websocket_event_data_t*>(event_data);
	if (id == WEBSOCKET_EVENT_BEGIN) {
		s.task.store(xTaskGetCurrentTaskHandle());
		return;
	}
	if (id == WEBSOCKET_EVENT_ERROR) {
		// Comes right before the DISCONNECTED of a failed read, poll or write; the first one is kept.
		if (event->data_len > 0 && !s.library_error_taken.exchange(true)) {
			const uint8_t* p = reinterpret_cast<const uint8_t*>(event->data_ptr);
			size_t k = 0;
			for (int i = 0; i < event->data_len && p[i] != '\0' && k + 1 < sizeof(s.library_error); ++i) {
				s.library_error[k++] = p[i] >= 0x20 && p[i] < 0x7F ? static_cast<char>(p[i]) : '?';
			}
			s.library_error[k] = '\0';
			s.library_error_ready.store(true, std::memory_order_release);
		}
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
			s.connected.store(true);
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
		case WEBSOCKET_EVENT_FINISH:
			// The client task ends; the library closes the transport next, without its lock (after the
			// server's close, or after its own close of a broken connection). Waits for the sender's
			// write in flight (its lock and write timeouts bound it) and lets none start after.
			xSemaphoreTake(s.io, portMAX_DELAY);
			s.transport_ended = true;
			xSemaphoreGive(s.io);
			break;
		case WEBSOCKET_EVENT_DATA: {
			uint8_t opcode = event->op_code & 0x0F;
			if (opcode == 0x0A) {
				s.deliver(Kind::pong);
				break;
			}
			if (opcode == 0x08) {
				s.server_close_code = event->close_status_code > 0 ? static_cast<uint16_t>(event->close_status_code) : 0;
				const uint8_t* p = reinterpret_cast<const uint8_t*>(event->data_ptr);
				if (s.server_close_code == 0 && event->data_len >= 2) {
					s.server_close_code = static_cast<uint16_t>(p[0] << 8 | p[1]);
				}
				// The close frame's reason (after the code), printable bytes only.
				size_t k = 0;
				for (int i = 2; i < event->data_len && k + 1 < sizeof(s.server_close_reason); ++i) {
					s.server_close_reason[k++] = p[i] >= 0x20 && p[i] < 0x7F ? static_cast<char>(p[i]) : '?';
				}
				s.server_close_reason[k] = '\0';
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

void EspWebSocketClient::Session::report_failed_send() {
	// A write that fails on the socket makes the library end the connection, and its DISCONNECTED
	// event reports it; so does the client task when the connection ended meanwhile. A send the
	// library refused without either (its lock not free within the timeout) leaves the connection
	// open with a message missing: the session ends here.
	if (!esp_websocket_client_is_connected(client) || closed_reported.exchange(true)) {
		return;
	}
	int n = std::snprintf(closed_reason, sizeof(closed_reason), "a message could not be sent within %d s",
		kSendTimeoutMs / 1000);
	append_heap(closed_reason, sizeof(closed_reason), n);
	post_closed();
}

bool EspWebSocketClient::Session::write(uint8_t opcode, uint32_t at, uint32_t length) {
	xSemaphoreTake(io, portMAX_DELAY); // the client task holds it only to mark its end
	bool ok = !transport_ended && transmit(opcode, at, length);
	xSemaphoreGive(io);
	return ok;
}

void EspWebSocketClient::Session::send_close_frame() {
	// Under `io` like every write, with a bounded wait (the library's own close waits without
	// limit for its lock and the socket, and then for the client task, which the FINISH handler
	// would make wait for `io`).
	xSemaphoreTake(io, portMAX_DELAY);
	if (!transport_ended && esp_websocket_client_is_connected(client)) {
		uint8_t body[2 + sizeof(close_reason)];
		size_t length = std::strlen(close_reason);
		body[0] = static_cast<uint8_t>(close_code >> 8);
		body[1] = static_cast<uint8_t>(close_code & 0xFF);
		std::memcpy(body + 2, close_reason, length);
		esp_websocket_client_send_with_opcode(client, WS_TRANSPORT_OPCODES_CLOSE, body, static_cast<int>(2 + length),
			pdMS_TO_TICKS(kCloseTimeoutMs));
	}
	xSemaphoreGive(io);
}

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
		if (header[0] != kSkipEntry && !s->failed && esp_websocket_client_is_connected(s->client)) {
			// A failed write aborts the connection here; its DISCONNECTED event reports the end.
			s->failed = !s->write(header[0], at, length);
			if (s->failed) {
				s->report_failed_send();
			}
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
	// The loop let go: what is still queued is dropped. The close frame, then the teardown: the
	// client task ends after its current read (it reads the server's answer to the close frame if
	// that comes within it), the library closes the transport, and everything is freed.
	s->send_close_frame();
	esp_websocket_client_destroy(s->client); // stops the client task first if it still runs
	ESP_LOGI(kTag, "WebSocket session ended: sender task stack %u of %u B unused",
		static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)),
		static_cast<unsigned>(kSenderStack));
	EspWebSocketClient* owner = s->owner;
	delete[] s->queue;
	delete s;
	owner->alive_.fetch_sub(1);
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
	if (alive_.load() > 0) {
		*error = "the previous connection is still closing";
		return false;
	}
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
	s->io = xSemaphoreCreateMutexStatic(&s->io_storage);

	esp_websocket_client_config_t config = {};
	config.uri = url;
	// No reconnect by the client task (the core reconnects with a new session) and no reconnect after
	// the server's close (enable_close_reconnect stays off): the client task ends with its connection,
	// and its FINISH event comes before the transport's last close.
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
	alive_.fetch_add(1); // the sender frees the session and counts it down
	if (xTaskCreate(&EspWebSocketClient::sender, "hc_ehome_tx", kSenderStack, s, kSenderPriority, &s->sender) !=
		pdPASS) {
		alive_.fetch_sub(1);
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

char* EspWebSocketClient::claim_text(size_t max) {
	Session* s = session_;
	if (s == nullptr || s->claimed || !esp_websocket_client_is_connected(s->client) || max > 0xFFFF) {
		return nullptr;
	}
	size_t free = queue_free(*s);
	uint32_t at = static_cast<uint32_t>((s->in + kQueueHeader) % kSendQueueBytes);
	if (at + max > kSendQueueBytes) {
		// The text would wrap around the end (the header does not: then `at` would be at the
		// front): a skip entry takes the rest, the claim starts at the front.
		uint32_t rest = static_cast<uint32_t>(kSendQueueBytes - at);
		if (free < 2 * kQueueHeader + rest + max) {
			return nullptr;
		}
		uint8_t header[kQueueHeader] = {kSkipEntry, 0, static_cast<uint8_t>(rest & 0xFF), static_cast<uint8_t>(rest >> 8)};
		std::memcpy(s->queue + s->in, header, kQueueHeader);
		s->in = 0;
		s->used.fetch_add(kQueueHeader + rest, std::memory_order_release);
		xTaskNotifyGive(s->sender);
		at = kQueueHeader;
	} else if (free < kQueueHeader + max) {
		return nullptr;
	}
	s->claimed = true;
	s->claim_max = static_cast<uint32_t>(max);
	return reinterpret_cast<char*>(s->queue + at);
}

void EspWebSocketClient::commit_text(size_t length) {
	Session* s = session_;
	if (s == nullptr || !s->claimed) {
		return;
	}
	s->claimed = false;
	if (length > s->claim_max) {
		return;
	}
	uint8_t header[kQueueHeader] = {static_cast<uint8_t>(WS_TRANSPORT_OPCODES_TEXT), 0,
		static_cast<uint8_t>(length & 0xFF), static_cast<uint8_t>(length >> 8)};
	for (size_t i = 0; i < kQueueHeader; ++i) {
		s->queue[(s->in + i) % kSendQueueBytes] = header[i];
	}
	s->in = static_cast<uint32_t>((s->in + kQueueHeader + length) % kSendQueueBytes);
	s->used.fetch_add(static_cast<uint32_t>(kQueueHeader + length), std::memory_order_release);
	xTaskNotifyGive(s->sender);
}

void EspWebSocketClient::abandon_text() {
	if (session_ != nullptr) {
		session_->claimed = false;
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
