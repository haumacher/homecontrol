/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "web_server.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "hc/log.h"
#include "hc/network_settings.h"
#include "hc/update_check.h"
#include "hc/websocket.h"

namespace hc {
namespace esp {

namespace {

constexpr const char* kTag = "hc.web";

/// The one server instance (close_fn and the work callbacks carry no context of ours).
WebServer* g_server = nullptr;

/// Chunk size for the UI files (one TCP segment).
constexpr size_t kFileChunk = 1436;

/// Longest header values copied for the core. Longer ones are replaced by a value the core refuses
/// (a cut-off token or origin must never pass as a shorter, valid one).
constexpr size_t kMaxAuthorization = 96;
constexpr size_t kMaxRequestedWith = 32;
constexpr size_t kMaxOrigin = 96;
constexpr size_t kMaxPath = CONFIG_HTTPD_MAX_URI_LEN + 1;

void log_heap(const char* what) {
	Log::info(kTag, "%s: free heap %u B (min %u B, largest block %u B)", what,
		static_cast<unsigned>(esp_get_free_heap_size()), static_cast<unsigned>(esp_get_minimum_free_heap_size()),
		static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

const char* status_line(int status) {
	switch (status) {
		case 200: return "200 OK";
		case 201: return "201 Created";
		case 204: return "204 No Content";
		case 304: return "304 Not Modified";
		case 400: return "400 Bad Request";
		case 401: return "401 Unauthorized";
		case 403: return "403 Forbidden";
		case 404: return "404 Not Found";
		case 405: return "405 Method Not Allowed";
		case 409: return "409 Conflict";
		case 413: return "413 Content Too Large";
		case 415: return "415 Unsupported Media Type";
		case 422: return "422 Unprocessable Content";
		case 429: return "429 Too Many Requests";
		case 500: return "500 Internal Server Error";
		case 503: return "503 Service Unavailable";
		default: return status >= 500 ? "500 Internal Server Error" : "400 Bad Request";
	}
}

const char* content_type_of(const char* path) {
	const char* dot = std::strrchr(path, '.');
	if (dot == nullptr) return "application/octet-stream";
	const char* ext = dot + 1;
	if (std::strcmp(ext, "html") == 0) return "text/html; charset=utf-8";
	if (std::strcmp(ext, "css") == 0) return "text/css; charset=utf-8";
	if (std::strcmp(ext, "js") == 0 || std::strcmp(ext, "mjs") == 0) return "text/javascript; charset=utf-8";
	if (std::strcmp(ext, "json") == 0) return "application/json";
	if (std::strcmp(ext, "svg") == 0) return "image/svg+xml";
	if (std::strcmp(ext, "png") == 0) return "image/png";
	if (std::strcmp(ext, "ico") == 0) return "image/x-icon";
	if (std::strcmp(ext, "webmanifest") == 0) return "application/manifest+json";
	if (std::strcmp(ext, "woff2") == 0) return "font/woff2";
	if (std::strcmp(ext, "txt") == 0) return "text/plain; charset=utf-8";
	return "application/octet-stream";
}

/// Sends a JSON refusal {"error", "code"} (httpd task).
esp_err_t send_refusal(httpd_req_t* req, int status, const char* code, const char* text) {
	static char body[256];
	JsonWriter json(body, sizeof(body));
	json.begin_object().member("error", text).member("code", code).end_object();
	httpd_resp_set_status(req, status_line(status));
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	return httpd_resp_send(req, json.data(), static_cast<ssize_t>(json.length()));
}

/// Copies header `name` into `out`; false if absent. A value longer than the buffer is replaced
/// by `overlong` (a value the core refuses).
bool copy_header(httpd_req_t* req, const char* name, char* out, size_t size, const char* overlong) {
	size_t length = httpd_req_get_hdr_value_len(req, name);
	if (length == 0) {
		return false;
	}
	if (length >= size) {
		std::snprintf(out, size, "%s", overlong);
		return true;
	}
	return httpd_req_get_hdr_value_str(req, name, out, size) == ESP_OK;
}

/// The session cookie of the request as a Cookie header the core parses ("hc_session=<id>"); false
/// if the request has none.
bool copy_session_cookie(httpd_req_t* req, char* out, size_t size) {
	size_t prefix = std::strlen(Auth::kCookieName) + 1;
	if (size <= prefix + 1) {
		return false;
	}
	std::snprintf(out, size, "%s=", Auth::kCookieName);
	size_t length = size - prefix;
	esp_err_t err = httpd_req_get_cookie_val(req, Auth::kCookieName, out + prefix, &length);
	if (err == ESP_ERR_HTTPD_RESULT_TRUNC) {
		std::snprintf(out + prefix, size - prefix, "invalid");
		return true;
	}
	return err == ESP_OK;
}

/// The peer's address as text (IPv4-mapped IPv6 addresses as IPv4).
void peer_address(int fd, char* out, size_t size) {
	std::snprintf(out, size, "?");
	sockaddr_storage address;
	socklen_t length = sizeof(address);
	if (getpeername(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
		return;
	}
	if (address.ss_family == AF_INET) {
		const sockaddr_in* v4 = reinterpret_cast<const sockaddr_in*>(&address);
		inet_ntop(AF_INET, &v4->sin_addr, out, size);
	}
#if CONFIG_LWIP_IPV6
	else if (address.ss_family == AF_INET6) {
		const sockaddr_in6* v6 = reinterpret_cast<const sockaddr_in6*>(&address);
		const uint8_t* b = reinterpret_cast<const uint8_t*>(&v6->sin6_addr);
		static const uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
		if (std::memcmp(b, kMapped, sizeof(kMapped)) == 0) {
			inet_ntop(AF_INET, b + 12, out, size);
		} else {
			inet_ntop(AF_INET6, &v6->sin6_addr, out, size);
		}
	}
#endif
}

HttpMethod method_of(int method) {
	switch (method) {
		case HTTP_GET: return HttpMethod::get;
		case HTTP_POST: return HttpMethod::post;
		case HTTP_PUT: return HttpMethod::put;
		case HTTP_DELETE: return HttpMethod::del;
		default: return HttpMethod::other;
	}
}

uint32_t make_handle(int fd, uint16_t generation) {
	return (static_cast<uint32_t>(generation) << 16) | (static_cast<uint32_t>(fd) & 0xffffu);
}

int handle_fd(uint32_t handle) {
	return static_cast<int>(handle & 0xffffu);
}

uint16_t handle_generation(uint32_t handle) {
	return static_cast<uint16_t>(handle >> 16);
}

} // namespace

// ---- Crypto -----------------------------------------------------------------------------------

bool EspCrypto::random(uint8_t* out, size_t length) {
	// The hardware RNG; with WiFi running (or the bootloader's entropy at start) it is a
	// cryptographically secure source (ESP-IDF "Random Number Generation").
	esp_fill_random(out, length);
	return true;
}

void EspCrypto::pbkdf2_sha256(const uint8_t* password, size_t password_length, const uint8_t* salt,
	size_t salt_length, uint32_t iterations, uint8_t* out, size_t out_length) {
	int err = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, password, password_length, salt, salt_length,
		iterations, static_cast<uint32_t>(out_length), out);
	if (err != 0) {
		// Cannot happen with valid arguments; a zero key never matches a stored one.
		Log::error(kTag, "PBKDF2 failed (mbedTLS -0x%04x)", static_cast<unsigned>(-err));
		std::memset(out, 0, out_length);
	}
}

// ---- The job slot (httpd task -> event loop) ---------------------------------------------------

struct WebServer::Job {
	enum class Kind : uint8_t { api, ws_open, ws_message };

	Kind kind = Kind::api;
	uint32_t sequence = 0;
	std::atomic<uint32_t> done{0};

	// api
	WebRequest request;
	WebResponse response{nullptr, 0};
	PasswordHash hash;
	int64_t hash_us = 0;
	char path[kMaxPath] = {};
	char cookie[48] = {};
	char authorization[kMaxAuthorization] = {};
	char requested_with[kMaxRequestedWith] = {};
	char source[Auth::kMaxSource] = {};
	char body[WebApi::kMaxBody + 1] = {};

	// ws_open, ws_message
	uint32_t handle = 0;
	bool has_cookie = false;
	bool has_origin = false;
	bool has_host = false;
	char origin[kMaxOrigin] = {};
	char host[kMaxOrigin] = {};
	char text[kMaxIncoming + 1] = {};
	size_t text_length = 0;
};

struct WebServer::SendBuffer {
	uint32_t handle = 0;
	bool close = false;
	uint16_t length = 0;
	char data[LiveFeed::kMessageSize] = {};
};

namespace {

/// Static storage: the job, the API's response body, the file chunk and the send pool live in
/// .bss, not on the heap (doc/core.md: no heap in steady state).
char g_response[WebServer::kResponseBuffer];
char g_file_chunk[kFileChunk];

} // namespace

// ---- Server -----------------------------------------------------------------------------------

WebServer::WebServer(Controller& controller, DatapointModel& model, LoopQueue& queue, NetServices& services,
	OtaService& ota, const char* device_name)
	: controller_(controller),
	  model_(model),
	  queue_(queue),
	  services_(services),
	  ota_(ota),
	  device_name_(device_name),
	  auth_(controller.settings(), controller.clock(), crypto_),
	  api_(controller, model, auth_, *this),
	  feed_(api_, controller.loop(), *this),
	  demo_(model, controller.loop(), controller.clock()),
	  console_command_{"web", "status | token create <name> | token list | token revoke <name> | password clear | demo",
		  "Web server (doc/web.md): state, API tokens for machines (shown once, stored hashed), reset the device "
		  "password (onboarding opens again), load the demo datapoints until the next restart",
		  &WebServer::console_web, this} {
	static Job job;
	job_ = &job;
	static SendBuffer buffers[kSendBuffers];
	buffers_ = buffers;
}

bool WebServer::mdns_announced() const {
	return services_.mdns_running();
}

const char* WebServer::ntp_server() const {
	return services_.sntp_running() ? services_.ntp_server() : "";
}

void WebServer::ntp_server_changed() {
	services_.restart_sntp();
}

bool WebServer::set_update_url(const char* url, const char** error) {
	return ota_.set_url(url, error);
}

void WebServer::write_update_state(JsonWriter& json) const {
	const esp_partition_t* running = esp_ota_get_running_partition();
	json.member("available", true).member("partition", running != nullptr ? running->label : "?");
	json.member("confirmed", ota_.confirmed()).member("checking", ota_.checking());
	char url[Settings::kMaxStringLength + 1] = {};
	bool has_url = controller_.settings().get_string(setting::kUpdateUrl, url, sizeof(url)) == SettingsStatus::ok &&
		url[0] != '\0';
	json.member("source", has_url ? url : "");
	json.member("last", ota_.last_result());
	uint64_t now = controller_.clock().monotonic_ms();
	if (ota_.last_result_ms() != 0) {
		json.member("last_ago_s", (now - ota_.last_result_ms()) / 1000);
	}
	if (has_url) {
		uint64_t next = ota_.next_check_ms();
		json.member("next_check_s", next > now ? (next - now) / 1000 : 0);
	}
}

bool WebServer::mount_ui() {
	esp_vfs_littlefs_conf_t conf = {};
	conf.base_path = kMountPoint;
	conf.partition_label = kPartitionLabel;
	// Never format: an unreadable partition means the UI image was not flashed (or is damaged);
	// the server says so on every page instead of hiding it behind an empty file system.
	conf.format_if_mount_failed = false;
	conf.dont_mount = false;
	// Nothing writes the partition yet: read-only keeps the image intact whatever happens.
	conf.read_only = true;
	esp_err_t err = esp_vfs_littlefs_register(&conf);
	if (err != ESP_OK) {
		Log::error(kTag, "web UI not available: cannot mount the storage partition (%s); flash storage.bin "
			"(doc/web.md)", esp_err_to_name(err));
		return false;
	}
	char path[48];
	std::snprintf(path, sizeof(path), "%s/VERSION", kUiDir);
	FILE* file = std::fopen(path, "r");
	if (file == nullptr) {
		Log::error(kTag, "web UI not available: the storage partition has no UI image (%s missing)", path);
		return false;
	}
	size_t n = std::fread(ui_version_, 1, sizeof(ui_version_) - 1, file);
	std::fclose(file);
	while (n > 0 && (ui_version_[n - 1] == '\n' || ui_version_[n - 1] == '\r' || ui_version_[n - 1] == ' ')) {
		--n;
	}
	ui_version_[n] = '\0';
	size_t total = 0;
	size_t used = 0;
	esp_littlefs_info(kPartitionLabel, &total, &used);
	Log::info(kTag, "web UI image %s mounted at %s (%u of %u B used)", ui_version_, kMountPoint,
		static_cast<unsigned>(used), static_cast<unsigned>(total));
	return true;
}

bool WebServer::start() {
	if (server_ != nullptr) {
		Log::error(kTag, "refusing start: web server already running");
		return false;
	}
	g_server = this;
	log_heap("before the web server");
	job_done_ = xSemaphoreCreateBinaryStatic(&job_done_storage_);
	free_ = xQueueCreateStatic(kSendBuffers, 1, free_storage_, &free_control_);
	outgoing_ = xQueueCreateStatic(kSendBuffers, 1, outgoing_storage_, &outgoing_control_);
	for (uint8_t i = 0; i < kSendBuffers; ++i) {
		xQueueSend(free_, &i, 0);
	}
	ui_mounted_ = mount_ui();
	auth_.start();
	api_.set_feed(&feed_);
	if (!feed_.start()) {
		Log::error(kTag, "web server not started: the live feed cannot start");
		return false;
	}

	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.server_port = kPort;
	config.max_open_sockets = kMaxSockets;
	config.stack_size = kStack;
	config.lru_purge_enable = true;
	config.uri_match_fn = httpd_uri_match_wildcard;
	config.close_fn = &WebServer::close_handler;
	config.max_uri_handlers = 8;
	esp_err_t err = httpd_start(&server_, &config);
	if (err != ESP_OK) {
		Log::error(kTag, "web server not started: %s", esp_err_to_name(err));
		server_ = nullptr;
		return false;
	}

	httpd_uri_t api = {};
	api.uri = "/api/*";
	api.handler = &WebServer::api_handler;
	const httpd_method_t methods[] = {HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE};
	for (httpd_method_t method : methods) {
		api.method = method;
		httpd_register_uri_handler(server_, &api);
	}
	httpd_uri_t ws = {};
	ws.uri = "/ws";
	ws.method = HTTP_GET;
	ws.handler = &WebServer::ws_handler;
	ws.is_websocket = true;
	httpd_register_uri_handler(server_, &ws);
	httpd_uri_t files = {};
	files.uri = "/*";
	files.method = HTTP_GET;
	files.handler = &WebServer::file_handler;
	httpd_register_uri_handler(server_, &files);

	Log::info(kTag, "web server on port %u (http://%s.local/)%s", static_cast<unsigned>(kPort), device_name_,
		auth_.has_password() ? "" : ": no device password yet, the onboarding page sets it");
	log_heap("web server started");
	return true;
}

// ---- Jobs -------------------------------------------------------------------------------------

bool WebServer::run_on_loop() {
	if (job_busy_.exchange(true)) {
		return false; // the previous job timed out and has not run yet
	}
	uint32_t sequence = job_->sequence + 1;
	job_->sequence = sequence;
	if (!queue_.post(&WebServer::run_job, this, 0, nullptr, "web request")) {
		job_busy_.store(false);
		return false;
	}
	TickType_t start = xTaskGetTickCount();
	TickType_t timeout = pdMS_TO_TICKS(kJobTimeoutMs);
	while (job_->done.load() != sequence) {
		TickType_t waited = xTaskGetTickCount() - start;
		if (waited >= timeout || xSemaphoreTake(job_done_, timeout - waited) != pdTRUE) {
			if (job_->done.load() == sequence) {
				break;
			}
			Log::error(kTag, "the event loop did not take a web request within %u ms",
				static_cast<unsigned>(kJobTimeoutMs));
			return false;
		}
	}
	return true;
}

void WebServer::run_job(void* context, const LoopMessage&) {
	WebServer& self = *static_cast<WebServer*>(context);
	Job& job = *self.job_;
	switch (job.kind) {
		case Job::Kind::api:
			if (job.request.hash != nullptr) {
				Log::info(kTag, "PBKDF2-HMAC-SHA256, %u iterations: %u ms", static_cast<unsigned>(job.hash.params.iterations),
					static_cast<unsigned>(job.hash_us / 1000));
			}
			self.api_.handle(job.request, job.response);
			break;
		case Job::Kind::ws_open:
			self.feed_.open(job.handle, job.has_cookie ? job.cookie : nullptr, job.has_origin ? job.origin : nullptr,
				job.has_host ? job.host : nullptr);
			break;
		case Job::Kind::ws_message:
			self.feed_.message(job.handle, job.text, job.text_length);
			break;
	}
	job.done.store(job.sequence);
	self.job_busy_.store(false);
	xSemaphoreGive(self.job_done_);
}

void WebServer::post_closed(void* context, const LoopMessage& message) {
	static_cast<WebServer*>(context)->feed_.closed(static_cast<uint32_t>(message.number));
}

void WebServer::post_writable(void* context, const LoopMessage&) {
	static_cast<WebServer*>(context)->feed_.writable();
}

// ---- /api/* -----------------------------------------------------------------------------------

esp_err_t WebServer::api_handler(httpd_req_t* req) {
	return g_server->handle_api(req);
}

esp_err_t WebServer::handle_api(httpd_req_t* req) {
	if (job_busy_.load()) {
		return send_refusal(req, 503, "api.busy", "the controller is busy; try again");
	}
	if (req->content_len > WebApi::kMaxBody) {
		return send_refusal(req, 413, "api.body_too_large", "the request body is longer than 1024 bytes");
	}
	Job& job = *job_;
	job.kind = Job::Kind::api;
	job.request = WebRequest();
	job.response = WebResponse(g_response, sizeof(g_response));

	std::snprintf(job.path, sizeof(job.path), "%s", req->uri);
	char* query = std::strchr(job.path, '?');
	if (query != nullptr) {
		*query++ = '\0';
	}
	size_t received = 0;
	while (received < req->content_len) {
		int n = httpd_req_recv(req, job.body + received, req->content_len - received);
		if (n == HTTPD_SOCK_ERR_TIMEOUT) {
			continue;
		}
		if (n <= 0) {
			return ESP_FAIL; // connection lost: httpd closes it
		}
		received += static_cast<size_t>(n);
	}
	job.body[received] = '\0';

	WebRequest& request = job.request;
	request.method = method_of(req->method);
	request.path = job.path;
	request.query = query != nullptr ? query : "";
	request.cookie = copy_session_cookie(req, job.cookie, sizeof(job.cookie)) ? job.cookie : nullptr;
	request.authorization =
		copy_header(req, "Authorization", job.authorization, sizeof(job.authorization), "Bearer invalid")
		? job.authorization : nullptr;
	request.requested_with =
		copy_header(req, "X-Requested-With", job.requested_with, sizeof(job.requested_with), "invalid")
		? job.requested_with : nullptr;
	peer_address(httpd_req_to_sockfd(req), job.source, sizeof(job.source));
	request.source = job.source;
	request.body = job.body;
	request.body_length = received;

	if (!run_on_loop()) {
		return send_refusal(req, 503, "api.busy", "the controller is busy; try again");
	}
	if (job.response.needs_hash) {
		// The password hash, here in the httpd task: it takes long enough (doc/web.md) that the event
		// loop must not wait for it.
		job.hash = PasswordHash();
		job.hash.params = job.response.hash_params;
		int64_t start = esp_timer_get_time();
		crypto_.pbkdf2_sha256(reinterpret_cast<const uint8_t*>(job.response.password), job.response.password_length,
			job.hash.params.salt, sizeof(job.hash.params.salt), job.hash.params.iterations, job.hash.key,
			sizeof(job.hash.key));
		job.hash_us = esp_timer_get_time() - start;
		std::memset(job.response.password, 0, sizeof(job.response.password));
		request.hash = &job.hash;
		job.response = WebResponse(g_response, sizeof(g_response));
		bool ran = run_on_loop();
		std::memset(static_cast<void*>(&job.hash), 0, sizeof(job.hash));
		if (!ran) {
			return send_refusal(req, 503, "api.busy", "the controller is busy; try again");
		}
	}
	std::memset(job.body, 0, received + 1); // may hold a password

	WebResponse& response = job.response;
	static char retry_after[12];
	httpd_resp_set_status(req, status_line(response.status));
	httpd_resp_set_type(req, response.content_type);
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	if (response.set_cookie[0] != '\0') {
		httpd_resp_set_hdr(req, "Set-Cookie", response.set_cookie);
	}
	if (response.retry_after_s > 0) {
		std::snprintf(retry_after, sizeof(retry_after), "%u", static_cast<unsigned>(response.retry_after_s));
		httpd_resp_set_hdr(req, "Retry-After", retry_after);
	}
	return httpd_resp_send(req, response.body, static_cast<ssize_t>(response.length));
}

// ---- /ws --------------------------------------------------------------------------------------

WebServer::WsSlot* WebServer::ws_find(int fd) {
	for (WsSlot& slot : ws_) {
		if (slot.fd == fd) {
			return &slot;
		}
	}
	return nullptr;
}

esp_err_t WebServer::ws_handler(httpd_req_t* req) {
	return g_server->handle_ws(req);
}

namespace {

/// Sends a close frame right away (httpd task, inside a handler) and lets httpd close the socket.
esp_err_t close_now(httpd_req_t* req, uint16_t code, const char* reason) {
	uint8_t payload[125];
	httpd_ws_frame_t frame = {};
	frame.final = true;
	frame.type = HTTPD_WS_TYPE_CLOSE;
	frame.payload = payload;
	frame.len = websocket::encode_close(payload, code, reason);
	httpd_ws_send_frame(req, &frame);
	httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
	return ESP_OK;
}

} // namespace

esp_err_t WebServer::handle_ws(httpd_req_t* req) {
	int fd = httpd_req_to_sockfd(req);
	Job& job = *job_;
	if (req->method == HTTP_GET) {
		// The handshake is done; the feed decides whether the client may stay.
		WsSlot* slot = ws_find(-1);
		if (slot == nullptr) {
			return close_now(req, websocket::kCloseTryAgainLater, "too many WebSocket connections");
		}
		if (job_busy_.load()) {
			return close_now(req, websocket::kCloseTryAgainLater, "the controller is busy; try again");
		}
		slot->fd = fd;
		slot->generation = next_generation_++;
		if (next_generation_ == 0) {
			next_generation_ = 1;
		}
		slot->opened = false;
		slot->dead = false;
		job.kind = Job::Kind::ws_open;
		job.handle = make_handle(fd, slot->generation);
		job.has_cookie = copy_session_cookie(req, job.cookie, sizeof(job.cookie));
		job.has_origin = copy_header(req, "Origin", job.origin, sizeof(job.origin), "invalid://");
		job.has_host = copy_header(req, "Host", job.host, sizeof(job.host), "invalid");
		if (!run_on_loop()) {
			return close_now(req, websocket::kCloseTryAgainLater, "the controller is busy; try again");
		}
		slot->opened = true;
		log_heap("WebSocket client connected");
		return ESP_OK;
	}

	WsSlot* slot = ws_find(fd);
	httpd_ws_frame_t frame = {};
	esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
	if (err != ESP_OK) {
		return err;
	}
	if (slot == nullptr || slot->dead) {
		return ESP_FAIL; // not ours (any more): httpd closes the socket
	}
	if (frame.type == HTTPD_WS_TYPE_PONG) {
		uint8_t ignored[125];
		frame.payload = ignored;
		return httpd_ws_recv_frame(req, &frame, sizeof(ignored));
	}
	if (frame.type != HTTPD_WS_TYPE_TEXT || !frame.final) {
		slot->dead = true;
		return close_now(req, websocket::kCloseUnsupported, "only unfragmented text messages are accepted");
	}
	if (frame.len > kMaxIncoming) {
		slot->dead = true;
		return close_now(req, websocket::kCloseTooBig, "message longer than 1024 bytes");
	}
	if (job_busy_.load()) {
		slot->dead = true;
		return close_now(req, websocket::kCloseTryAgainLater, "the controller is busy; try again");
	}
	frame.payload = reinterpret_cast<uint8_t*>(job.text);
	err = httpd_ws_recv_frame(req, &frame, kMaxIncoming);
	if (err != ESP_OK) {
		return err;
	}
	if (!websocket::utf8_valid(frame.payload, frame.len)) {
		slot->dead = true;
		return close_now(req, websocket::kCloseInvalidPayload, "text message is not valid UTF-8");
	}
	job.text[frame.len] = '\0';
	job.text_length = frame.len;
	job.kind = Job::Kind::ws_message;
	job.handle = make_handle(fd, slot->generation);
	if (!run_on_loop()) {
		slot->dead = true;
		return close_now(req, websocket::kCloseTryAgainLater, "the controller is busy; try again");
	}
	return ESP_OK;
}

void WebServer::close_handler(httpd_handle_t, int fd) {
	if (g_server != nullptr) {
		g_server->handle_close(fd);
	}
	close(fd);
}

void WebServer::handle_close(int fd) {
	WsSlot* slot = ws_find(fd);
	if (slot == nullptr) {
		return;
	}
	if (slot->opened) {
		queue_.post(&WebServer::post_closed, this, static_cast<int32_t>(make_handle(fd, slot->generation)), nullptr,
			"WebSocket closed");
	}
	*slot = WsSlot();
	log_heap("WebSocket client disconnected");
}

bool WebServer::ws_send(uint32_t client, const char* text, size_t length) {
	if (server_ == nullptr || length > LiveFeed::kMessageSize) {
		return false;
	}
	uint8_t index = 0;
	if (xQueueReceive(free_, &index, 0) != pdTRUE) {
		// Set before the second look: a drain that frees a buffer afterwards sees the flag and posts
		// writable().
		starved_.store(true);
		if (xQueueReceive(free_, &index, 0) != pdTRUE) {
			return false;
		}
	}
	SendBuffer& buffer = buffers_[index];
	buffer.handle = client;
	buffer.close = false;
	buffer.length = static_cast<uint16_t>(length);
	std::memcpy(buffer.data, text, length);
	xQueueSend(outgoing_, &index, 0);
	if (!drain_queued_.exchange(true)) {
		if (httpd_queue_work(server_, &WebServer::drain_work, this) != ESP_OK) {
			// Sent with the next message (the status heartbeat at the latest).
			drain_queued_.store(false);
		}
	}
	return true;
}

void WebServer::ws_close(uint32_t client, uint16_t code, const char* reason) {
	if (server_ == nullptr) {
		return;
	}
	uint8_t index = 0;
	if (xQueueReceive(free_, &index, 0) != pdTRUE) {
		// No buffer for the close frame: close without one.
		httpd_sess_trigger_close(server_, handle_fd(client));
		return;
	}
	SendBuffer& buffer = buffers_[index];
	buffer.handle = client;
	buffer.close = true;
	buffer.length = static_cast<uint16_t>(websocket::encode_close(reinterpret_cast<uint8_t*>(buffer.data), code,
		reason != nullptr ? reason : ""));
	xQueueSend(outgoing_, &index, 0);
	if (!drain_queued_.exchange(true)) {
		if (httpd_queue_work(server_, &WebServer::drain_work, this) != ESP_OK) {
			drain_queued_.store(false);
		}
	}
}

void WebServer::drain_work(void* arg) {
	static_cast<WebServer*>(arg)->drain();
}

void WebServer::drain() {
	drain_queued_.store(false);
	uint8_t index = 0;
	while (xQueueReceive(outgoing_, &index, 0) == pdTRUE) {
		SendBuffer& buffer = buffers_[index];
		int fd = handle_fd(buffer.handle);
		WsSlot* slot = ws_find(fd);
		bool live = slot != nullptr && slot->generation == handle_generation(buffer.handle) && !slot->dead &&
			httpd_ws_get_fd_info(server_, fd) == HTTPD_WS_CLIENT_WEBSOCKET;
		if (live) {
			httpd_ws_frame_t frame = {};
			frame.final = true;
			frame.type = buffer.close ? HTTPD_WS_TYPE_CLOSE : HTTPD_WS_TYPE_TEXT;
			frame.payload = reinterpret_cast<uint8_t*>(buffer.data);
			frame.len = buffer.length;
			esp_err_t err = httpd_ws_send_frame_async(server_, fd, &frame);
			if (buffer.close || err != ESP_OK) {
				if (err != ESP_OK) {
					Log::warn(kTag, "WebSocket client on socket %d not writable (%s): closing it", fd,
						esp_err_to_name(err));
				}
				slot->dead = true;
				httpd_sess_trigger_close(server_, fd);
			}
		}
		xQueueSend(free_, &index, 0);
	}
	if (starved_.exchange(false)) {
		queue_.post(&WebServer::post_writable, this, 0, nullptr, "WebSocket writable");
	}
}

// ---- UI files ---------------------------------------------------------------------------------

esp_err_t WebServer::file_handler(httpd_req_t* req) {
	return g_server->handle_file(req);
}

esp_err_t WebServer::handle_file(httpd_req_t* req) {
	static const char kMissing[] =
		"<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" "
		"content=\"width=device-width\"><title>HomeControl</title></head><body style=\"font-family:sans-serif;"
		"background:#111;color:#eee;padding:1em\"><h1>HomeControl</h1><p>The web UI image is missing on this "
		"device: flash <code>storage.bin</code> to the storage partition (doc/web.md). The JSON API under "
		"<code>/api/</code> works without it.</p></body></html>";
	if (!ui_mounted_) {
		httpd_resp_set_status(req, status_line(503));
		httpd_resp_set_type(req, "text/html; charset=utf-8");
		httpd_resp_set_hdr(req, "Cache-Control", "no-store");
		return httpd_resp_send(req, kMissing, sizeof(kMissing) - 1);
	}
	static char path[kMaxPath + 32];
	static char uri[kMaxPath];
	std::snprintf(uri, sizeof(uri), "%s", req->uri);
	char* query = std::strchr(uri, '?');
	if (query != nullptr) {
		*query = '\0';
	}
	if (uri[0] != '/' || std::strstr(uri, "..") != nullptr || std::strchr(uri, '\\') != nullptr) {
		httpd_resp_set_status(req, status_line(400));
		httpd_resp_set_type(req, "text/plain; charset=utf-8");
		return httpd_resp_sendstr(req, "bad path\n");
	}
	const char* name = uri[1] == '\0' || uri[std::strlen(uri) - 1] == '/' ? "index.html" : "";
	std::snprintf(path, sizeof(path), "%s%s%s.gz", kUiDir, uri, name);
	struct stat info;
	if (stat(path, &info) != 0) {
		const char* last = std::strrchr(uri, '/');
		if (std::strchr(last, '.') != nullptr) {
			httpd_resp_set_status(req, status_line(404));
			httpd_resp_set_type(req, "text/plain; charset=utf-8");
			return httpd_resp_sendstr(req, "not found\n");
		}
		// A page of the single-page UI (/settings, /log, ...): the router in index.html shows it.
		std::snprintf(path, sizeof(path), "%s/index.html.gz", kUiDir);
	}
	static char etag[sizeof(ui_version_) + 2];
	std::snprintf(etag, sizeof(etag), "\"%s\"", ui_version_);
	static char if_none_match[sizeof(etag) + 8];
	if (copy_header(req, "If-None-Match", if_none_match, sizeof(if_none_match), "") &&
		std::strcmp(if_none_match, etag) == 0) {
		httpd_resp_set_status(req, status_line(304));
		httpd_resp_set_hdr(req, "ETag", etag);
		httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
		return httpd_resp_send(req, nullptr, 0);
	}
	FILE* file = std::fopen(path, "rb");
	if (file == nullptr) {
		httpd_resp_set_status(req, status_line(404));
		httpd_resp_set_type(req, "text/plain; charset=utf-8");
		return httpd_resp_sendstr(req, "not found\n");
	}
	// The content type of the file without its ".gz".
	path[std::strlen(path) - 3] = '\0';
	httpd_resp_set_type(req, content_type_of(path));
	httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
	httpd_resp_set_hdr(req, "ETag", etag);
	httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
	httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
	httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
	esp_err_t err = ESP_OK;
	for (;;) {
		size_t n = std::fread(g_file_chunk, 1, sizeof(g_file_chunk), file);
		if (n == 0) {
			break;
		}
		err = httpd_resp_send_chunk(req, g_file_chunk, static_cast<ssize_t>(n));
		if (err != ESP_OK) {
			break;
		}
	}
	std::fclose(file);
	if (err != ESP_OK) {
		return err;
	}
	return httpd_resp_send_chunk(req, nullptr, 0);
}

// ---- Console ----------------------------------------------------------------------------------

int WebServer::console_web(void* context, int argc, char** argv) {
	return static_cast<WebServer*>(context)->web_command(argc, argv);
}

int WebServer::web_command(int argc, char** argv) {
	static const char kUsage[] =
		"web status | token create <name> | token list | token revoke <name> | password clear | demo";
	const char* sub = argc >= 2 ? argv[1] : "status";
	if (std::strcmp(sub, "status") == 0 && argc <= 2) {
		std::printf("server    %s, http://%s.local/\n", server_ != nullptr ? "running on port 80" : "not running",
			device_name_);
		std::printf("password  %s\n", auth_.has_password() ? "set" : "not set (the onboarding page sets it)");
		std::printf("sessions  %u of %u\n", static_cast<unsigned>(auth_.session_count()),
			static_cast<unsigned>(Auth::kSessionSlots));
		std::printf("tokens    %u of %u\n", static_cast<unsigned>(auth_.token_count()),
			static_cast<unsigned>(Auth::kMaxTokens));
		std::printf("live      %u of %u WebSocket clients\n", static_cast<unsigned>(feed_.client_count()),
			static_cast<unsigned>(LiveFeed::kMaxClients));
		std::printf("ui        %s\n", ui_mounted_ ? ui_version_ : "missing (flash storage.bin)");
		std::printf("heap      %u B free (min %u B)\n", static_cast<unsigned>(esp_get_free_heap_size()),
			static_cast<unsigned>(esp_get_minimum_free_heap_size()));
		return 0;
	}
	if (std::strcmp(sub, "token") == 0 && argc >= 3) {
		const char* action = argv[2];
		if (std::strcmp(action, "create") == 0 && argc == 4) {
			char token[Auth::kTokenText];
			AuthStatus status = auth_.create_token(argv[3], token);
			if (status != AuthStatus::ok) {
				std::printf("error: %s\n", auth_status_text(status));
				return 1;
			}
			std::printf("token %s created; it is shown only now, store it in the client:\n%s\n", argv[3], token);
			std::printf("use it as the header  Authorization: Bearer %s\n", token);
			std::memset(token, 0, sizeof(token));
			return 0;
		}
		if (std::strcmp(action, "list") == 0 && argc == 3) {
			size_t count = 0;
			for (size_t i = 0; i < Auth::kMaxTokens; ++i) {
				const char* name = auth_.token_name(i);
				if (name != nullptr && name[0] != '\0') {
					std::printf("%s\n", name);
					++count;
				}
			}
			if (count == 0) {
				std::printf("no tokens (web token create <name>)\n");
			}
			return 0;
		}
		if (std::strcmp(action, "revoke") == 0 && argc == 4) {
			AuthStatus status = auth_.revoke_token(argv[3]);
			if (status != AuthStatus::ok) {
				std::printf("error: %s\n", auth_status_text(status));
				return 1;
			}
			std::printf("token %s revoked\n", argv[3]);
			return 0;
		}
	}
	if (std::strcmp(sub, "password") == 0 && argc == 3 && std::strcmp(argv[2], "clear") == 0) {
		AuthStatus status = auth_.clear_password();
		if (status != AuthStatus::ok) {
			std::printf("error: %s\n", auth_status_text(status));
			return 1;
		}
		std::printf("device password cleared, all sessions ended; set a new one at http://%s.local/\n", device_name_);
		return 0;
	}
	if (std::strcmp(sub, "demo") == 0 && argc == 2) {
		if (!demo_.load()) {
			std::printf("error: the demo datapoints are not loaded: the model holds datapoints already\n");
			return 1;
		}
		std::printf("demo datapoints loaded until the next restart\n");
		return 0;
	}
	std::printf("usage: %s\n", kUsage);
	return 1;
}

} // namespace esp
} // namespace hc
