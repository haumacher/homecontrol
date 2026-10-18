/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/posix/websocket_client.h"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <random>
#include <string>

#include "hc/websocket.h"

namespace hc {
namespace posix {

namespace {

/// The parts of `ws://host[:port][/path]`.
struct WsUrl {
	std::string host;
	std::string port = "80";
	std::string host_header;
	std::string path = "/";
};

bool parse_url(const char* url, WsUrl& out, const char** error) {
	if (std::strncmp(url, "wss://", 6) == 0) {
		*error = "wss:// needs TLS, which the POSIX WebSocket client does not have (use ws:// for development; "
			"the device speaks TLS)";
		return false;
	}
	if (std::strncmp(url, "ws://", 5) != 0) {
		*error = "not a ws:// URL";
		return false;
	}
	std::string rest(url + 5);
	size_t slash = rest.find('/');
	std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
	if (slash != std::string::npos) {
		out.path = rest.substr(slash);
	}
	if (authority.empty()) {
		*error = "the URL has no host";
		return false;
	}
	out.host_header = authority;
	size_t colon = authority.rfind(':');
	if (colon != std::string::npos && authority.find(']') == std::string::npos) {
		out.host = authority.substr(0, colon);
		out.port = authority.substr(colon + 1);
	} else {
		out.host = authority;
	}
	return true;
}

std::string base64(const uint8_t* data, size_t length) {
	static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	for (size_t i = 0; i < length; i += 3) {
		uint32_t v = static_cast<uint32_t>(data[i]) << 16;
		if (i + 1 < length) {
			v |= static_cast<uint32_t>(data[i + 1]) << 8;
		}
		if (i + 2 < length) {
			v |= data[i + 2];
		}
		out += kAlphabet[(v >> 18) & 63];
		out += kAlphabet[(v >> 12) & 63];
		out += i + 1 < length ? kAlphabet[(v >> 6) & 63] : '=';
		out += i + 2 < length ? kAlphabet[v & 63] : '=';
	}
	return out;
}

/// A TCP connection to host:port within the connect timeout; -1 with `error` filled.
int open_socket(const WsUrl& url, std::string& error) {
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo* list = nullptr;
	int rc = ::getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &list);
	if (rc != 0) {
		error = "cannot resolve " + url.host + ": " + gai_strerror(rc);
		return -1;
	}
	int fd = -1;
	error = "no address";
	for (addrinfo* a = list; a != nullptr; a = a->ai_next) {
		fd = ::socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
		if (fd < 0) {
			error = std::string("socket: ") + std::strerror(errno);
			continue;
		}
		int flags = ::fcntl(fd, F_GETFL, 0);
		::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
		int r = ::connect(fd, a->ai_addr, a->ai_addrlen);
		if (r != 0 && errno == EINPROGRESS) {
			pollfd p{fd, POLLOUT, 0};
			r = ::poll(&p, 1, PosixWebSocketClient::kConnectTimeoutS * 1000);
			if (r == 1) {
				int so_error = 0;
				socklen_t len = sizeof(so_error);
				::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
				r = so_error == 0 ? 0 : -1;
				errno = so_error;
			} else {
				errno = r == 0 ? ETIMEDOUT : errno;
				r = -1;
			}
		}
		if (r == 0) {
			::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
			timeval tv{PosixWebSocketClient::kSendTimeoutS, 0};
			::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
			int one = 1;
			::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
			error.clear();
			break;
		}
		error = "cannot connect to " + url.host + ":" + url.port + ": " + std::strerror(errno);
		::close(fd);
		fd = -1;
	}
	::freeaddrinfo(list);
	return fd;
}

} // namespace

struct PosixWebSocketClient::Shared {
	LoopExecutor* executor = nullptr;
	WebSocketClientListener* listener = nullptr;
	uint32_t generation = 0;
	bool alive = true;
};

struct PosixWebSocketClient::Session {
	uint32_t generation = 0;
	WsUrl url;
	std::mutex mutex; ///< Guards fd, closed, open and the socket's write side.
	int fd = -1;
	bool closed = false; ///< Ended by the client: no callback.
	bool open = false;   ///< The handshake completed.
	std::atomic<bool> done{false};
	std::mt19937 masks{std::random_device{}()};

