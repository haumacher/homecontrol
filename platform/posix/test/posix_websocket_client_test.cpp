/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "doctest.h"
#include "hc/posix/websocket_client.h"
#include "hc/websocket.h"

// The E-Home client of the POSIX build (doc/ehome.md) against an in-process server that speaks the
// core's codec (the server role) over a real TCP socket.

namespace {

namespace ws = hc::websocket;

/// A listening socket on 127.0.0.1 that accepts one connection, answers the upgrade and speaks frames.
class TestServer {
public:
	TestServer() {
		listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
		REQUIRE(listen_fd_ >= 0);
		int one = 1;
		::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		REQUIRE(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
		REQUIRE(::listen(listen_fd_, 1) == 0);
		socklen_t len = sizeof(addr);
		::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
		port = ntohs(addr.sin_port);
	}
	~TestServer() {
		if (fd_ >= 0) {
			::close(fd_);
		}
		::close(listen_fd_);
	}

	std::string url(const char* path) const { return "ws://127.0.0.1:" + std::to_string(port) + path; }

	/// Accepts and answers the upgrade; returns the request head.
	std::string accept_upgrade() {
		pollfd p{listen_fd_, POLLIN, 0};
		REQUIRE(::poll(&p, 1, 3000) == 1);
		fd_ = ::accept(listen_fd_, nullptr, nullptr);
		REQUIRE(fd_ >= 0);
		std::string head;
		while (head.find("\r\n\r\n") == std::string::npos) {
			char chunk[512];
			pollfd q{fd_, POLLIN, 0};
			REQUIRE(::poll(&q, 1, 3000) == 1);
			ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
			REQUIRE(n > 0);
			head.append(chunk, static_cast<size_t>(n));
		}
		size_t at = head.find("Sec-WebSocket-Key: ");
		REQUIRE(at != std::string::npos);
		std::string key = head.substr(at + 19, head.find("\r\n", at) - at - 19);
		char accept[ws::kAcceptKeyLength + 1];
		REQUIRE(ws::accept_key(key.c_str(), accept));
		std::string answer = std::string("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
			"Sec-WebSocket-Accept: ") + accept + "\r\n\r\n";
		REQUIRE(::send(fd_, answer.data(), answer.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(answer.size()));
		return head;
	}

	void send(ws::Opcode opcode, const std::string& payload) {
		uint8_t header[ws::kMaxHeader];
		size_t n = ws::encode_header(header, opcode, true, payload.size());
		std::string frame(reinterpret_cast<char*>(header), n);
		frame += payload;
		REQUIRE(::send(fd_, frame.data(), frame.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(frame.size()));
	}

	/// The next message or control frame from the client: its opcode and payload.
	bool read(ws::Opcode& opcode, std::string& payload) {
		for (;;) {
			if (offset_ < rx_.size()) {
				size_t consumed = 0;
				ws::FrameDecoder::Event e = decoder_.push(rx_.data() + offset_, rx_.size() - offset_, consumed);
				offset_ += consumed;
				if (e == ws::FrameDecoder::Event::message || e == ws::FrameDecoder::Event::control) {
					opcode = decoder_.opcode();
					payload.assign(reinterpret_cast<const char*>(decoder_.data()), decoder_.length());
					return true;
				}
				if (e == ws::FrameDecoder::Event::error) {
					return false;
				}
			}
			pollfd p{fd_, POLLIN, 0};
			if (::poll(&p, 1, 3000) != 1) {
				return false;
			}
			uint8_t chunk[4096];
			ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
			if (n <= 0) {
				return false;
			}
			rx_.assign(chunk, chunk + n);
			offset_ = 0;
		}
	}

	uint16_t port = 0;

private:
	int listen_fd_ = -1;
	int fd_ = -1;
	std::vector<uint8_t> rx_;
	size_t offset_ = 0;
	uint8_t buffer_[8192];
	ws::FrameDecoder decoder_{buffer_, sizeof(buffer_)};
};

struct Recorder : hc::WebSocketClientListener {
	void ws_client_opened() override { ++opened; }
	void ws_client_text(const char* text, size_t length) override { texts.push_back(std::string(text, length)); }
	void ws_client_binary(const uint8_t* data, size_t length, bool) override {
		binary.append(reinterpret_cast<const char*>(data), length);
	}
	void ws_client_pong() override { ++pongs; }
	void ws_client_writable() override {}
	void ws_client_closed(const char* reason) override { reasons.push_back(reason); }
	int opened = 0;
	int pongs = 0;
	std::vector<std::string> texts;
	std::string binary;
	std::vector<std::string> reasons;
};

bool pump(hc::posix::LoopExecutor& executor, const std::function<bool()>& done, int timeout_ms = 5000) {
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (!done()) {
		if (std::chrono::steady_clock::now() > deadline) {
			return false;
		}
		executor.wait(10);
	}
	return true;
}

} // namespace

TEST_CASE("PosixWebSocketClient: upgrade, text both ways, binary, ping and pong, the server's close") {
	TestServer server;
	std::atomic<bool> server_ok{false};
	std::string head;
	std::string client_text;
	std::thread script([&] {
		head = server.accept_upgrade();
		server.send(ws::Opcode::text, "{\"type\":\"challenge\",\"nonce\":\"00\"}");
		ws::Opcode op;
		std::string payload;
		REQUIRE(server.read(op, payload));
		CHECK(op == ws::Opcode::text);
		client_text = payload;
		REQUIRE(server.read(op, payload));
		CHECK(op == ws::Opcode::ping);
		server.send(ws::Opcode::pong, "");
		server.send(ws::Opcode::binary, std::string(5000, 'b'));
		server.send(ws::Opcode::ping, "x");
		REQUIRE(server.read(op, payload));
		CHECK(op == ws::Opcode::pong);
		CHECK(payload == "x");
		uint8_t close[ws::kMaxControlPayload];
		size_t n = ws::encode_close(close, ws::kClosePolicy, "refused");
		server.send(ws::Opcode::close, std::string(reinterpret_cast<char*>(close), n));
		server_ok = true;
	});
	hc::posix::LoopExecutor executor;
	Recorder rec;
	{
		hc::posix::PosixWebSocketClient client(executor);
		const char* error = nullptr;
		REQUIRE(client.connect(server.url("/api/controllers/c-17/socket").c_str(), rec, &error));
		REQUIRE(pump(executor, [&] { return rec.texts.size() == 1; }));
		CHECK(rec.opened == 1);
		CHECK(rec.texts[0] == "{\"type\":\"challenge\",\"nonce\":\"00\"}");
		REQUIRE(client.send_text("{\"type\":\"auth\"}", 15));
		REQUIRE(client.send_ping());
		REQUIRE(pump(executor, [&] { return !rec.reasons.empty(); }));
		CHECK(rec.pongs == 1);
		CHECK(rec.binary == std::string(5000, 'b'));
		CHECK(rec.reasons[0] == "closed by the server (1008)");
		script.join();
		CHECK(server_ok);
		CHECK(client_text == "{\"type\":\"auth\"}");
		CHECK(head.find("GET /api/controllers/c-17/socket HTTP/1.1\r\n") == 0);
		CHECK_FALSE(client.send_text("x", 1)); // the session ended
	}
}

TEST_CASE("PosixWebSocketClient: wss:// is refused with the reason, a refused connection is reported") {
	hc::posix::LoopExecutor executor;
	Recorder rec;
	hc::posix::PosixWebSocketClient client(executor);
	const char* error = nullptr;
	CHECK_FALSE(client.connect("wss://ehome.example/socket", rec, &error));
	CHECK(std::string(error).find("wss:// needs TLS") == 0);
	int port = 0;
	{
		TestServer closed; // bound and closed again: nobody listens on its port
		port = closed.port;
	}
	REQUIRE(client.connect(("ws://127.0.0.1:" + std::to_string(port) + "/s").c_str(), rec, &error));
	REQUIRE(pump(executor, [&] { return !rec.reasons.empty(); }));
	CHECK(rec.reasons[0].find("cannot connect: ") == 0);
	CHECK(rec.opened == 0);
}
