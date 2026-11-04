/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_POSIX_WEBSOCKET_CLIENT_H
#define HC_POSIX_WEBSOCKET_CLIENT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "hc/platform/websocket_client.h"
#include "hc/posix/web_server.h"

namespace hc {
namespace posix {

/// The POSIX WebSocketClient (doc/ehome.md): the client side of RFC 6455 over a TCP socket, on the
/// core's codec (hc/websocket.h: masked frames out, the client role of the FrameDecoder in).
/// Plain `ws://` only: `wss://` is refused with that reason (no TLS on the host build; the device
/// speaks TLS through esp_websocket_client). Enough to run the controller against an E-Home on
/// the developer's machine.
///
/// A reader thread per session connects, makes the upgrade handshake, reads and answers pings;
/// it hands every event to the event loop through the LoopExecutor (blocking until the loop ran
/// it). Writes happen on the loop thread under a mutex with a send timeout of kSendTimeoutS (a
/// write that times out ends the session).
class PosixWebSocketClient : public WebSocketClient {
public:
	static constexpr int kConnectTimeoutS = 10;
	static constexpr int kSendTimeoutS = 5;
	/// Largest binary message accepted (the runtime image's limit and some room).
	static constexpr size_t kMaxBinary = 300 * 1024;

	explicit PosixWebSocketClient(LoopExecutor& executor);
	/// Ends the session and waits for the reader thread; call on the loop thread.
	~PosixWebSocketClient();

	PosixWebSocketClient(const PosixWebSocketClient&) = delete;
	PosixWebSocketClient& operator=(const PosixWebSocketClient&) = delete;

	bool connect(const char* url, WebSocketClientListener& listener, const char** error) override;
	bool send_text(const char* text, size_t length) override;
	bool send_binary(const uint8_t* data, size_t length) override;
	bool send_ping() override;
	/// Writes are synchronous (the kernel's socket buffer is the queue): never short of room.
	size_t send_room() override { return SIZE_MAX; }
	void want_writable() override {}
	/// The claim is a buffer of the loop thread; the commit sends it synchronously.
	char* claim_text(size_t max) override {
		claim_.resize(max);
		return claim_.data();
	}
	void commit_text(size_t length) override { send_text(claim_.data(), length); }
	void abandon_text() override {}
	void close(uint16_t code, const char* reason) override;

	// Implementation: the state shared with the reader thread (websocket_client.cpp).
	struct Session;
	struct Shared;

private:
	static void reader(std::shared_ptr<Session> session, std::shared_ptr<Shared> shared);
	bool send_frame(uint8_t opcode, const uint8_t* data, size_t length);
	void end_session(uint16_t code, const char* reason);
	void reap(bool all);

	LoopExecutor& executor_;
	std::vector<char> claim_;
	std::shared_ptr<Shared> shared_;
	std::shared_ptr<Session> session_;
	struct Worker {
		std::thread thread;
		std::shared_ptr<Session> session;
	};
	std::vector<Worker> workers_;
};

} // namespace posix
} // namespace hc

#endif // HC_POSIX_WEBSOCKET_CLIENT_H
