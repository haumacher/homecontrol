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
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <random>
#include <string>

#include "hc/websocket.h"

#ifdef HC_POSIX_TLS
#include <arpa/inet.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

namespace hc {
namespace posix {

namespace {

/// The parts of `ws[s]://host[:port][/path]`.
struct WsUrl {
	bool tls = false;
	std::string host;
	std::string port = "80";
	std::string host_header;
	std::string path = "/";
};

bool parse_url(const char* url, WsUrl& out, const char** error) {
	size_t scheme = 0;
	if (std::strncmp(url, "wss://", 6) == 0) {
		if (!PosixWebSocketClient::tls_supported()) {
			*error = "wss:// needs TLS, which this POSIX build does not have (install the OpenSSL development files "
				"and reconfigure, or use ws:// for development)";
			return false;
		}
		out.tls = true;
		out.port = "443";
		scheme = 6;
	} else if (std::strncmp(url, "ws://", 5) == 0) {
		scheme = 5;
	} else {
		*error = "not a ws:// or wss:// URL";
		return false;
	}
	std::string rest(url + scheme);
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
	if (out.host.size() > 2 && out.host.front() == '[' && out.host.back() == ']') {
		out.host = out.host.substr(1, out.host.size() - 2); // [::1]
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
#ifdef HC_POSIX_TLS
	/// The TLS session over `fd` (wss://, nullptr for ws://); used under `mutex` only: the reader and
	/// the loop's writes share it. The socket is non-blocking then.
	SSL_CTX* tls_context = nullptr;
	SSL* tls = nullptr;
#endif

	/// Writes all of `data`; the caller holds `mutex` (or is the reader before the session opened).
	bool write_all(const uint8_t* data, size_t total) {
		size_t sent = 0;
		while (sent < total) {
#ifdef HC_POSIX_TLS
			if (tls != nullptr) {
				int n = SSL_write(tls, data + sent, static_cast<int>(total - sent));
				if (n > 0) {
					sent += static_cast<size_t>(n);
					continue;
				}
				int err = SSL_get_error(tls, n);
				short events = err == SSL_ERROR_WANT_READ ? POLLIN : err == SSL_ERROR_WANT_WRITE ? POLLOUT : 0;
				pollfd p{fd, events, 0};
				if (events == 0 || ::poll(&p, 1, PosixWebSocketClient::kSendTimeoutS * 1000) != 1) {
					return false;
				}
				continue;
			}
#endif
			ssize_t n = ::send(fd, data + sent, total - sent, MSG_NOSIGNAL);
			if (n < 0 && errno == EINTR) {
				continue;
			}
			if (n <= 0) {
				return false;
			}
			sent += static_cast<size_t>(n);
		}
		return true;
	}

	/// Reads what arrives within `timeout_ms`: the byte count, 0 if nothing came (a timeout, a
	/// signal, a TLS record not complete yet), -1 when the connection ended (`reason` says how).
	/// Called by the reader thread only.
	ssize_t read_some(uint8_t* buffer, size_t size, int timeout_ms, std::string& reason) {
#ifdef HC_POSIX_TLS
		if (tls != nullptr) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (SSL_pending(tls) > 0) {
					return tls_read(buffer, size, reason);
				}
			}
			pollfd p{fd, POLLIN, 0};
			int r = ::poll(&p, 1, timeout_ms);
			if (r <= 0) {
				return r < 0 && errno != EINTR ? (reason = std::string("poll: ") + std::strerror(errno), -1) : 0;
			}
			std::lock_guard<std::mutex> lock(mutex);
			return tls_read(buffer, size, reason);
		}
#endif
		pollfd p{fd, POLLIN, 0};
		int r = ::poll(&p, 1, timeout_ms);
		if (r <= 0) {
			return r < 0 && errno != EINTR ? (reason = std::string("poll: ") + std::strerror(errno), -1) : 0;
		}
		ssize_t got = ::recv(fd, buffer, size, 0);
		if (got < 0 && errno == EINTR) {
			return 0;
		}
		if (got <= 0) {
			reason = got == 0 ? "connection closed by the server" : std::string("recv: ") + std::strerror(errno);
			return -1;
		}
		return got;
	}

#ifdef HC_POSIX_TLS
	/// SSL_read under `mutex`.
	ssize_t tls_read(uint8_t* buffer, size_t size, std::string& reason) {
		int n = SSL_read(tls, buffer, static_cast<int>(size));
		if (n > 0) {
			return n;
		}
		int err = SSL_get_error(tls, n);
		if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
			return 0;
		}
		reason = err == SSL_ERROR_ZERO_RETURN || err == SSL_ERROR_SYSCALL ? std::string("connection closed by the server")
			: "TLS: " + tls_error();
		return -1;
	}

	static std::string tls_error() {
		unsigned long code = ERR_get_error();
		char text[160] = "unknown error";
		if (code != 0) {
			ERR_error_string_n(code, text, sizeof(text));
		}
		ERR_clear_error();
		return text;
	}

