/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "web_server.h"

#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"
#include "sdkconfig.h"
#if !CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
#error "the live feed opens its clients in ws_post_handshake_cb: set CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT (sdkconfig.defaults.app)"
#endif
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "hc/image/image_body.h"
#include "hc/log.h"
#include "hc/network_settings.h"
#include "hc/signature.h"
#include "hc/update_check.h"
#include "hc/websocket.h"
#include "hc/language_packs.h"
#include "littlefs_blob_store.h"
#include "storage.h"
#include "ui_files.h"

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
		case 410: return "410 Gone";
		case 413: return "413 Content Too Large";
		case 415: return "415 Unsupported Media Type";
		case 422: return "422 Unprocessable Content";
		case 429: return "429 Too Many Requests";
		case 500: return "500 Internal Server Error";
		case 501: return "501 Not Implemented";
		case 502: return "502 Bad Gateway";
		case 503: return "503 Service Unavailable";
		case 504: return "504 Gateway Timeout";
		case 507: return "507 Insufficient Storage";
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
	enum class Kind : uint8_t { api, api_poll, api_stream, ws_open, ws_message };

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
	/// X-Image-Signature (128 hex digits; a longer value becomes "invalid", which the API refuses).
	char image_signature[2 * kEd25519SignatureSize + 1] = {};
	char source[Auth::kMaxSource] = {};
	// The request body of an api job and the text of a ws_message job share their storage: the
	// slot holds one job at a time (the httpd task waits for it), and the kinds exclude each other.
	union {
		char body[WebApi::kMaxBody + 1];
		char text[kMaxIncoming + 1];
	};

	// api_stream: the piece at `offset` of the streamed document `token`, into g_response
	uint32_t stream_token = 0;
	size_t stream_offset = 0;
	size_t stream_got = 0;
	bool stream_ok = false;

	// ws_open, ws_message
	uint32_t handle = 0;
	bool has_cookie = false;
	bool has_origin = false;
	bool has_host = false;
	char origin[kMaxOrigin] = {};
	char host[kMaxOrigin] = {};
	size_t text_length = 0;
};

