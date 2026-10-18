/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_WEB_SERVER_H
#define HC_ESP_WEB_SERVER_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "console.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "hc/auth.h"
#include "hc/controller.h"
#include "hc/datapoint.h"
#include "hc/demo.h"
#include "hc/live_feed.h"
#include "hc/platform/crypto.h"
#include "hc/platform/web_host.h"
#include "hc/web_api.h"
#include "loop_queue.h"
#include "net_services.h"
#include "ota_service.h"

namespace hc {

class Mqtt;
namespace esp {

/// Hardware random numbers (esp_fill_random, with the RF subsystem as entropy source once WiFi
/// runs) and PBKDF2 through mbedTLS (the SHA accelerator).
class EspCrypto : public Crypto {
public:
	bool random(uint8_t* out, size_t length) override;
	void pbkdf2_sha256(const uint8_t* password, size_t password_length, const uint8_t* salt, size_t salt_length,
		uint32_t iterations, uint8_t* out, size_t out_length) override;
};

/// The web server of the device (doc/web.md, spec/api.md): esp_http_server on port 80 with
///
/// - /api/* : the core's WebApi. The httpd task copies the request into a static slot and runs
///   WebApi::handle() on the event loop (LoopQueue, waits up to kJobTimeoutMs); the password hash
///   of a login or onboarding is computed in the httpd task between the two passes. A body above
///   WebApi::kMaxBody (PUT /api/image, up to WebApi::body_limit()) goes to a heap buffer of its
///   length, freed after the response (507 when the heap has no block that large). A streamed
///   response (GET /api/image) is sent chunked, each piece fetched by a job of its own.
/// - /ws : the WebSocket of the core's LiveFeed. Incoming messages go to the loop the same way;
///   outgoing messages are copied into a pool of kSendBuffers buffers and sent by work queued to
///   the httpd task (httpd_queue_work), so the loop never blocks on a socket.
/// - everything else: the UI files from the LittleFS image (/storage/ui, gzip-compressed, ETag =
///   the image's content hash), extensionless paths fall back to index.html.
///
/// Also the `web` console command (tokens, password reset, demo datapoints, status).
class WebServer : public WebHost, public WsTransport {
public:
	static constexpr uint16_t kPort = 80;
	/// Sockets of the server: 4 WebSocket clients, a few page loads; the least recently used
	/// connection is purged when all are taken.
	static constexpr uint16_t kMaxSockets = 10;
	static constexpr uint32_t kStack = 6144;
	static constexpr uint32_t kJobTimeoutMs = 5000;
	static constexpr size_t kSendBuffers = 8;
	static constexpr size_t kResponseBuffer = 8192;
	/// Largest WebSocket message accepted from a client.
	static constexpr size_t kMaxIncoming = 1024;
	/// WebSocket connections tracked by the server (4 feed clients plus the ones being refused).
	static constexpr size_t kMaxWs = 8;
	static constexpr const char* kMountPoint = "/storage";
	static constexpr const char* kPartitionLabel = "storage";
	static constexpr const char* kUiDir = "/storage/ui";

	WebServer(Controller& controller, DatapointModel& model, LoopQueue& queue, NetServices& services,
		OtaService& ota, const char* device_name);

	/// Mounts the UI image, reads the stored password and tokens and starts the server. Call on the
	/// event loop once the network stack is initialized (after WifiDriver::init). Logs the free heap
	/// before and after. Returns false (logged) if the server cannot start.
	bool start();

	/// The runtime image behind /api/image and the status (call before start()).
	void set_image(image::ImageService* image) { api_.set_image(image); }
	/// The language packs (doc/i18n.md): ui.lang changes start their sync, /api/settings reports them;
	/// their downloaded files are served at /i18n/<lang>.json (call before start()).
	void set_language_packs(LanguagePacks* packs) { api_.set_language_packs(packs); }
	/// The MQTT client the settings mqtt.user and mqtt.password reconnect (call before start()).
	void set_mqtt(Mqtt* mqtt) { api_.set_mqtt(mqtt); }
	/// The KNX tunnel and its telegram log: the "knx" status, knx.gateway and knx.nat applied to it.
	void set_knx(knx::KnxTunnel* tunnel, const knx::TelegramLog* telegrams) { api_.set_knx(tunnel, telegrams); }
	/// The Zigbee coordinator behind /api/zigbee; nullptr with the reason when it does not run.
	void set_zigbee(zigbee::ZigbeeAdapter* adapter, const char* reason) { api_.set_zigbee(adapter, reason); }
	/// The external values behind POST /api/external/<id>/fetch and the "external" status.
	void set_external(ExternalValues* external) { api_.set_external(external); }
	/// The blob store the factory reset empties and the "storage" status counts.
	void set_blob_store(BlobStore* blobs) {
		blobs_ = blobs;
		api_.set_blob_store(blobs);
	}
	/// The diagnostics whose core dump DELETE /api/coredump erases.
	void set_diagnostics(EspDiagnostics* diagnostics) { diagnostics_ = diagnostics; }
	/// The `web` console command (ConsoleContext::extensions).
	const ConsoleCommand* console_commands() const { return &console_command_; }
	size_t console_command_count() const { return 1; }

