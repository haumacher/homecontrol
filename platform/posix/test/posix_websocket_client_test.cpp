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

#ifdef HC_POSIX_TLS
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <cstdlib>
#endif

// The E-Home client of the POSIX build (doc/ehome.md) against an in-process server that speaks the
// core's codec (the server role) over a real TCP socket.

namespace {

namespace ws = hc::websocket;

#ifdef HC_POSIX_TLS
/// A self-signed server certificate for `ip` (its IP address, the CN "localhost"), its key, and the
/// certificate as a PEM file the client may trust (SSL_CERT_FILE).
struct TestCertificate {
	explicit TestCertificate(const char* ip) {
		key = EVP_EC_gen("P-256");
		REQUIRE(key != nullptr);
		cert = X509_new();
		ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
		X509_gmtime_adj(X509_getm_notBefore(cert), -60);
		X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
		X509_set_pubkey(cert, key);
		X509_NAME* name = X509_get_subject_name(cert);
		X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
		X509_set_issuer_name(cert, name);
		X509V3_CTX ctx;
		X509V3_set_ctx_nodb(&ctx);
		X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);
		std::string san = std::string("IP:") + ip + ",DNS:localhost";
		X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, NID_subject_alt_name, san.c_str());
		REQUIRE(ext != nullptr);
		X509_add_ext(cert, ext, -1);
		X509_EXTENSION_free(ext);
		REQUIRE(X509_sign(cert, key, EVP_sha256()) > 0);
		char path[] = "/tmp/hc-ws-ca-XXXXXX";
		int fd = ::mkstemp(path);
		REQUIRE(fd >= 0);
		std::FILE* file = ::fdopen(fd, "w");
		PEM_write_X509(file, cert);
		std::fclose(file);
		pem = path;
	}
	~TestCertificate() {
		::unlink(pem.c_str());
		X509_free(cert);
		EVP_PKEY_free(key);
	}
	EVP_PKEY* key = nullptr;
	X509* cert = nullptr;
	std::string pem;
};

/// Sets SSL_CERT_FILE (the client's trust store) for one scope.
struct TrustOnly {
	explicit TrustOnly(const std::string& pem) { ::setenv("SSL_CERT_FILE", pem.c_str(), 1); }
	~TrustOnly() { ::unsetenv("SSL_CERT_FILE"); }
};
#endif

/// A listening socket on 127.0.0.1 that accepts one connection, answers the upgrade and speaks
/// frames; with a certificate it speaks TLS (wss://).
class TestServer {
public:
#ifdef HC_POSIX_TLS
	explicit TestServer(const TestCertificate& certificate) : TestServer() {
		tls_context_ = SSL_CTX_new(TLS_server_method());
		REQUIRE(SSL_CTX_use_certificate(tls_context_, certificate.cert) == 1);
		REQUIRE(SSL_CTX_use_PrivateKey(tls_context_, certificate.key) == 1);
	}
#endif
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
#ifdef HC_POSIX_TLS
		if (tls_ != nullptr) {
			SSL_free(tls_);
		}
		if (tls_context_ != nullptr) {
			SSL_CTX_free(tls_context_);
		}
#endif
		if (fd_ >= 0) {
			::close(fd_);
		}
		::close(listen_fd_);
	}

	std::string url(const char* path) const {
		return std::string(secure() ? "wss" : "ws") + "://127.0.0.1:" + std::to_string(port) + path;
	}

	bool secure() const {
#ifdef HC_POSIX_TLS
		return tls_context_ != nullptr;
#else
		return false;
#endif
	}

	/// Accepts the connection (and the TLS handshake); false if the handshake failed (the client
	/// refused the certificate).
	bool accept_connection() {
		pollfd p{listen_fd_, POLLIN, 0};
		REQUIRE(::poll(&p, 1, 3000) == 1);
		fd_ = ::accept(listen_fd_, nullptr, nullptr);
		REQUIRE(fd_ >= 0);
#ifdef HC_POSIX_TLS
		if (tls_context_ != nullptr) {
			tls_ = SSL_new(tls_context_);
			SSL_set_fd(tls_, fd_);
			return SSL_accept(tls_) == 1;
		}
#endif
		return true;
	}

	/// Accepts and answers the upgrade; returns the request head.
	std::string accept_upgrade() {
		REQUIRE(accept_connection());
		std::string head;
		while (head.find("\r\n\r\n") == std::string::npos) {
			char chunk[512];
			ssize_t n = receive(chunk, sizeof(chunk));
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
		REQUIRE(transmit(answer.data(), answer.size()));
		return head;
	}

	void send(ws::Opcode opcode, const std::string& payload) {
		uint8_t header[ws::kMaxHeader];
		size_t n = ws::encode_header(header, opcode, true, payload.size());
		std::string frame(reinterpret_cast<char*>(header), n);
		frame += payload;
		REQUIRE(transmit(frame.data(), frame.size()));
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
			uint8_t chunk[4096];
			ssize_t n = receive(chunk, sizeof(chunk));
			if (n <= 0) {
				return false;
			}
			rx_.assign(chunk, chunk + n);
			offset_ = 0;
		}
	}

	uint16_t port = 0;

