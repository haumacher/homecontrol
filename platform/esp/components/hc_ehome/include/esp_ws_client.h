/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_WS_CLIENT_H
#define HC_ESP_WS_CLIENT_H

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
/// - Sending: on the loop, under the client's separate send lock
///   (CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK) with kSendTimeoutMs; a failed send ends the session.
///   The library's own ping is off: the core pings (send_ping) and judges the pong.
/// - Ending: close() hands the session to a short-lived task (kReaperStack) that sends the close
///   frame, stops the client task and frees everything, so the loop never waits for the network.
///   A session the server or the network ended is reported once through ws_client_closed() and torn
///   down the same way.
class EspWebSocketClient : public WebSocketClient {
public:
	/// The client's receive and send buffer (each); messages are sent and received in frames of
	/// this size.
	static constexpr int kBufferSize = 1024;
	/// Stack of the client task: the TLS record layer and the handshake's certificate check.
	static constexpr int kTaskStack = 6144;
	static constexpr int kTaskPriority = 4;
	static constexpr uint32_t kReaperStack = 3072;
	static constexpr int kSendTimeoutMs = 2000;
	static constexpr int kNetworkTimeoutMs = 10000;

	explicit EspWebSocketClient(LoopQueue& queue);

	EspWebSocketClient(const EspWebSocketClient&) = delete;
	EspWebSocketClient& operator=(const EspWebSocketClient&) = delete;

	bool connect(const char* url, WebSocketClientListener& listener, const char** error) override;
	bool send_text(const char* text, size_t length) override;
	bool send_binary(const uint8_t* data, size_t length) override;
	bool send_ping() override;
	void close(uint16_t code, const char* reason) override;

	/// Stack high-water mark of the current session's client task in bytes (0 without one), for
	/// the log line at "connected".
	uint32_t stack_free() const;

	struct Session;

private:
	static void on_event(void* context, const LoopMessage& message);
	static void reaper(void* context);
	void end(Session* session, uint16_t code, const char* reason);
	bool send(int opcode, const void* data, size_t length);

	LoopQueue& queue_;
	WebSocketClientListener* listener_ = nullptr;
	Session* session_ = nullptr;
	uint32_t generation_ = 0;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_WS_CLIENT_H
