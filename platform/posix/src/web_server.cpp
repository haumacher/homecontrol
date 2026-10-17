/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/posix/web_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "hc/image/image.h"
#include "hc/log.h"
#include "hc/sha.h"
#include "hc/update_check.h"
#include "hc/websocket.h"
#include "httplib.h"

namespace hc {
namespace posix {

namespace {

constexpr const char* kTag = "hc.web";

/// Longest WebSocket message from a client.
constexpr size_t kMaxIncoming = 4096;

/// Response buffer of the API (the ESP build uses the same size).
constexpr size_t kResponseBuffer = 8192;

} // namespace

// ---- Crypto -----------------------------------------------------------------------------------

bool PosixCrypto::random(uint8_t* out, size_t length) {
	size_t done = 0;
	while (done < length) {
		ssize_t n = getrandom(out + done, length - done, 0);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			Log::error(kTag, "getrandom failed: %s", std::strerror(errno));
			return false;
		}
		done += static_cast<size_t>(n);
	}
	return true;
}

void PosixCrypto::pbkdf2_sha256(const uint8_t* password, size_t password_length, const uint8_t* salt,
	size_t salt_length, uint32_t iterations, uint8_t* out, size_t out_length) {
	hc::pbkdf2_sha256(password, password_length, salt, salt_length, iterations, out, out_length);
}

// ---- Executor ---------------------------------------------------------------------------------

bool LoopExecutor::run(const std::function<void()>& fn) {
	Job job{&fn};
	std::unique_lock<std::mutex> lock(mutex_);
	if (stopped_) {
		return false;
	}
	jobs_.push_back(&job);
	wake_.notify_one();
	signal_wake();
	done_.wait(lock, [&] { return job.done || stopped_; });
	return job.done;
}

LoopExecutor::LoopExecutor() {
	if (pipe2(wake_pipe_, O_NONBLOCK | O_CLOEXEC) != 0) {
		wake_pipe_[0] = wake_pipe_[1] = -1;
		hc::Log::error("hc.web", "cannot create the executor's wake pipe: %s; a poll()ing main loop will not wake for jobs",
			std::strerror(errno));
	}
}

LoopExecutor::~LoopExecutor() {
	for (int& fd : wake_pipe_) {
		if (fd >= 0) {
			::close(fd);
			fd = -1;
		}
	}
}

void LoopExecutor::signal_wake() {
	if (wake_pipe_[1] >= 0) {
		uint8_t byte = 1;
		ssize_t n = ::write(wake_pipe_[1], &byte, 1); // a full pipe already wakes the reader
		(void) n;
	}
}

void LoopExecutor::run_jobs(std::unique_lock<std::mutex>& lock) {
	if (wake_pipe_[0] >= 0) {
		uint8_t drain[16];
		while (::read(wake_pipe_[0], drain, sizeof(drain)) > 0) {
		}
	}
	while (!jobs_.empty()) {
		Job* job = jobs_.front();
		jobs_.pop_front();
		lock.unlock();
		(*job->fn)();
		lock.lock();
		job->done = true;
		done_.notify_all();
	}
}

void LoopExecutor::wait(uint64_t ms) {
	std::unique_lock<std::mutex> lock(mutex_);
	if (jobs_.empty() && !stopped_) {
		wake_.wait_for(lock, std::chrono::milliseconds(ms));
	}
	run_jobs(lock);
}

void LoopExecutor::run_pending() {
	std::unique_lock<std::mutex> lock(mutex_);
	run_jobs(lock);
}

void LoopExecutor::stop() {
	std::lock_guard<std::mutex> lock(mutex_);
	stopped_ = true;
	wake_.notify_all();
	signal_wake();
	done_.notify_all();
}

// ---- Server -----------------------------------------------------------------------------------

struct WebServer::Connection {
	uint32_t id = 0;
	int fd = -1;
	std::mutex send_mutex;
	std::atomic<bool> closing{false};
};

struct WebServer::Impl {
	httplib::Server http;
	std::atomic<int> threads{0};
};

namespace {

const char* content_type_of(const std::string& path) {
	size_t dot = path.rfind('.');
	std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
	if (ext == "html") return "text/html; charset=utf-8";
	if (ext == "css") return "text/css; charset=utf-8";
	if (ext == "js" || ext == "mjs") return "text/javascript; charset=utf-8";
	if (ext == "json") return "application/json";
	if (ext == "svg") return "image/svg+xml";
	if (ext == "png") return "image/png";
	if (ext == "ico") return "image/x-icon";
	if (ext == "webmanifest") return "application/manifest+json";
	if (ext == "woff2") return "font/woff2";
	if (ext == "txt") return "text/plain; charset=utf-8";
	return "application/octet-stream";
}

bool read_file(const std::string& path, std::string& out) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return false;
	}
	std::ostringstream content;
	content << in.rdbuf();
	out = content.str();
	return true;
}

