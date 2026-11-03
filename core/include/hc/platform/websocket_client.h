/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_PLATFORM_WEBSOCKET_CLIENT_H
#define HC_PLATFORM_WEBSOCKET_CLIENT_H

#include <cstddef>
#include <cstdint>

namespace hc {

/// The events of a WebSocket client session, always on the event loop's thread, never from within
/// a WebSocketClient call.
class WebSocketClientListener {
public:
	/// The handshake completed: frames may be sent.
	virtual void ws_client_opened() = 0;
	/// A complete text message (reassembled from its fragments). Valid during the call only.
	virtual void ws_client_text(const char* text, size_t length) = 0;
	/// A piece of a binary message, in order; `last` on the message's final piece. Valid during the
	/// call only. The platform hands pieces of its own size (its receive buffer).
	virtual void ws_client_binary(const uint8_t* data, size_t length, bool last) = 0;
	/// A pong arrived (the answer to send_ping()).
	virtual void ws_client_pong() = 0;
	/// The client sent a queued message, so its send room grew (once per want_writable()).
	virtual void ws_client_writable() = 0;
	/// The connection attempt failed or an established connection ended; `reason` says why
	/// ("cannot connect: ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST", "closed by the server (1008)",
	/// "a text message longer than 4096 bytes"). After it the client is idle until the next
	/// connect().
	virtual void ws_client_closed(const char* reason) = 0;

protected:
	~WebSocketClientListener() = default;
};

/// An outbound WebSocket client of the platform (doc/ehome.md): esp_websocket_client with TLS
/// through the IDF certificate bundle on the ESP, a small client over a TCP socket on POSIX (plain
/// ws:// only), a scripted fake in the tests. The core decides when to connect and reconnect (the
/// client never reconnects by itself) and when to ping; the platform moves the bytes off the event
/// loop and reports back on the loop's thread.
///
/// A session: connect() starts it; exactly one ws_client_opened() or ws_client_closed() follows;
/// after ws_client_opened() a ws_client_closed() follows when the connection ends, unless the core
/// ended it with close(). Calls of a session that ended deliver nothing.
///
/// Incoming text messages are reassembled up to kMaxText bytes; a longer one ends the session (the
/// reason names the limit). Binary messages are handed in pieces, unbounded.
///
/// Sending: a send queues one whole message and never blocks the loop on the network; messages go
/// out whole and in the order they were queued (the ESP client copies them into a bounded send
/// queue that a task of its own drains; a write that times out there ends the session). A send
/// that returns false was not queued, nothing of it is sent: not connected, or the queue has no
/// room for it now (send_room()). want_writable() then asks for ws_client_writable() once the
/// queue has sent a message. A platform that sends synchronously (POSIX) never runs out of room.
///
/// Every call is made on the event loop's thread.
class WebSocketClient {
public:
	/// Longest incoming text message.
	static constexpr size_t kMaxText = 4096;
	/// Longest URL accepted, in bytes without the NUL.
	static constexpr size_t kMaxUrl = 255;

	/// Starts a session to `url` (`wss://host[:port]/path`, or `ws://` for development); an existing
	/// one is ended first, without a callback. False with `error` (static text) when it cannot start:
	/// the scheme is not supported by the platform (POSIX has no TLS), no memory, a URL the platform
	/// refuses.
	virtual bool connect(const char* url, WebSocketClientListener& listener, const char** error) = 0;

	/// Sends one text message (the platform may fragment it).
	virtual bool send_text(const char* text, size_t length) = 0;

	/// Sends one binary message.
	virtual bool send_binary(const uint8_t* data, size_t length) = 0;

	/// Sends a ping frame; ws_client_pong() reports the answer.
	virtual bool send_ping() = 0;

	/// The longest message a send takes now (0: none; SIZE_MAX: the platform sends synchronously).
	virtual size_t send_room() = 0;

	/// Asks for one ws_client_writable() after the queue has sent its next message (at once when it
	/// sent one since the last send_room() or refused send). Ignored without a session or a queue.
	virtual void want_writable() = 0;

	/// Ends the session with a close frame carrying `code` and `reason` (best effort) and drops the
	/// connection. No callback follows. Never blocks the loop for the closing handshake.
	virtual void close(uint16_t code, const char* reason) = 0;

protected:
	~WebSocketClient() = default;
};

} // namespace hc

#endif // HC_PLATFORM_WEBSOCKET_CLIENT_H