	// WebHost
	const char* device_name() const override { return device_name_; }
	bool mdns_announced() const override;
	const char* ntp_server() const override;
	void ntp_server_changed() override;
	bool set_update_url(const char* url, const char** error) override;
	void write_update_state(JsonWriter& json) const override;
	const char* ui_version() const override { return ui_version_; }
	uint16_t websocket_port() const override { return 0; }
	HostResult system_action_possible(SystemAction action, const char** reason) const override;
	void system_action(SystemAction action) override;
	HostResult start_update(bool force, const char* url, const char** reason) override;
	HostResult start_upload(const char* token, const char** reason) override;
	HostResult clear_coredump(const char** reason) override;
	bool set_platform_log_level(const char* tag, LogLevel level) override;
	bool has_platform_log() const override { return true; }
	bool storage_usage(uint64_t& total, uint64_t& used) const override;

	// WsTransport
	bool ws_send(uint32_t client, const char* text, size_t length) override;
	void ws_close(uint32_t client, uint16_t code, const char* reason) override;

private:
	struct Job;
	struct SendBuffer;

	static esp_err_t api_handler(httpd_req_t* req);
	static esp_err_t ws_handler(httpd_req_t* req);
	static esp_err_t file_handler(httpd_req_t* req);
	static void close_handler(httpd_handle_t hd, int fd);
	static void drain_work(void* arg);
	static void run_job(void* context, const LoopMessage& message);
	static void post_closed(void* context, const LoopMessage& message);
	static void post_writable(void* context, const LoopMessage& message);
	static int console_web(void* context, int argc, char** argv);

	esp_err_t handle_api(httpd_req_t* req);
	esp_err_t send_stream(httpd_req_t* req, size_t length, uint32_t token);
	void wait_job();
	esp_err_t handle_ws(httpd_req_t* req);
	esp_err_t handle_file(httpd_req_t* req);
	void handle_close(int fd);
	void drain();
	bool run_on_loop();
	bool mount_ui();
	int web_command(int argc, char** argv);

	Controller& controller_;
	DatapointModel& model_;
	LoopQueue& queue_;
	NetServices& services_;
	OtaService& ota_;
	const char* device_name_;
	EspCrypto crypto_;
	Auth auth_;
	WebApi api_;
	LiveFeed feed_;
	DemoTechnology demo_;
	httpd_handle_t server_ = nullptr;
	char ui_version_[24] = "";
	bool ui_mounted_ = false;
	BlobStore* blobs_ = nullptr;
	EspDiagnostics* diagnostics_ = nullptr;

	/// The one job in flight from the httpd task to the loop (the httpd task serves one request at
	/// a time). Stays busy after a timeout until the loop has run it.
	Job* job_;
	std::atomic<bool> job_busy_{false};
	SemaphoreHandle_t job_done_ = nullptr;
	StaticSemaphore_t job_done_storage_;

	/// WebSocket connections, httpd task only: fd and generation of each open one.
	struct WsSlot {
		int fd = -1;
		uint16_t generation = 0;
		bool opened = false; ///< LiveFeed::open() was called (closed() is owed).
		bool dead = false;   ///< Being closed: nothing more is sent or delivered.
	};
	WsSlot ws_[kMaxWs];
	uint16_t next_generation_ = 1;
	WsSlot* ws_find(int fd);

	/// The send pool: indices of free buffers and of buffers to send (FIFO).
	SendBuffer* buffers_;
	QueueHandle_t free_ = nullptr;
	QueueHandle_t outgoing_ = nullptr;
	StaticQueue_t free_control_;
	StaticQueue_t outgoing_control_;
	uint8_t free_storage_[kSendBuffers];
	uint8_t outgoing_storage_[kSendBuffers];
	std::atomic<bool> drain_queued_{false};
	std::atomic<bool> starved_{false};

	ConsoleCommand console_command_;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_WEB_SERVER_H