HttpMethod method_of(const std::string& method) {
	if (method == "GET") return HttpMethod::get;
	if (method == "POST") return HttpMethod::post;
	if (method == "PUT") return HttpMethod::put;
	if (method == "DELETE") return HttpMethod::del;
	return HttpMethod::other;
}

void json_error(httplib::Response& res, int status, const char* code, const char* text) {
	char body[512];
	JsonWriter json(body, sizeof(body));
	json.begin_object().member("error", text).member("code", code).end_object();
	res.status = status;
	res.set_content(body, json.length(), "application/json");
}

/// Writes everything or fails.
bool send_all(int fd, const void* data, size_t length) {
	const uint8_t* p = static_cast<const uint8_t*>(data);
	while (length > 0) {
		ssize_t n = ::send(fd, p, length, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		p += n;
		length -= static_cast<size_t>(n);
	}
	return true;
}

} // namespace

WebServer::WebServer(Controller& controller, DatapointModel& model, LoopExecutor& executor, uint16_t port,
	std::string www_dir)
	: controller_(controller),
	  executor_(executor),
	  auth_(controller.settings(), controller.clock(), crypto_),
	  api_(controller, model, auth_, *this),
	  feed_(api_, controller.loop(), *this),
	  port_(port),
	  www_dir_(std::move(www_dir)),
	  impl_(new Impl) {
	char host[48] = "host";
	gethostname(host, sizeof(host) - 1);
	std::snprintf(device_name_, sizeof(device_name_), "%s", host);
	std::snprintf(ui_version_, sizeof(ui_version_), "dev");
}

WebServer::~WebServer() {
	stop();
}

void WebServer::ntp_server_changed() {
	Log::info(kTag, "ntp.server stored; the POSIX build takes the host's clock and does not use it");
}

bool WebServer::set_update_url(const char* url, const char** error) {
	if (url[0] == '\0') {
		SettingsStatus status = controller_.settings().erase(setting::kUpdateUrl);
		if (status != SettingsStatus::ok && status != SettingsStatus::not_found) {
			*error = settings_status_name(status);
			return false;
		}
		return true;
	}
	if (!update_url_valid(url, error)) {
		return false;
	}
	SettingsStatus status = controller_.settings().set_string(setting::kUpdateUrl, url);
	if (status != SettingsStatus::ok) {
		*error = settings_status_name(status);
		return false;
	}
	Log::info(kTag, "update.url stored (the POSIX build does not update itself)");
	return true;
}

void WebServer::write_update_state(JsonWriter& json) const {
	char url[Settings::kMaxStringLength + 1] = {};
	controller_.settings().get_string(setting::kUpdateUrl, url, sizeof(url));
	json.member("available", false).member("source", url).member("last", "the POSIX build does not update itself");
}

