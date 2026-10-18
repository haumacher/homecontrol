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
/// Sending happens on the loop's thread; the platform may block for a bounded time (the ESP client
/// writes into the TLS socket with a timeout; a write that times out ends the session). A send
/// that returns false was not sent (not connected, the platform cannot take it now).
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

	/// Ends the session with a close frame carrying `code` and `reason` (best effort) and drops the
	/// connection. No callback follows. Never blocks the loop for the closing handshake.
	virtual void close(uint16_t code, const char* reason) = 0;

protected:
	~WebSocketClient() = default;
};

} // namespace hc

#endif // HC_PLATFORM_WEBSOCKET_CLIENT_H