	/// Sends one masked frame; the caller holds `mutex`. False (and the socket shut down) on failure.
	bool send_locked(uint8_t opcode, const uint8_t* data, size_t length) {
		if (fd < 0 || closed) {
			return false;
		}
		uint8_t mask[4];
		uint32_t m = masks();
		std::memcpy(mask, &m, sizeof(mask));
		std::vector<uint8_t> frame(websocket::kMaxHeader + length);
		size_t header = websocket::encode_header(frame.data(), static_cast<websocket::Opcode>(opcode), true, length, mask);
		if (length > 0) {
			std::memcpy(frame.data() + header, data, length);
			websocket::apply_mask(frame.data() + header, length, mask);
		}
		size_t total = header + length;
		size_t sent = 0;
		while (sent < total) {
			ssize_t n = ::send(fd, frame.data() + sent, total - sent, MSG_NOSIGNAL);
			if (n < 0 && errno == EINTR) {
				continue;
			}
			if (n <= 0) {
				::shutdown(fd, SHUT_RDWR);
				return false;
			}
			sent += static_cast<size_t>(n);
		}
		return true;
	}
};

PosixWebSocketClient::PosixWebSocketClient(LoopExecutor& executor)
	: executor_(executor), shared_(std::make_shared<Shared>()) {
	shared_->executor = &executor;
}

PosixWebSocketClient::~PosixWebSocketClient() {
	shared_->alive = false;
	end_session(websocket::kCloseGoingAway, "the controller stops");
	reap(true);
}

void PosixWebSocketClient::reap(bool all) {
	for (size_t i = 0; i < workers_.size();) {
		Worker& w = workers_[i];
		if (!all && !w.session->done.load()) {
			++i;
			continue;
		}
		while (!w.session->done.load()) {
			executor_.wait(10); // the reader may wait for the loop to run its last event
		}
		w.thread.join();
		workers_.erase(workers_.begin() + static_cast<long>(i));
	}
}

void PosixWebSocketClient::end_session(uint16_t code, const char* reason) {
	if (!session_) {
		return;
	}
	std::shared_ptr<Session> s = session_;
	session_.reset();
	++shared_->generation;
	std::lock_guard<std::mutex> lock(s->mutex);
	if (s->open && s->fd >= 0) {
		uint8_t payload[websocket::kMaxControlPayload];
		size_t n = websocket::encode_close(payload, code, reason);
		s->send_locked(static_cast<uint8_t>(websocket::Opcode::close), payload, n);
	}
	s->closed = true;
	s->open = false;
	if (s->fd >= 0) {
		::shutdown(s->fd, SHUT_RDWR);
	}
}

bool PosixWebSocketClient::connect(const char* url, WebSocketClientListener& listener, const char** error) {
	end_session(websocket::kCloseNormal, "reconnecting");
	reap(false);
	auto s = std::make_shared<Session>();
	if (std::strlen(url) > kMaxUrl || !parse_url(url, s->url, error)) {
		if (std::strlen(url) > kMaxUrl) {
			*error = "URL longer than 255 bytes";
		}
		return false;
	}
	s->generation = ++shared_->generation;
	shared_->listener = &listener;
	session_ = s;
	workers_.push_back(Worker{std::thread(&PosixWebSocketClient::reader, s, shared_), s});
	return true;
}

bool PosixWebSocketClient::send_frame(uint8_t opcode, const uint8_t* data, size_t length) {
	if (!session_) {
		return false;
	}
	std::lock_guard<std::mutex> lock(session_->mutex);
	return session_->open && session_->send_locked(opcode, data, length);
}

bool PosixWebSocketClient::send_text(const char* text, size_t length) {
	return send_frame(static_cast<uint8_t>(websocket::Opcode::text), reinterpret_cast<const uint8_t*>(text), length);
}

bool PosixWebSocketClient::send_binary(const uint8_t* data, size_t length) {
	return send_frame(static_cast<uint8_t>(websocket::Opcode::binary), data, length);
}

bool PosixWebSocketClient::send_ping() {
	return send_frame(static_cast<uint8_t>(websocket::Opcode::ping), nullptr, 0);
}

void PosixWebSocketClient::close(uint16_t code, const char* reason) { end_session(code, reason); }

// ---- The reader thread -------------------------------------------------------------------------

namespace {

void deliver(const std::shared_ptr<PosixWebSocketClient::Shared>& shared, uint32_t generation,
	const std::function<void(WebSocketClientListener&)>& fn) {
	shared->executor->run([&shared, generation, &fn] {
		if (shared->alive && shared->generation == generation && shared->listener != nullptr) {
			fn(*shared->listener);
		}
	});
}

} // namespace

void PosixWebSocketClient::reader(std::shared_ptr<Session> s, std::shared_ptr<Shared> shared) {
	uint32_t generation = s->generation;
	std::string reason;
	int fd = open_socket(s->url, reason);
	{
		std::lock_guard<std::mutex> lock(s->mutex);
		if (s->closed) {
			if (fd >= 0) {
				::close(fd);
			}
			s->done.store(true);
			return;
		}
		s->fd = fd;
	}
	std::vector<uint8_t> rx;
	if (fd >= 0) {
		// The upgrade (RFC 6455 section 4.1).
		uint8_t nonce[16];
		for (uint8_t& b : nonce) {
			b = static_cast<uint8_t>(s->masks());
		}
		std::string key = base64(nonce, sizeof(nonce));
		std::string request = "GET " + s->url.path + " HTTP/1.1\r\nHost: " + s->url.host_header +
			"\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
			"\r\nSec-WebSocket-Version: 13\r\nUser-Agent: HomeControl\r\n\r\n";
		if (::send(fd, request.data(), request.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(request.size())) {
			reason = "cannot send the upgrade request";
		}
		std::string head;
		while (reason.empty()) {
			pollfd p{fd, POLLIN, 0};
			int r = ::poll(&p, 1, kConnectTimeoutS * 1000);
			if (r <= 0) {
				reason = r == 0 ? "no answer to the upgrade request" : std::string("poll: ") + std::strerror(errno);
				break;
			}
			char chunk[1024];
			ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
			if (got <= 0) {
				reason = "the server closed the connection during the upgrade";
				break;
			}
			head.append(chunk, static_cast<size_t>(got));
			size_t end = head.find("\r\n\r\n");
			if (end == std::string::npos) {
				if (head.size() > 8192) {
					reason = "the upgrade answer is too long";
				}
				continue;
			}
			rx.assign(head.begin() + static_cast<long>(end + 4), head.end()); // frames after the head
			head.resize(end + 2);
			break;
		}
		if (reason.empty()) {
			char expected[websocket::kAcceptKeyLength + 1];
			websocket::accept_key(key.c_str(), expected);
			std::string lower;
			for (char c : head) {
				lower += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
			}
			if (head.compare(0, 12, "HTTP/1.1 101") != 0) {
				size_t eol = head.find("\r\n");
				reason = "the server refused the upgrade: " + head.substr(0, eol);
			} else {
				size_t at = lower.find("\r\nsec-websocket-accept:");
				std::string accept;
				if (at != std::string::npos) {
					size_t value = head.find_first_not_of(' ', at + 23);
					size_t eol = head.find("\r\n", value);
					accept = head.substr(value, eol - value);
					while (!accept.empty() && accept.back() == ' ') {
						accept.pop_back();
					}
				}
				if (accept != expected) {
					reason = "the server's Sec-WebSocket-Accept is wrong";
				}
			}
		}
	}
	if (!reason.empty() || fd < 0) {
		bool ours;
		{
			std::lock_guard<std::mutex> lock(s->mutex);
			ours = s->closed;
			if (s->fd >= 0) {
				::close(s->fd);
			}
			s->fd = -1;
		}
		if (!ours) {
			std::string text = "cannot connect: " + reason;
			deliver(shared, generation, [&text](WebSocketClientListener& l) { l.ws_client_closed(text.c_str()); });
		}
		s->done.store(true);
		return;
	}
	{
		std::lock_guard<std::mutex> lock(s->mutex);
		s->open = !s->closed;
	}
	deliver(shared, generation, [](WebSocketClientListener& l) { l.ws_client_opened(); });

	std::vector<uint8_t> message(kMaxBinary);
	websocket::FrameDecoder decoder(message.data(), message.size(), websocket::FrameDecoder::Role::client);
	uint8_t chunk[4096];
	size_t offset = 0;
	while (reason.empty()) {
		if (offset >= rx.size()) {
			rx.clear();
			offset = 0;
			pollfd p{fd, POLLIN, 0};
			int r = ::poll(&p, 1, 200);
			if (r < 0 && errno == EINTR) {
				continue;
			}
			if (r < 0) {
				reason = std::string("poll: ") + std::strerror(errno);
				break;
			}
			if (r == 0) {
				std::lock_guard<std::mutex> lock(s->mutex);
				if (s->closed) {
					break;
				}
				continue;
			}
			ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
			if (got < 0 && errno == EINTR) {
				continue;
			}
			if (got <= 0) {
				reason = got == 0 ? "connection closed by the server" : std::string("recv: ") + std::strerror(errno);
				break;
			}
			rx.assign(chunk, chunk + got);
		}
		size_t consumed = 0;
		websocket::FrameDecoder::Event event = decoder.push(rx.data() + offset, rx.size() - offset, consumed);
		offset += consumed;
		switch (event) {
			case websocket::FrameDecoder::Event::need_more:
				break;
			case websocket::FrameDecoder::Event::error:
				reason = std::string("protocol error: ") + decoder.error();
				break;
			case websocket::FrameDecoder::Event::message:
				if (decoder.opcode() == websocket::Opcode::text) {
					if (decoder.length() > kMaxText) {
						reason = "a text message longer than 4096 bytes";
						break;
					}
					const char* text = reinterpret_cast<const char*>(decoder.data());
					size_t length = decoder.length();
					deliver(shared, generation,
						[text, length](WebSocketClientListener& l) { l.ws_client_text(text, length); });
				} else {
					const uint8_t* data = decoder.data();
					size_t length = decoder.length();
					deliver(shared, generation,
						[data, length](WebSocketClientListener& l) { l.ws_client_binary(data, length, true); });
				}
				break;
			case websocket::FrameDecoder::Event::control:
				if (decoder.opcode() == websocket::Opcode::ping) {
					std::lock_guard<std::mutex> lock(s->mutex);
					s->send_locked(static_cast<uint8_t>(websocket::Opcode::pong), decoder.data(), decoder.length());
				} else if (decoder.opcode() == websocket::Opcode::pong) {
					deliver(shared, generation, [](WebSocketClientListener& l) { l.ws_client_pong(); });
				} else {
					uint16_t code = decoder.length() >= 2
						? static_cast<uint16_t>((decoder.data()[0] << 8) | decoder.data()[1]) : websocket::kCloseNoStatus;
					char text[48];
					std::snprintf(text, sizeof(text), "closed by the server (%u)", static_cast<unsigned>(code));
					reason = text;
					std::lock_guard<std::mutex> lock(s->mutex);
					uint8_t payload[2] = {static_cast<uint8_t>(code >> 8), static_cast<uint8_t>(code & 0xFF)};
					s->send_locked(static_cast<uint8_t>(websocket::Opcode::close), payload, sizeof(payload));
				}
				break;
		}
	}
	bool ours;
	{
		std::lock_guard<std::mutex> lock(s->mutex);
		ours = s->closed;
		s->open = false;
		::close(s->fd);
		s->fd = -1;
	}
	if (!ours) {
		deliver(shared, generation, [&reason](WebSocketClientListener& l) { l.ws_client_closed(reason.c_str()); });
	}
	s->done.store(true);
}

} // namespace posix
} // namespace hc