	/// The TLS handshake on the connected socket `fd` (made non-blocking): the system's trust store
	/// (OpenSSL's default paths, which honour SSL_CERT_FILE and SSL_CERT_DIR), the server name for
	/// SNI and the certificate check. False with `reason` filled.
	bool tls_connect(std::string& reason) {
		tls_context = SSL_CTX_new(TLS_client_method());
		if (tls_context == nullptr) {
			reason = "TLS: " + tls_error();
			return false;
		}
		SSL_CTX_set_min_proto_version(tls_context, TLS1_2_VERSION);
		SSL_CTX_set_verify(tls_context, SSL_VERIFY_PEER, nullptr);
		if (SSL_CTX_set_default_verify_paths(tls_context) != 1) {
			reason = "TLS: no trust store: " + tls_error();
			return false;
		}
		tls = SSL_new(tls_context);
		if (tls == nullptr) {
			reason = "TLS: " + tls_error();
			return false;
		}
		SSL_set_mode(tls, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
		unsigned char ip[16];
		bool numeric = inet_pton(AF_INET, url.host.c_str(), ip) == 1 || inet_pton(AF_INET6, url.host.c_str(), ip) == 1;
		X509_VERIFY_PARAM* param = SSL_get0_param(tls);
		if (numeric) {
			X509_VERIFY_PARAM_set1_ip_asc(param, url.host.c_str());
		} else {
			SSL_set_tlsext_host_name(tls, url.host.c_str());
			X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
			X509_VERIFY_PARAM_set1_host(param, url.host.c_str(), 0);
		}
		int flags = ::fcntl(fd, F_GETFL, 0);
		::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
		SSL_set_fd(tls, fd);
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(PosixWebSocketClient::kConnectTimeoutS);
		for (;;) {
			int r = SSL_connect(tls);
			if (r == 1) {
				return true;
			}
			int err = SSL_get_error(tls, r);
			if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
				long verify = SSL_get_verify_result(tls);
				reason = verify != X509_V_OK
					? std::string("TLS: the certificate of ") + url.host + " is not trusted: " +
						X509_verify_cert_error_string(verify)
					: "TLS handshake with " + url.host + " failed: " + tls_error();
				ERR_clear_error();
				return false;
			}
			auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
			pollfd p{fd, static_cast<short>(err == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT), 0};
			if (left.count() <= 0 || ::poll(&p, 1, static_cast<int>(left.count())) == 0) {
				reason = "TLS handshake with " + url.host + ": no answer";
				return false;
			}
		}
	}
#endif

	/// Closes the socket (and the TLS session); the caller holds `mutex`.
	void close_locked() {
#ifdef HC_POSIX_TLS
		if (tls != nullptr) {
			SSL_free(tls);
			tls = nullptr;
		}
		if (tls_context != nullptr) {
			SSL_CTX_free(tls_context);
			tls_context = nullptr;
		}
#endif
		if (fd >= 0) {
			::close(fd);
		}
		fd = -1;
	}

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
		if (!write_all(frame.data(), header + length)) {
			::shutdown(fd, SHUT_RDWR);
			return false;
		}
		return true;
	}
};

bool PosixWebSocketClient::tls_supported() {
#ifdef HC_POSIX_TLS
	return true;
#else
	return false;
#endif
}

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
#ifdef HC_POSIX_TLS
	if (fd >= 0 && s->url.tls && !s->tls_connect(reason)) {
		fd = -1; // closed below with the session's socket
	}
#endif
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
		bool sent;
		{
			std::lock_guard<std::mutex> lock(s->mutex);
			sent = s->write_all(reinterpret_cast<const uint8_t*>(request.data()), request.size());
		}
		if (!sent) {
			reason = "cannot send the upgrade request";
		}
		std::string head;
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kConnectTimeoutS);
		while (reason.empty()) {
			auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
			if (left.count() <= 0) {
				reason = "no answer to the upgrade request";
				break;
			}
			uint8_t chunk[1024];
			std::string ended;
			ssize_t got = s->read_some(chunk, sizeof(chunk), static_cast<int>(left.count()), ended);
			if (got < 0) {
				reason = ended.rfind("connection closed", 0) == 0 ? "the server closed the connection during the upgrade"
					: ended;
				break;
			}
			if (got == 0) {
				continue;
			}
			head.append(reinterpret_cast<const char*>(chunk), static_cast<size_t>(got));
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
			s->close_locked();
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
			ssize_t got = s->read_some(chunk, sizeof(chunk), 200, reason);
			if (got < 0) {
				break;
			}
			if (got == 0) {
				std::lock_guard<std::mutex> lock(s->mutex);
				if (s->closed) {
					break;
				}
				continue;
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
		s->close_locked();
	}
	if (!ours) {
		deliver(shared, generation, [&reason](WebSocketClientListener& l) { l.ws_client_closed(reason.c_str()); });
	}
	s->done.store(true);
}

} // namespace posix
} // namespace hc