private:
	/// The next bytes within 3 s (-1 on a timeout or the end).
	ssize_t receive(void* buffer, size_t size) {
#ifdef HC_POSIX_TLS
		if (tls_ != nullptr && SSL_pending(tls_) > 0) {
			return SSL_read(tls_, buffer, static_cast<int>(size));
		}
#endif
		pollfd p{fd_, POLLIN, 0};
		if (::poll(&p, 1, 3000) != 1) {
			return -1;
		}
#ifdef HC_POSIX_TLS
		if (tls_ != nullptr) {
			return SSL_read(tls_, buffer, static_cast<int>(size));
		}
#endif
		return ::recv(fd_, buffer, size, 0);
	}

	bool transmit(const char* data, size_t size) {
#ifdef HC_POSIX_TLS
		if (tls_ != nullptr) {
			return SSL_write(tls_, data, static_cast<int>(size)) == static_cast<int>(size);
		}
#endif
		return ::send(fd_, data, size, MSG_NOSIGNAL) == static_cast<ssize_t>(size);
	}

#ifdef HC_POSIX_TLS
	SSL_CTX* tls_context_ = nullptr;
	SSL* tls_ = nullptr;
#endif
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


/// The exchange of the first test over `server`: upgrade, text both ways, binary, ping and pong, the
/// server's close.
void full_exchange(TestServer& server) {
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

} // namespace

TEST_CASE("PosixWebSocketClient: upgrade, text both ways, binary, ping and pong, the server's close") {
	TestServer server;
	full_exchange(server);
}

#ifdef HC_POSIX_TLS
TEST_CASE("PosixWebSocketClient: wss:// speaks TLS and checks the certificate against the trust store") {
	// The Linux package's E-Home connection (homecontrol #95): the same exchange over TLS, the
	// certificate trusted through SSL_CERT_FILE as a local CA would be through /etc/ssl/certs.
	CHECK(hc::posix::PosixWebSocketClient::tls_supported());
	TestCertificate certificate("127.0.0.1");
	TrustOnly trust(certificate.pem);
	TestServer server(certificate);
	full_exchange(server);
}

TEST_CASE("PosixWebSocketClient: wss:// refuses a server whose certificate is not trusted or not its own") {
	TestCertificate trusted("127.0.0.1");
	TestCertificate stranger("127.0.0.1");
	TestCertificate elsewhere("192.0.2.7"); // trusted, but issued for another address
	for (int round = 0; round < 2; ++round) {
		const TestCertificate& presented = round == 0 ? stranger : elsewhere;
		TrustOnly trust(round == 0 ? trusted.pem : elsewhere.pem);
		TestServer server(presented);
		std::thread script([&] { CHECK_FALSE(server.accept_connection()); });
		hc::posix::LoopExecutor executor;
		Recorder rec;
		hc::posix::PosixWebSocketClient client(executor);
		const char* error = nullptr;
		REQUIRE(client.connect(server.url("/socket").c_str(), rec, &error));
		REQUIRE(pump(executor, [&] { return !rec.reasons.empty(); }));
		script.join();
		CHECK(rec.opened == 0);
		INFO(rec.reasons[0]);
		CHECK(rec.reasons[0].find("cannot connect: TLS: the certificate of 127.0.0.1 is not trusted: ") == 0);
		CHECK(rec.reasons[0].find(round == 0 ? "self-signed certificate" : "IP address mismatch") != std::string::npos);
	}
}
#endif

TEST_CASE("PosixWebSocketClient: a refused connection is reported") {
	hc::posix::LoopExecutor executor;
	Recorder rec;
	hc::posix::PosixWebSocketClient client(executor);
	const char* error = nullptr;
	if (!hc::posix::PosixWebSocketClient::tls_supported()) {
		CHECK_FALSE(client.connect("wss://ehome.example/socket", rec, &error));
		CHECK(std::string(error).find("wss:// needs TLS") == 0);
	}
	CHECK_FALSE(client.connect("http://ehome.example/socket", rec, &error));
	CHECK(std::string(error) == "not a ws:// or wss:// URL");
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
