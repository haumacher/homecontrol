/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "doctest.h"
#include "hc/posix/httplib_fetcher.h"
#include "hc/posix/web_server.h"

// The POSIX HttpFetcher (issue #43) against an in-process HTTP server on a real TCP socket: the
// response reaches the receiver in pieces on the loop's thread, the receiver's "no" stops it, a
// short body is told from a complete one.

namespace {

/// Answers one request on 127.0.0.1 with a scripted response, then closes.
class OneShotServer {
public:
	explicit OneShotServer(std::string response) : response_(std::move(response)) {
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
		port_ = ntohs(addr.sin_port);
		thread_ = std::thread([this] { serve(); });
	}
	~OneShotServer() {
		thread_.join();
		::close(listen_fd_);
	}

	std::string url(const char* path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }

	/// The request as received (head and body); valid after the fetch ended.
	std::string request;

private:
	void serve() {
		pollfd p{listen_fd_, POLLIN, 0};
		if (::poll(&p, 1, 5000) != 1) {
			return;
		}
		int fd = ::accept(listen_fd_, nullptr, nullptr);
		if (fd < 0) {
			return;
		}
		// The head, then a body of Content-Length bytes.
		size_t expected = std::string::npos;
		for (;;) {
			size_t end = request.find("\r\n\r\n");
			if (end != std::string::npos && expected == std::string::npos) {
				size_t at = request.find("Content-Length: ");
				expected = end + 4 + (at != std::string::npos && at < end ? std::stoul(request.substr(at + 16)) : 0);
			}
			if (expected != std::string::npos && request.size() >= expected) {
				break;
			}
			char chunk[1024];
			pollfd q{fd, POLLIN, 0};
			if (::poll(&q, 1, 5000) != 1) {
				break;
			}
			ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
			if (n <= 0) {
				break;
			}
			request.append(chunk, static_cast<size_t>(n));
		}
		// The response in several writes, so the client reads it in pieces.
		for (size_t at = 0; at < response_.size(); at += 3000) {
			size_t n = response_.size() - at < 3000 ? response_.size() - at : 3000;
			if (::send(fd, response_.data() + at, n, MSG_NOSIGNAL) < 0) {
				break;
			}
		}
		::shutdown(fd, SHUT_WR);
		::close(fd);
	}

	std::string response_;
	int listen_fd_ = -1;
	uint16_t port_ = 0;
	std::thread thread_;
};

/// Records what arrives and on which thread; says no after `abort_after` pieces.
struct Recorder : hc::FetchReceiver {
	bool fetch_begin(int http_status, size_t content_length) override {
		on_loop = on_loop && std::this_thread::get_id() == loop_thread;
		status = http_status;
		announced = content_length;
		++begins;
		return true;
	}
	bool fetch_chunk(const char* data, size_t size) override {
		on_loop = on_loop && std::this_thread::get_id() == loop_thread;
		body.append(data, size);
		++pieces;
		return pieces < abort_after;
	}
	void fetch_end(const hc::FetchResult& r) override {
		on_loop = on_loop && std::this_thread::get_id() == loop_thread;
		result = r;
		error = r.error;
		ended = true;
	}

	std::thread::id loop_thread = std::this_thread::get_id();
	bool on_loop = true;
	int status = 0;
	size_t announced = 0;
	int begins = 0;
	std::string body;
	size_t pieces = 0;
	size_t abort_after = SIZE_MAX;
	hc::FetchResult result;
	std::string error;
	bool ended = false;
};

/// Runs the loop's jobs until the fetch ended.
void run(hc::posix::LoopExecutor& executor, const Recorder& recorder) {
	auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!recorder.ended && std::chrono::steady_clock::now() < until) {
		executor.wait(20);
	}
	REQUIRE(recorder.ended);
}

std::string pattern(size_t size) {
	std::string text(size, '\0');
	for (size_t i = 0; i < size; ++i) {
		text[i] = static_cast<char>('a' + i % 26);
	}
	return text;
}

} // namespace