namespace {

/// Static storage: the job, the API's response body and the send ring live in .bss, not on the heap
/// (doc/core.md: no heap in steady state).
char g_response[WebServer::kResponseBuffer];
alignas(4) uint8_t g_send_ring[WebServer::kSendRingBytes];
static_assert(LiveFeed::kMessageSize == WsSendRing::kMaxMessage && LiveFeed::kMaxClients == WsSendRing::kMaxRecipients,
	"the send ring holds the live feed's messages");
/// The chunk a file is sent in: the front of g_response. The httpd task serves one request at a
/// time, and handle_file() first waits for a job of an earlier request that timed out and may still
/// write g_response on the loop (wait_job()); the WebSocket feed sends from its own buffers.
static_assert(kFileChunk <= WebServer::kResponseBuffer, "the file chunk aliases the response buffer");
char (&g_file_chunk)[kFileChunk] = *reinterpret_cast<char (*)[kFileChunk]>(g_response);

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
	  ring_(g_send_ring, sizeof(g_send_ring)),
	  console_command_{"web", "status | token create <name> | token list | token revoke <name> | session clear | password clear | demo",
		  "Web server (doc/web.md): state, API tokens for machines (shown once, stored hashed), log out everywhere, reset the device "
		  "password (onboarding opens again), load the demo datapoints until the next restart",
		  &WebServer::console_web, this} {
	static Job job;
	job_ = &job;
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
	// The release key the firmware trusts for manifests (doc/ota.md "Manifest signature").
	if (const uint8_t* key = release_public_key()) {
		char fingerprint[kFingerprintSize];
		key_fingerprint(key, fingerprint);
		json.member("release_key", fingerprint);
	}
	uint64_t now = controller_.clock().monotonic_ms();
	if (ota_.last_result_ms() != 0) {
		json.member("last_ago_s", (now - ota_.last_result_ms()) / 1000);
	}
	if (has_url) {
		uint64_t next = ota_.next_check_ms();
		json.member("next_check_s", next > now ? (next - now) / 1000 : 0);
	}
}

// ---- The console's system commands over the API (issue #38) ------------------------------------

HostResult WebServer::system_action_possible(SystemAction, const char**) const {
	return HostResult::ok;
}

void WebServer::system_action(SystemAction action) {
	if (action == SystemAction::factory_reset) {
		factory_reset_and_restart(blobs_, "over the API");
	}
	Log::warn(kTag, "restart requested over the API: restarting");
	vTaskDelay(pdMS_TO_TICKS(100)); // let the log line out
	esp_restart();
}

HostResult WebServer::start_update(bool force, const char* url, const char** reason) {
	return ota_.check(force, url, reason) ? HostResult::ok : HostResult::refused;
}

HostResult WebServer::start_upload(const char* token, const char** reason) {
	return ota_.start_upload(token, reason) ? HostResult::ok : HostResult::refused;
}

HostResult WebServer::clear_coredump(const char** reason) {
	if (diagnostics_ == nullptr) {
		*reason = "this build has no diagnostics";
		return HostResult::unavailable;
	}
	if (!diagnostics_->coredump_clear()) {
		*reason = "the core dump could not be erased (see the log)";
		return HostResult::refused;
	}
	return HostResult::ok;
}

bool WebServer::set_platform_log_level(const char* tag, LogLevel level) {
	esp_log_level_t esp = ESP_LOG_INFO;
	switch (level) {
		case LogLevel::error: esp = ESP_LOG_ERROR; break;
		case LogLevel::warn: esp = ESP_LOG_WARN; break;
		case LogLevel::info: esp = ESP_LOG_INFO; break;
		case LogLevel::debug: esp = ESP_LOG_DEBUG; break;
	}
	esp_log_level_set(tag, esp);
	return true;
}

namespace {

/// Removes the directory `path` with everything in it (LittleFS, single task); false if some entry
/// stays. `path` is a buffer of `capacity` bytes that the names of the entries are appended to.
bool remove_tree(char* path, size_t capacity) {
	DIR* dir = opendir(path);
	if (dir == nullptr) {
		return false;
	}
	size_t length = std::strlen(path);
	bool ok = true;
	// Every removal starts the listing again: LittleFS does not promise a stable listing while it
	// changes. An entry that cannot be removed ends the walk.
	for (;;) {
		struct dirent* entry = readdir(dir);
		while (entry != nullptr && (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)) {
			entry = readdir(dir);
		}
		if (entry == nullptr) {
			break;
		}
		int n = std::snprintf(path + length, capacity - length, "/%s", entry->d_name);
		if (n < 0 || static_cast<size_t>(n) >= capacity - length) {
			ok = false;
			break;
		}
		bool directory = entry->d_type == DT_DIR;
		bool removed = directory ? remove_tree(path, capacity) : unlink(path) == 0;
		path[length] = '\0';
		if (!removed) {
			ok = false;
			break;
		}
		rewinddir(dir);
	}
	closedir(dir);
	return ok && rmdir(path) == 0;
}

} // namespace

void WebServer::remove_legacy_ui() {
	// Up to firmware 2026-11 the UI was a directory of the storage partition, written by flashing
	// storage.bin; it now comes with the firmware (ui_files.h). Its about 30 blocks go back to the
	// device's state (the stored runtime image, values and history; haumacher/homecontrol#45).
	if (!storage_mount()) {
		return;
	}
	struct stat info;
	if (stat(kLegacyUiDir, &info) != 0) {
		return;
	}
	size_t total = 0;
	size_t before = 0;
	hc::esp::storage_usage(total, before);
	static char path[kMaxPath + 32];
	std::snprintf(path, sizeof(path), "%s", kLegacyUiDir);
	bool removed = remove_tree(path, sizeof(path));
	size_t after = 0;
	hc::esp::storage_usage(total, after);
	if (removed) {
		Log::info(kTag, "removed the old web UI image %s: %u B freed, %u of %u B used", kLegacyUiDir,
			static_cast<unsigned>(before - after), static_cast<unsigned>(after), static_cast<unsigned>(total));
	} else {
		Log::error(kTag, "cannot remove the old web UI image %s completely (%u B freed)", kLegacyUiDir,
			static_cast<unsigned>(before > after ? before - after : 0));
	}
}

const char* WebServer::ui_version() const {
	return ui::kVersion;
}

const ui::UiFile* WebServer::find_ui_file(const char* path) {
	size_t low = 0;
	size_t high = ui::kFileCount;
	while (low < high) {
		size_t mid = low + (high - low) / 2;
		int order = std::strcmp(ui::kFiles[mid].path, path);
		if (order == 0) {
			return &ui::kFiles[mid];
		}
		if (order < 0) {
			low = mid + 1;
		} else {
			high = mid;
		}
	}
	return nullptr;
}

bool WebServer::start() {
	if (server_ != nullptr) {
		Log::error(kTag, "refusing start: web server already running");
		return false;
	}
	g_server = this;
	log_heap("before the web server");
	job_done_ = xSemaphoreCreateBinaryStatic(&job_done_storage_);
	remove_legacy_ui();
	Log::info(kTag, "web UI %s, %u files in the firmware", ui::kVersion, static_cast<unsigned>(ui::kFileCount));
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
	// ESP-IDF 5.5 no longer calls the handler after the handshake (5.3 did, with HTTP_GET): the client
	// is opened here, the handler gets the frames.
	ws.ws_post_handshake_cb = &WebServer::ws_opened;
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

namespace {

/// What a request may hold the event loop (homecontrol #74): above it the request is logged as a
/// warning with its path, below at debug level (`log level hc.web debug`).
constexpr int64_t kJobBudgetUs = 20000;

const char* method_text(HttpMethod method) {
	switch (method) {
		case HttpMethod::get: return "GET";
		case HttpMethod::post: return "POST";
		case HttpMethod::put: return "PUT";
		case HttpMethod::del: return "DELETE";
		case HttpMethod::other: break;
	}
	return "?";
}

} // namespace

void WebServer::run_job(void* context, const LoopMessage&) {
	WebServer& self = *static_cast<WebServer*>(context);
	Job& job = *self.job_;
	int64_t start = esp_timer_get_time();
	switch (job.kind) {
		case Job::Kind::api:
			if (job.request.hash != nullptr) {
				Log::info(kTag, "PBKDF2-HMAC-SHA256, %u iterations: %u ms", static_cast<unsigned>(job.hash.params.iterations),
					static_cast<unsigned>(job.hash_us / 1000));
			}
			self.api_.handle(job.request, job.response);
			break;
		case Job::Kind::api_poll:
			job.stream_ok = self.api_.poll_deferred(job.response);
			break;
		case Job::Kind::api_stream:
			job.stream_ok = self.api_.read_stream(job.stream_token, job.stream_offset, g_response, sizeof(g_response),
				job.stream_got);
			break;
		case Job::Kind::ws_open:
			self.feed_.open(job.handle, job.has_cookie ? job.cookie : nullptr, job.has_origin ? job.origin : nullptr,
				job.has_host ? job.host : nullptr);
			break;
		case Job::Kind::ws_message:
			self.feed_.message(job.handle, job.text, job.text_length);
			break;
	}
	// The time the request held the loop, per path (homecontrol #74); the PBKDF2 hash ran before, on
	// the httpd task.
	int64_t took = esp_timer_get_time() - start;
	if (took > kJobBudgetUs || Log::enabled(LogLevel::debug, kTag)) {
		const char* what = "";
		const char* path = "";
		switch (job.kind) {
			case Job::Kind::api: what = method_text(job.request.method); path = job.request.path; break;
			case Job::Kind::api_poll: what = "poll"; path = job.request.path; break;
			case Job::Kind::api_stream: what = "stream"; path = job.request.path; break;
			case Job::Kind::ws_open: what = "WebSocket open"; break;
			case Job::Kind::ws_message: what = "WebSocket message"; break;
		}
		unsigned ms = static_cast<unsigned>(took / 1000);
		unsigned fraction = static_cast<unsigned>(took % 1000 / 100);
		if (took > kJobBudgetUs) {
			Log::warn(kTag, "%s %s held the event loop %u.%u ms (budget %u ms)", what, path, ms, fraction,
				static_cast<unsigned>(kJobBudgetUs / 1000));
		} else {
			Log::debug(kTag, "%s %s: %u.%u ms on the event loop", what, path, ms, fraction);
		}
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

void WebServer::wait_job() {
	// A job that timed out still runs later: memory it points to stays valid until it has.
	while (job_busy_.load()) {
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

esp_err_t WebServer::handle_api(httpd_req_t* req) {
	if (job_busy_.load()) {
		return send_refusal(req, 503, "api.busy", "the controller is busy; try again");
	}
	Job& job = *job_;
	std::snprintf(job.path, sizeof(job.path), "%s", req->uri);
	char* query = std::strchr(job.path, '?');
	if (query != nullptr) {
		*query++ = '\0';
	}
	size_t limit = api_.body_limit(method_of(req->method), job.path);
	if (req->content_len > limit) {
		static char text[128];
		api_.body_too_long(method_of(req->method), job.path, req->content_len, text, sizeof(text));
		Log::warn(kTag, "refusing %s: %s", job.path, text);
		return send_refusal(req, 413, "api.body_too_large", text);
	}
	HttpMethod method = method_of(req->method);
	if (const char* busy = api_.body_refusal(method, job.path)) {
		// The previous image is still being stored off the loop (#68): not even the body is read.
		Log::warn(kTag, "refusing %s: %s", job.path, busy);
		httpd_resp_set_hdr(req, "Retry-After", "1");
		return send_refusal(req, 503, "image.busy", busy);
	}
	// A large body (the runtime image) in the image staging area (#66) or a heap buffer of its own,
	// released on every path out (unless the image service took it over for its store, #68).
	image::ImageBody large;
	bool use_large = req->content_len > WebApi::kMaxBody;
	if (use_large) {
		const char* staging_refusal = nullptr;
		if (!large.begin(req->content_len, api_.body_staging(method, job.path), &staging_refusal)) {
			Log::warn(kTag, "refusing a %u-byte body for %s: no heap block that large (largest %u B)%s%s",
				static_cast<unsigned>(req->content_len), job.path,
				static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
				staging_refusal != nullptr ? "; the staging area declined: " : "",
				staging_refusal != nullptr ? staging_refusal : "");
			return send_refusal(req, 507, "api.no_memory", "the device has not enough free memory for this body");
		}
		if (staging_refusal != nullptr) {
			Log::info(kTag, "the %u-byte body for %s goes to the heap: %s", static_cast<unsigned>(req->content_len),
				job.path, staging_refusal);
		}
	}
	job.kind = Job::Kind::api;
	job.request = WebRequest();
	job.response = WebResponse(g_response, sizeof(g_response));

	size_t received = 0;
	while (received < req->content_len) {
		// A large body passes through the job's buffer in pieces.
		char* into = use_large ? job.body : job.body + received;
		size_t room = use_large ? WebApi::kMaxBody : req->content_len - received;
		size_t want = req->content_len - received < room ? req->content_len - received : room;
		int n = httpd_req_recv(req, into, want);
		if (n == HTTPD_SOCK_ERR_TIMEOUT) {
			continue;
		}
		if (n <= 0) {
			return ESP_FAIL; // connection lost: httpd closes it
		}
		if (use_large && !large.append(into, static_cast<size_t>(n))) {
			Log::warn(kTag, "refusing the body for %s: the staging area failed", job.path);
			return send_refusal(req, 507, "api.no_memory", "the device could not store this body");
		}
		received += static_cast<size_t>(n);
	}
	const char* body = job.body;
	if (use_large) {
		body = large.finish();
		if (body == nullptr) {
			Log::warn(kTag, "refusing the body for %s: the staged body cannot be read", job.path);
			return send_refusal(req, 507, "api.no_memory", "the device could not read this body back");
		}
	} else {
		job.body[received] = '\0';
	}

	WebRequest& request = job.request;
	request.method = method;
	request.path = job.path;
	request.query = query != nullptr ? query : "";
	request.cookie = copy_session_cookie(req, job.cookie, sizeof(job.cookie)) ? job.cookie : nullptr;
	request.authorization =
		copy_header(req, "Authorization", job.authorization, sizeof(job.authorization), "Bearer invalid")
		? job.authorization : nullptr;
	request.requested_with =
		copy_header(req, "X-Requested-With", job.requested_with, sizeof(job.requested_with), "invalid")
		? job.requested_with : nullptr;
	request.image_signature =
		copy_header(req, "X-Image-Signature", job.image_signature, sizeof(job.image_signature), "invalid")
		? job.image_signature : nullptr;
	peer_address(httpd_req_to_sockfd(req), job.source, sizeof(job.source));
	request.source = job.source;
	request.body = body;
	request.body_length = received;
	request.body_holder = use_large ? &large : nullptr;

	if (!run_on_loop()) {
		if (use_large) {
			wait_job(); // the late job still reads the body
		}
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
	if (job.response.deferred) {
		// The answer waits for another service (the E-Home enrollment, doc/ehome.md): poll it on the
		// loop. The httpd task waits meanwhile (the core bounds the wait well below the limit).
		job.stream_ok = false;
		for (uint32_t waited = 0; !job.stream_ok && waited < WebApi::kDeferredLimitMs;
			waited += WebApi::kDeferredPollMs) {
			vTaskDelay(pdMS_TO_TICKS(WebApi::kDeferredPollMs));
			job.kind = Job::Kind::api_poll;
			if (!run_on_loop()) {
				wait_job();
			}
		}
		if (!job.stream_ok) {
			return send_refusal(req, 504, "api.timeout", "the answer did not come in time");
		}
	}
	if (!use_large) {
		std::memset(job.body, 0, received + 1); // may hold a password
	}

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
	if (response.stream) {
		return send_stream(req, response.stream_length, response.stream_token);
	}
	return httpd_resp_send(req, response.body, static_cast<ssize_t>(response.length));
}

esp_err_t WebServer::send_stream(httpd_req_t* req, size_t length, uint32_t token) {
	// Chunked: each piece is read on the loop into g_response, then sent from here.
	Job& job = *job_;
	size_t sent = 0;
	while (sent < length) {
		job.kind = Job::Kind::api_stream;
		job.stream_token = token;
		job.stream_offset = sent;
		job.stream_got = 0;
		job.stream_ok = false;
		if (!run_on_loop()) {
			wait_job();
			Log::warn(kTag, "streamed response cut after %u of %u bytes: the loop was busy", static_cast<unsigned>(sent),
				static_cast<unsigned>(length));
			return ESP_FAIL; // the client sees the connection close before the last chunk
		}
		if (!job.stream_ok || job.stream_got == 0) {
			Log::warn(kTag, "streamed response cut after %u of %u bytes: the document changed",
				static_cast<unsigned>(sent), static_cast<unsigned>(length));
			return ESP_FAIL;
		}
		size_t piece = job.stream_got < length - sent ? job.stream_got : length - sent;
		if (httpd_resp_send_chunk(req, g_response, static_cast<ssize_t>(piece)) != ESP_OK) {
			return ESP_FAIL;
		}
		sent += piece;
	}
	return httpd_resp_send_chunk(req, nullptr, 0);
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

esp_err_t WebServer::ws_opened(httpd_req_t* req) {
	return g_server->handle_ws_open(req);
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

// After the handshake (ws_post_handshake_cb): the feed decides whether the client may stay. Every
// refusal is logged: a browser only sees the close.
esp_err_t WebServer::handle_ws_open(httpd_req_t* req) {
	int fd = httpd_req_to_sockfd(req);
	Job& job = *job_;
	WsSlot* slot = ws_find(-1);
	if (slot == nullptr) {
		Log::warn(kTag, "WebSocket refused: all %u connection slots taken", static_cast<unsigned>(kMaxWs));
		return close_now(req, websocket::kCloseTryAgainLater, "too many WebSocket connections");
	}
	if (job_busy_.load()) {
		Log::warn(kTag, "WebSocket refused: the controller is busy with a request");
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
		Log::warn(kTag, "WebSocket refused: the event loop did not take the open in time");
		return close_now(req, websocket::kCloseTryAgainLater, "the controller is busy; try again");
	}
	slot->opened = true;
	log_heap("WebSocket client connected");
	return ESP_OK;
}

esp_err_t WebServer::handle_ws(httpd_req_t* req) {
	int fd = httpd_req_to_sockfd(req);
	Job& job = *job_;
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

void WebServer::kick(bool refused) {
	if (refused) {
		// Set before the drain looks: a drain that frees room afterwards sees the flag and posts
		// writable().
		starved_.store(true);
	}
	if (server_ != nullptr && !drain_queued_.exchange(true)) {
		if (httpd_queue_work(server_, &WebServer::drain_work, this) != ESP_OK) {
			// Sent with the next message (the status heartbeat at the latest).
			drain_queued_.store(false);
		}
	}
}

char* WebServer::ws_claim(bool bulk) {
	if (server_ == nullptr) {
		return nullptr;
	}
	char* claim = ring_.claim(LiveFeed::kMessageSize, bulk);
	if (claim == nullptr) {
		kick(true);
	}
	return claim;
}

void WebServer::ws_commit(const uint32_t* clients, size_t count, size_t length) {
	ring_.commit(clients, count, length);
	kick(false);
}

bool WebServer::ws_send(uint32_t client, const char* text, size_t length) {
	if (server_ == nullptr || length > LiveFeed::kMessageSize) {
		return false;
	}
	bool ok = ring_.push(&client, 1, text, length, false, false);
	kick(!ok);
	return ok;
}

bool WebServer::ws_send_bulk(uint32_t client, const char* text, size_t length) {
	if (server_ == nullptr || length > LiveFeed::kMessageSize) {
		return false;
	}
	bool ok = ring_.push(&client, 1, text, length, false, true);
	kick(!ok);
	return ok;
}

void WebServer::ws_close(uint32_t client, uint16_t code, const char* reason) {
	if (server_ == nullptr) {
		return;
	}
	uint8_t frame[websocket::kMaxControlPayload];
	size_t length = websocket::encode_close(frame, code, reason != nullptr ? reason : "");
	if (!ring_.push(&client, 1, reinterpret_cast<const char*>(frame), length, true, false)) {
		// No room for the close frame: close without one.
		httpd_sess_trigger_close(server_, handle_fd(client));
		return;
	}
	kick(false);
}

void WebServer::drain_work(void* arg) {
	static_cast<WebServer*>(arg)->drain();
}

void WebServer::drain() {
	drain_queued_.store(false);
	WsSendRing::Entry entry;
	while (ring_.front(entry)) {
		for (size_t i = 0; i < entry.count; ++i) {
			uint32_t handle = entry.clients[i];
			int fd = handle_fd(handle);
			WsSlot* slot = ws_find(fd);
			bool live = slot != nullptr && slot->generation == handle_generation(handle) && !slot->dead &&
				httpd_ws_get_fd_info(server_, fd) == HTTPD_WS_CLIENT_WEBSOCKET;
			if (!live) {
				continue;
			}
			httpd_ws_frame_t frame = {};
			frame.final = true;
			frame.type = entry.close ? HTTPD_WS_TYPE_CLOSE : HTTPD_WS_TYPE_TEXT;
			frame.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(entry.data));
			frame.len = entry.length;
			esp_err_t err = httpd_ws_send_frame_async(server_, fd, &frame);
			if (entry.close || err != ESP_OK) {
				if (err != ESP_OK) {
					Log::warn(kTag, "WebSocket client on socket %d not writable (%s): closing it", fd,
						esp_err_to_name(err));
				}
				slot->dead = true;
				httpd_sess_trigger_close(server_, fd);
			}
		}
		ring_.pop();
	}
	if (starved_.exchange(false)) {
		queue_.post(&WebServer::post_writable, this, 0, nullptr, "WebSocket writable");
	}
}

// ---- UI files ---------------------------------------------------------------------------------

esp_err_t WebServer::file_handler(httpd_req_t* req) {
	return g_server->handle_file(req);
}

/// A language pack the device downloaded for its release (doc/i18n.md): GET /i18n/<lang>.json is
/// the blob `i18n.<lang>.json.gz` of the LanguagePacks, sent as stored (gzip). Read straight from
/// the blob store's directory (LittleFS serializes the access; the blob is replaced atomically by a
/// rename, so a reader sees the old or the new pack). 204 when there is none (an expected state,
/// not an error for the browser console): the UI then uses the pack bundled with it, or English.
/// Not cached: a new pack can arrive at any time.
static esp_err_t send_language_pack(httpd_req_t* req, const char* uri) {
	static constexpr const char kPrefix[] = "/i18n/";
	char lang[setting::kMaxUiLangLength + 1] = {};
	const char* code = uri + sizeof(kPrefix) - 1;
	const char* dot = std::strchr(code, '.');
	size_t length = dot != nullptr ? static_cast<size_t>(dot - code) : 0;
	char name[BlobStore::kMaxNameLength + 1];
	FILE* file = nullptr;
	if (dot != nullptr && std::strcmp(dot, ".json") == 0 && length < sizeof(lang)) {
		std::memcpy(lang, code, length);
		if (LanguagePacks::blob_name(lang, name, sizeof(name))) {
			char path[64 + sizeof(name)]; // "/storage/state/" + the blob name
			std::snprintf(path, sizeof(path), "%s/%s", LittleFsBlobStore::kDirectory, name);
			file = std::fopen(path, "rb");
		}
	}
	if (file == nullptr) {
		httpd_resp_set_status(req, status_line(204)); // no downloaded language pack
		httpd_resp_set_hdr(req, "Cache-Control", "no-store");
		return httpd_resp_send(req, nullptr, 0);
	}
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
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
	return err != ESP_OK ? err : httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t WebServer::handle_file(httpd_req_t* req) {
	static char uri[kMaxPath + 16];
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
	if (std::strncmp(uri, "/i18n/", 6) == 0) {
		wait_job(); // send_language_pack() reads into g_file_chunk, which is g_response
		return send_language_pack(req, uri);
	}
	size_t length = std::strlen(uri);
	if (uri[length - 1] == '/' && length + 10 < sizeof(uri)) {
		std::strcat(uri, "index.html");
	}
	const ui::UiFile* file = find_ui_file(uri);
	if (file == nullptr) {
		const char* last = std::strrchr(uri, '/');
		if (std::strchr(last, '.') != nullptr) {
			httpd_resp_set_status(req, status_line(404));
			httpd_resp_set_type(req, "text/plain; charset=utf-8");
			return httpd_resp_sendstr(req, "not found\n");
		}
		// A page of the single-page UI (/settings, /log, ...): the router in index.html shows it.
		std::snprintf(uri, sizeof(uri), "/index.html");
		file = find_ui_file(uri);
		if (file == nullptr) {
			httpd_resp_set_status(req, status_line(404));
			httpd_resp_set_type(req, "text/plain; charset=utf-8");
			return httpd_resp_sendstr(req, "not found\n");
		}
	}
	static char etag[24];
	std::snprintf(etag, sizeof(etag), "\"%s\"", ui::kVersion);
	static char if_none_match[sizeof(etag) + 8];
	if (copy_header(req, "If-None-Match", if_none_match, sizeof(if_none_match), "") &&
		std::strcmp(if_none_match, etag) == 0) {
		httpd_resp_set_status(req, status_line(304));
		httpd_resp_set_hdr(req, "ETag", etag);
		httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
		return httpd_resp_send(req, nullptr, 0);
	}
	httpd_resp_set_type(req, content_type_of(file->path));
	httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
	httpd_resp_set_hdr(req, "ETag", etag);
	httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
	httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
	httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
	// Sent straight from the application image in flash, in pieces of kFileChunk (the socket copies them).
	for (size_t offset = 0; offset < file->size; offset += kFileChunk) {
		size_t n = file->size - offset < kFileChunk ? file->size - offset : kFileChunk;
		esp_err_t err = httpd_resp_send_chunk(req, reinterpret_cast<const char*>(file->data + offset),
			static_cast<ssize_t>(n));
		if (err != ESP_OK) {
			return err;
		}
	}
	return httpd_resp_send_chunk(req, nullptr, 0);
}

// ---- Console ----------------------------------------------------------------------------------

int WebServer::console_web(void* context, int argc, char** argv) {
	return static_cast<WebServer*>(context)->web_command(argc, argv);
}

int WebServer::web_command(int argc, char** argv) {
	static const char kUsage[] =
		"web status | token create <name> | token list | token revoke <name> | session clear | password clear | demo";
	const char* sub = argc >= 2 ? argv[1] : "status";
	if (std::strcmp(sub, "status") == 0 && argc <= 2) {
		std::printf("server    %s, http://%s.local/\n", server_ != nullptr ? "running on port 80" : "not running",
			device_name_);
		std::printf("password  %s\n", auth_.has_password() ? "set" : "not set (the onboarding page sets it)");
		std::printf("sessions  %u (%u of %u remembered, %u of %u in RAM)\n", static_cast<unsigned>(auth_.session_count()),
			static_cast<unsigned>(auth_.remembered_count()), static_cast<unsigned>(Auth::kRememberedSlots),
			static_cast<unsigned>(auth_.session_count() - auth_.remembered_count()),
			static_cast<unsigned>(Auth::kSessionSlots));
		std::printf("tokens    %u of %u\n", static_cast<unsigned>(auth_.token_count()),
			static_cast<unsigned>(Auth::kMaxTokens));
		std::printf("live      %u of %u WebSocket clients\n", static_cast<unsigned>(feed_.client_count()),
			static_cast<unsigned>(LiveFeed::kMaxClients));
		std::printf("send ring %u of %u B free (lowest %u B)\n", static_cast<unsigned>(ring_.free_bytes()),
			static_cast<unsigned>(ring_.capacity()), static_cast<unsigned>(ring_.lowest_free()));
		std::printf("ui        %s, %u files in the firmware\n", ui::kVersion, static_cast<unsigned>(ui::kFileCount));
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
	if (std::strcmp(sub, "session") == 0 && argc == 3 && std::strcmp(argv[2], "clear") == 0) {
		size_t ended = 0;
		AuthStatus status = auth_.clear_sessions(&ended);
		if (status != AuthStatus::ok) {
			std::printf("error: %s (the sessions ended until the restart)\n", auth_status_text(status));
			return 1;
		}
		std::printf("all sessions ended (%u), the remembered ones included\n", static_cast<unsigned>(ended));
		return 0;
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