bool WebServer::start() {
	auth_.start();
	if (!feed_.start()) {
		return false;
	}
	httplib::Server& http = impl_->http;
	// The largest body any route takes (PUT /api/image); each route's own limit is checked below.
	http.set_payload_max_length(image::kMaxImageBytes);

	auto api_handler = [this](const httplib::Request& req, httplib::Response& res) {
		size_t limit = WebApi::body_limit(method_of(req.method), req.path.c_str());
		if (req.body.size() > limit) {
			std::string text = "the request body is longer than " + std::to_string(limit) + " bytes";
			json_error(res, 413, "api.body_too_large", text.c_str());
			return;
		}
		std::string query;
		for (const auto& p : req.params) {
			if (!query.empty()) {
				query += '&';
			}
			query += p.first + "=" + p.second;
		}
		std::string cookie = req.get_header_value("Cookie");
		std::string authorization = req.get_header_value("Authorization");
		std::string requested_with = req.get_header_value("X-Requested-With");
		WebRequest request;
		request.method = method_of(req.method);
		request.path = req.path.c_str();
		request.query = query.c_str();
		request.cookie = req.has_header("Cookie") ? cookie.c_str() : nullptr;
		request.authorization = req.has_header("Authorization") ? authorization.c_str() : nullptr;
		request.requested_with = req.has_header("X-Requested-With") ? requested_with.c_str() : nullptr;
		request.source = req.remote_addr.c_str();
		request.body = req.body.data();
		request.body_length = req.body.size();
		std::unique_ptr<char[]> buffer(new char[kResponseBuffer]);
		WebResponse response(buffer.get(), kResponseBuffer);
		if (!executor_.run([&] { api_.handle(request, response); })) {
			json_error(res, 503, "api.stopping", "the controller is stopping");
			return;
		}
		PasswordHash hash;
		if (response.needs_hash) {
			hash.params = response.hash_params;
			auto start = std::chrono::steady_clock::now();
			crypto_.pbkdf2_sha256(reinterpret_cast<const uint8_t*>(response.password), response.password_length,
				hash.params.salt, sizeof(hash.params.salt), hash.params.iterations, hash.key, sizeof(hash.key));
			auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
			std::memset(response.password, 0, sizeof(response.password));
			request.hash = &hash;
			WebResponse second(buffer.get(), kResponseBuffer);
			if (!executor_.run([&] {
					Log::info(kTag, "PBKDF2-HMAC-SHA256, %u iterations: %lld ms", static_cast<unsigned>(hash.params.iterations),
						static_cast<long long>(ms));
					api_.handle(request, second);
				})) {
				json_error(res, 503, "api.stopping", "the controller is stopping");
				return;
			}
			response.status = second.status;
			response.length = second.length;
			response.content_type = second.content_type;
			response.retry_after_s = second.retry_after_s;
			std::memcpy(response.set_cookie, second.set_cookie, sizeof(response.set_cookie));
		}
		res.status = response.status;
		if (response.set_cookie[0] != '\0') {
			res.set_header("Set-Cookie", response.set_cookie);
		}
		if (response.retry_after_s > 0) {
			res.set_header("Retry-After", std::to_string(response.retry_after_s));
		}
		res.set_header("Cache-Control", "no-store");
		if (response.stream) {
			// A stored document (GET /api/image): fetched piecewise on the loop, as the ESP build does.
			std::string body;
			body.reserve(response.stream_length);
			bool ok = true;
			while (ok && body.size() < response.stream_length) {
				size_t length = 0;
				if (!executor_.run([&] {
						ok = api_.read_stream(response.stream_token, body.size(), buffer.get(), kResponseBuffer, length);
					}) || length == 0) {
					ok = false;
				}
				body.append(buffer.get(), ok ? length : 0);
			}
			if (!ok) {
				json_error(res, 409, "image.changed", "the stored image changed while it was read; repeat the request");
				return;
			}
			res.set_content(body, response.content_type);
			return;
		}
		res.set_content(buffer.get(), response.length, response.content_type);
	};
	http.Get(R"(/api/.*)", api_handler);
	http.Post(R"(/api/.*)", api_handler);
	http.Put(R"(/api/.*)", api_handler);
	http.Delete(R"(/api/.*)", api_handler);
	http.Get("/ws", [this](const httplib::Request&, httplib::Response& res) {
		char text[96];
		std::snprintf(text, sizeof(text), "the POSIX build serves the WebSocket on port %u",
			static_cast<unsigned>(websocket_port()));
		json_error(res, 426, "ws.other_port", text);
	});
	http.Get(R"(/.*)", [this](const httplib::Request& req, httplib::Response& res) {
		std::string path = req.path;
		if (path.find("..") != std::string::npos || path.find('\\') != std::string::npos) {
			res.status = 400;
			res.set_content("bad path\n", "text/plain");
			return;
		}
		if (path.empty() || path.back() == '/') {
			path += "index.html";
		}
		std::string content;
		std::string file = www_dir_ + path;
		if (!read_file(file, content)) {
			size_t slash = path.rfind('/');
			bool has_extension = path.find('.', slash == std::string::npos ? 0 : slash) != std::string::npos;
			if (has_extension || !read_file(www_dir_ + "/index.html", content)) {
				res.status = 404;
				res.set_content("not found: " + req.path + "\n", "text/plain; charset=utf-8");
				return;
			}
			path = "/index.html"; // single-page app: the UI routes extensionless paths itself
		}
		uint8_t digest[Sha256::kDigestSize];
		Sha256::hash(content.data(), content.size(), digest);
		char etag[24];
		etag[0] = '"';
		to_hex(digest, 8, etag + 1);
		etag[17] = '"';
		etag[18] = '\0';
		res.set_header("ETag", etag);
		res.set_header("Cache-Control", "no-cache");
		if (req.get_header_value("If-None-Match") == etag) {
			res.status = 304;
			return;
		}
		res.set_content(content, content_type_of(path));
	});

	if (!http.bind_to_port("0.0.0.0", port_)) {
		Log::error(kTag, "web server not started: cannot bind port %u", static_cast<unsigned>(port_));
		return false;
	}

	ws_listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(ws_listener_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(websocket_port());
	if (ws_listener_ < 0 || ::bind(ws_listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
		::listen(ws_listener_, 8) != 0) {
		Log::error(kTag, "WebSocket listener not started: cannot bind port %u (%s)",
			static_cast<unsigned>(websocket_port()), std::strerror(errno));
		if (ws_listener_ >= 0) {
			::close(ws_listener_);
			ws_listener_ = -1;
		}
		http.stop();
		return false;
	}
	http_thread_ = std::thread([this] { http_thread(); });
	ws_thread_ = std::thread([this] { ws_accept_thread(); });
	Log::info(kTag, "web UI at http://localhost:%u/ (files from %s), WebSocket on port %u", static_cast<unsigned>(port_),
		www_dir_.c_str(), static_cast<unsigned>(websocket_port()));
	return true;
}

void WebServer::http_thread() {
	impl_->http.listen_after_bind();
}

void WebServer::stop() {
	if (stopping_.exchange(true)) {
		return;
	}
	impl_->http.stop();
	if (http_thread_.joinable()) {
		http_thread_.join();
	}
	if (ws_listener_ >= 0) {
		::shutdown(ws_listener_, SHUT_RDWR);
		::close(ws_listener_);
	}
	if (ws_thread_.joinable()) {
		ws_thread_.join();
	}
	{
		std::lock_guard<std::mutex> lock(connections_mutex_);
		for (auto& entry : connections_) {
			::shutdown(entry.second->fd, SHUT_RDWR);
		}
	}
	for (int i = 0; i < 200 && impl_->threads.load() > 0; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

std::shared_ptr<WebServer::Connection> WebServer::find(uint32_t id) {
	std::lock_guard<std::mutex> lock(connections_mutex_);
	auto it = connections_.find(id);
	return it == connections_.end() ? nullptr : it->second;
}

bool WebServer::send_frame(Connection& connection, uint8_t opcode, const uint8_t* payload, size_t length) {
	uint8_t header[websocket::kMaxHeader];
	size_t n = websocket::encode_header(header, static_cast<websocket::Opcode>(opcode), true, length);
	std::lock_guard<std::mutex> lock(connection.send_mutex);
	return send_all(connection.fd, header, n) && send_all(connection.fd, payload, length);
}

bool WebServer::ws_send(uint32_t client, const char* text, size_t length) {
	std::shared_ptr<Connection> connection = find(client);
	if (connection == nullptr || connection->closing.load()) {
		return false;
	}
	return send_frame(*connection, static_cast<uint8_t>(websocket::Opcode::text), reinterpret_cast<const uint8_t*>(text),
		length);
}

void WebServer::ws_close(uint32_t client, uint16_t code, const char* reason) {
	std::shared_ptr<Connection> connection = find(client);
	if (connection == nullptr || connection->closing.exchange(true)) {
		return;
	}
	uint8_t payload[websocket::kMaxControlPayload];
	size_t n = websocket::encode_close(payload, code, reason);
	send_frame(*connection, static_cast<uint8_t>(websocket::Opcode::close), payload, n);
	::shutdown(connection->fd, SHUT_RDWR); // the connection thread reports the close
}

void WebServer::ws_accept_thread() {
	while (!stopping_.load()) {
		sockaddr_in peer = {};
		socklen_t peer_length = sizeof(peer);
		int fd = ::accept(ws_listener_, reinterpret_cast<sockaddr*>(&peer), &peer_length);
		if (fd < 0) {
			if (stopping_.load()) {
				break;
			}
			if (errno == EINTR) {
				continue;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}
		timeval timeout = {0, 500000}; // a slow client must not stall the event loop's sends for long
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
		int one = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		auto connection = std::make_shared<Connection>();
		connection->fd = fd;
		{
			std::lock_guard<std::mutex> lock(connections_mutex_);
			connection->id = next_id_++;
			connections_[connection->id] = connection;
		}
		impl_->threads++;
		std::thread([this, connection] {
			ws_connection_thread(connection);
			impl_->threads--;
		}).detach();
	}
}

namespace {

/// Reads the HTTP request head (up to the empty line); false on EOF, overflow or timeout.
bool read_head(int fd, std::string& head, std::string& rest) {
	char buffer[1024];
	while (head.size() < 8192) {
		pollfd p = {fd, POLLIN, 0};
		if (::poll(&p, 1, 5000) <= 0) {
			return false;
		}
		ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
		if (n <= 0) {
			return false;
		}
		head.append(buffer, static_cast<size_t>(n));
		size_t end = head.find("\r\n\r\n");
		if (end != std::string::npos) {
			rest = head.substr(end + 4);
			head.resize(end + 2);
			return true;
		}
	}
	return false;
}

std::string header_value(const std::string& head, const char* name) {
	size_t pos = head.find("\r\n");
	size_t name_length = std::strlen(name);
	while (pos != std::string::npos && pos + 2 < head.size()) {
		size_t start = pos + 2;
		size_t end = head.find("\r\n", start);
		if (end == std::string::npos) {
			break;
		}
		if (end - start > name_length && head[start + name_length] == ':' &&
			strncasecmp(head.c_str() + start, name, name_length) == 0) {
			size_t v = start + name_length + 1;
			while (v < end && head[v] == ' ') {
				++v;
			}
			return head.substr(v, end - v);
		}
		pos = end;
	}
	return std::string();
}

bool contains_token(std::string value, const char* token) {
	for (char& c : value) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return value.find(token) != std::string::npos;
}

void http_refuse(int fd, int status, const char* reason, const char* code, const char* text) {
	char body[256];
	JsonWriter json(body, sizeof(body));
	json.begin_object().member("error", text).member("code", code).end_object();
	char response[512];
	int n = std::snprintf(response, sizeof(response),
		"HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s", status,
		reason, static_cast<unsigned>(json.length()), body);
	send_all(fd, response, static_cast<size_t>(n));
}

} // namespace

void WebServer::ws_connection_thread(std::shared_ptr<Connection> connection) {
	int fd = connection->fd;
	uint32_t id = connection->id;
	auto forget = [&] {
		std::lock_guard<std::mutex> lock(connections_mutex_);
		connections_.erase(id);
	};
	std::string head;
	std::string rest;
	if (!read_head(fd, head, rest)) {
		forget();
		::close(fd);
		return;
	}
	std::string key = header_value(head, "Sec-WebSocket-Key");
	char accept[websocket::kAcceptKeyLength + 1];
	bool upgrade = head.compare(0, 8, "GET /ws ") == 0 || head.compare(0, 8, "GET /ws?") == 0;
	if (!upgrade || !contains_token(header_value(head, "Upgrade"), "websocket") ||
		header_value(head, "Sec-WebSocket-Version") != "13" || !websocket::accept_key(key.c_str(), accept)) {
		http_refuse(fd, 400, "Bad Request", "ws.bad_handshake",
			"this port speaks only WebSocket at /ws (version 13, with Sec-WebSocket-Key)");
		forget();
		::close(fd);
		return;
	}
	char response[256];
	int n = std::snprintf(response, sizeof(response),
		"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",
		accept);
	if (!send_all(fd, response, static_cast<size_t>(n))) {
		forget();
		::close(fd);
		return;
	}
	std::string cookie = header_value(head, "Cookie");
	std::string origin = header_value(head, "Origin");
	std::string host = header_value(head, "Host");
	bool accepted = false;
	executor_.run([&] {
		accepted = feed_.open(id, cookie.empty() ? nullptr : cookie.c_str(), origin.empty() ? nullptr : origin.c_str(),
			host.empty() ? nullptr : host.c_str());
	});

	std::unique_ptr<uint8_t[]> message(new uint8_t[kMaxIncoming]);
	websocket::FrameDecoder decoder(message.get(), kMaxIncoming);
	std::string pending = rest;
	uint8_t buffer[2048];
	bool open = accepted;
	while (open && !stopping_.load()) {
		size_t offset = 0;
		while (offset < pending.size() && open) {
			size_t consumed = 0;
			websocket::FrameDecoder::Event event = decoder.push(
				reinterpret_cast<const uint8_t*>(pending.data()) + offset, pending.size() - offset, consumed);
			offset += consumed;
			switch (event) {
			case websocket::FrameDecoder::Event::need_more:
				break;
			case websocket::FrameDecoder::Event::message: {
				if (decoder.opcode() != websocket::Opcode::text) {
					ws_close(id, websocket::kCloseUnsupported, "only text messages are understood");
					open = false;
					break;
				}
				std::string text(reinterpret_cast<const char*>(decoder.data()), decoder.length());
				executor_.run([&] { feed_.message(id, text.data(), text.size()); });
				break;
			}
			case websocket::FrameDecoder::Event::control:
				if (decoder.opcode() == websocket::Opcode::ping) {
					send_frame(*connection, static_cast<uint8_t>(websocket::Opcode::pong), decoder.data(), decoder.length());
				} else if (decoder.opcode() == websocket::Opcode::close) {
					if (!connection->closing.exchange(true)) {
						send_frame(*connection, static_cast<uint8_t>(websocket::Opcode::close), decoder.data(),
							decoder.length() >= 2 ? 2 : 0);
					}
					open = false;
				}
				break;
			case websocket::FrameDecoder::Event::error:
				Log::warn(kTag, "WebSocket client %u: %s", static_cast<unsigned>(id), decoder.error());
				ws_close(id, decoder.close_code(), decoder.error());
				open = false;
				break;
			}
		}
		pending.clear();
		if (!open) {
			break;
		}
		ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
		if (got <= 0) {
			break;
		}
		pending.assign(reinterpret_cast<const char*>(buffer), static_cast<size_t>(got));
	}
	connection->closing.store(true);
	forget();
	executor_.run([&] { feed_.closed(id); });
	{
		std::lock_guard<std::mutex> lock(connection->send_mutex);
		::close(fd);
	}
}

} // namespace posix
} // namespace hc
