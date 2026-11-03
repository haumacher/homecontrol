/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_WS_CLIENT_H
#define HC_ESP_WS_CLIENT_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hc/platform/websocket_client.h"
#include "loop_queue.h"

namespace hc {
namespace esp {

/// The device's WebSocketClient (doc/ehome.md): esp_websocket_client (managed component, wss:// with
/// the IDF certificate bundle, ws:// for development) in its own task per connection.
///
/// - Receiving: the client task reassembles a text message (up to kMaxText) in the session's buffer
///   and hands binary messages over piece by piece (kBufferSize each, straight from the client's
///   receive buffer). Every event goes to the event loop through the LoopQueue and the client task
///   waits until the loop has consumed it, so one slot carries all events in order and nothing is
///   copied twice. A generation number drops events of an ended session.
/// - Sending (issue #55): the loop copies each message whole into the session's send queue
///   (kSendQueueBytes, a ring) or refuses it when it does not fit; the session's sender task
///   (below the loop's priority) drains the queue in order through the library under its separate
///   send lock (CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK) with kSendTimeoutMs, so the loop never waits
///   for the TLS write, its CPU time (about 6 ms per 1.3 KB on the C6) or the server's ACKs. A
///   message that wraps around the ring end goes out as two fragments of one message. After a sent
///   message the sender posts ws_client_writable() when the loop asked (want_writable()). A failed
///   write ends the session. The library's own ping is off: the core pings (send_ping) and judges
///   the pong.
/// - Ending: close() hands the session to its sender task, which drops what is still queued, sends
///   the close frame, stops the client task and frees everything, so the loop never waits for the
///   network. A new connection is refused until that teardown has ended (the core retries). A session the server or the network ended is reported once through
///   ws_client_closed() and torn down the same way.
class EspWebSocketClient : public WebSocketClient {
public:
	/// The client's receive and send buffer (each); messages are sent and received in frames of
	/// this size.
	static constexpr int kBufferSize = 1024;
	/// Stack of the client task: the TLS record layer and the handshake's certificate check (logged
	/// at every open: "client task stack ... unused"). Measured on the C6: 2612 B used (2026-11-03,
	/// wss to the development CA's certificate); 5120 B keep 2.5 KB for the handshake against a
	/// public chain through the certificate bundle (RSA, not measured on the desk) and the
	/// reconnects. 6144 B before issue #45.
	static constexpr int kTaskStack = 5120;
	static constexpr int kTaskPriority = 4;
	/// The send queue: one snapshot chunk of the live feed beside EhomeLink::kBulkReserve (a whole
	/// response and small messages), each message with a header of kQueueHeader bytes.
	static constexpr size_t kSendQueueBytes = 6144;
	static constexpr size_t kQueueHeader = 4;
	/// Stack of the sender task: the TLS write and, at the end, the close frame and the client's
	/// teardown (logged at the end of every session: "sender task stack ... unused").
	static constexpr uint32_t kSenderStack = 3072;
	/// Below the loop (5) and the network tasks (4): the TLS write costs CPU time.
	static constexpr int kSenderPriority = 3;
	/// How long one message may wait for room in the socket: longer than TCP needs to recover a
	/// lost segment (lwIP's retransmission timeout is a second or more), so a single loss on WiFi
	/// does not end the session (homecontrol #66: 2000 ms ended it while a 32 KB image streamed
	/// to E-Home). A dead peer is still found by the core's ping (EhomeLink::kPongTimeoutMs).
	static constexpr int kSendTimeoutMs = 10000;
	/// How long the teardown waits for the server's answer to the close frame.
	static constexpr int kCloseTimeoutMs = 2000;
	static constexpr int kNetworkTimeoutMs = 10000;

	explicit EspWebSocketClient(LoopQueue& queue);

	EspWebSocketClient(const EspWebSocketClient&) = delete;
	EspWebSocketClient& operator=(const EspWebSocketClient&) = delete;

	bool connect(const char* url, WebSocketClientListener& listener, const char** error) override;
	bool send_text(const char* text, size_t length) override;
	bool send_binary(const uint8_t* data, size_t length) override;
	bool send_ping() override;
	size_t send_room() override;
	void want_writable() override;
	void close(uint16_t code, const char* reason) override;

	/// Stack high-water mark of the current session's client task in bytes (0 without one), for
	/// the log line at "connected".
	uint32_t stack_free() const;

	struct Session;

private:
	static void on_event(void* context, const LoopMessage& message);
	static void on_closed(void* context, const LoopMessage& message);
	static void on_writable(void* context, const LoopMessage& message);
	static void sender(void* context);
	void end(Session* session, uint16_t code, const char* reason);
	bool send(int opcode, const void* data, size_t length);
	/// Free bytes of the session's queue; remembers how many messages it had sent by then.
	size_t queue_free(Session& session);

	LoopQueue& queue_;
	WebSocketClientListener* listener_ = nullptr;
	Session* session_ = nullptr;
	uint32_t generation_ = 0;
	/// Sessions whose resources still exist (the current one and those their sender tasks are
	/// still tearing down). connect() waits for 0: two TLS sessions at once cost about 30 KB and
	/// fragment the heap (#66: after an overlap the largest free block was 17.9 KB and a 32 KB
	/// image could not be received).
	std::atomic<uint32_t> alive_{0};
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_WS_CLIENT_H