TEST_CASE("HttplibFetcher: a 40 KB body arrives in pieces on the loop's thread, announced and chunked") {
	hc::posix::LoopExecutor executor;
	hc::posix::HttplibFetcher fetcher(executor);
	std::string body = pattern(40 * 1024);
	{
		OneShotServer server("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
			"\r\nConnection: close\r\n\r\n" + body);
		Recorder recorder;
		const char* error = nullptr;
		std::string url = server.url("/i18n/lang-de.json.gz");
		REQUIRE(fetcher.fetch(url.c_str(), recorder, &error));
		CHECK(fetcher.busy());
		CHECK_FALSE(fetcher.fetch(url.c_str(), recorder, &error));
		CHECK(std::string(error) == "busy: a fetch is in flight");
		run(executor, recorder);
		CHECK(recorder.on_loop);
		CHECK(recorder.begins == 1);
		CHECK(recorder.status == 200);
		CHECK(recorder.announced == body.size());
		CHECK(recorder.pieces > 1);
		CHECK(recorder.body == body);
		CHECK(recorder.result.ok);
		CHECK(recorder.result.complete);
		CHECK(recorder.result.length == body.size());
		CHECK(recorder.error.empty());
		CHECK_FALSE(fetcher.busy());
	}
	{
		std::string chunked;
		for (size_t at = 0; at < body.size(); at += 5000) {
			size_t n = body.size() - at < 5000 ? body.size() - at : 5000;
			char size[16];
			std::snprintf(size, sizeof(size), "%zx\r\n", n);
			chunked += size + body.substr(at, n) + "\r\n";
		}
		chunked += "0\r\n\r\n";
		OneShotServer server("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n" + chunked);
		Recorder recorder;
		const char* error = nullptr;
		REQUIRE(fetcher.fetch(server.url("/x").c_str(), recorder, &error));
		run(executor, recorder);
		CHECK(recorder.announced == hc::FetchReceiver::kUnknownLength);
		CHECK(recorder.body == body);
		CHECK(recorder.result.ok);
	}
}

TEST_CASE("HttplibFetcher: the receiver's no stops the transfer; a short body is not complete") {
	hc::posix::LoopExecutor executor;
	hc::posix::HttplibFetcher fetcher(executor);
	std::string body = pattern(40 * 1024);
	{
		OneShotServer server("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
			"\r\nConnection: close\r\n\r\n" + body);
		Recorder recorder;
		recorder.abort_after = 1;
		const char* error = nullptr;
		REQUIRE(fetcher.fetch(server.url("/x").c_str(), recorder, &error));
		run(executor, recorder);
		CHECK(recorder.pieces == 1);
		CHECK(recorder.result.aborted);
		CHECK_FALSE(recorder.result.ok);
		CHECK_FALSE(recorder.result.complete);
		CHECK(recorder.error == "aborted by the receiver");
	}
	{
		OneShotServer server("HTTP/1.1 200 OK\r\nContent-Length: 1000\r\nConnection: close\r\n\r\n" + pattern(500));
		Recorder recorder;
		const char* error = nullptr;
		REQUIRE(fetcher.fetch(server.url("/x").c_str(), recorder, &error));
		run(executor, recorder);
		CHECK_FALSE(recorder.result.ok);
		CHECK_FALSE(recorder.result.complete);
		CHECK_FALSE(recorder.error.empty());
		MESSAGE("short body: " << recorder.error);
	}
}

TEST_CASE("HttplibFetcher: a POST delivers the body of a refusal with its status") {
	hc::posix::LoopExecutor executor;
	hc::posix::HttplibFetcher fetcher(executor);
	std::string answer = "{\"error\":\"no such code\",\"code\":\"ehome.code_unknown\"}";
	OneShotServer server("HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: " +
		std::to_string(answer.size()) + "\r\nConnection: close\r\n\r\n" + answer);
	Recorder recorder;
	const char* error = nullptr;
	std::string sent = "{\"code\":\"WRONG\"}";
	REQUIRE(fetcher.post(server.url("/api/controllers/enroll").c_str(), sent.data(), sent.size(), recorder, &error));
	run(executor, recorder);
	CHECK(recorder.status == 404);
	CHECK(recorder.body == answer);
	CHECK(recorder.result.complete);
	CHECK_FALSE(recorder.result.ok);
	CHECK(recorder.error == "HTTP status 404");
	CHECK(server.request.find("POST /api/controllers/enroll") == 0);
	CHECK(server.request.find("\r\n\r\n" + sent) != std::string::npos);
}

TEST_CASE("HttplibFetcher: no server: only the end, with the reason") {
	hc::posix::LoopExecutor executor;
	hc::posix::HttplibFetcher fetcher(executor);
	// A port nobody listens on: bind one and close it again.
	int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
	socklen_t len = sizeof(addr);
	::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
	::close(fd);
	std::string url = "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port)) + "/x";
	Recorder recorder;
	const char* error = nullptr;
	REQUIRE(fetcher.fetch(url.c_str(), recorder, &error));
	run(executor, recorder);
	CHECK(recorder.begins == 0);
	CHECK(recorder.result.http_status == 0);
	CHECK_FALSE(recorder.result.ok);
	CHECK(recorder.error.find("cannot connect") == 0);
}
