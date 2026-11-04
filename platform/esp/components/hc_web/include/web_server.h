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
#include "hc/ws_send_ring.h"
#include "loop_queue.h"
#include "net_services.h"
#include "ota_service.h"

namespace hc {

class Mqtt;
namespace esp {
namespace ui {
struct UiFile;
} // namespace ui

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
///   outgoing messages are formatted into (or copied into) one send ring of kSendRingBytes
///   (hc::WsSendRing: whole messages of variable length, one entry per message for all its
///   clients) and sent by work queued to the httpd task (httpd_queue_work), so the loop never
///   blocks on a socket.
/// - everything else: the UI files, compiled into the firmware (ui_files.h, gzip-compressed, ETag =
///   the UI's content hash), extensionless paths fall back to index.html.
///
/// Also the `web` console command (tokens, password reset, demo datapoints, status).
class WebServer : public WebHost, public WsTransport {
public:
	static constexpr uint16_t kPort = 80;
	/// Sockets of the server: the 4 WebSocket clients of the live feed (LiveFeed::kMaxClients) and 6
	/// for page loads and API requests (a browser opens up to 6 at once); the least recently used
	/// connection is purged when all are taken. E-Home does not take one: the controller connects out
	/// to it (hc_ehome). Issue #45 tried 7: six parallel loads then purged three of four live-feed
	/// sockets on the C6 (2026-11-03), so it stays 10.
	static constexpr uint16_t kMaxSockets = 10;
	/// The httpd task's stack: request parsing and what runs there (the UI files sent from flash in
	/// chunks, the body received into the job slot and handed to the loop, the WebSocket frames
	/// and the queued sends; the API itself, the password check included, runs on the loop).
	/// Measured on the C6: 2052 B used (2026-11-03), 2692 B (2026-10-18, with page loads and a
	/// login); 4608 B keep 1.9 KB over that. 6144 B before issue #45.
	static constexpr uint32_t kStack = 4608;
	static constexpr uint32_t kJobTimeoutMs = 5000;
	/// The WebSocket send ring (issue #45 item 7: it replaced a pool of 8 buffers of 1408 bytes,
	/// 11 264 B): at least two messages of the largest size, many small ones.
	static constexpr size_t kSendRingBytes = 4096;
	static constexpr size_t kResponseBuffer = 8192;
	/// Largest WebSocket message accepted from a client.
	static constexpr size_t kMaxIncoming = 1024;
	/// WebSocket connections tracked by the server (4 feed clients plus the ones being refused).
	static constexpr size_t kMaxWs = 8;
	static constexpr const char* kMountPoint = "/storage";
	static constexpr const char* kPartitionLabel = "storage";
	/// Where the storage partition held the UI before it came with the firmware; removed at start.
	static constexpr const char* kLegacyUiDir = "/storage/ui";

	WebServer(Controller& controller, DatapointModel& model, LoopQueue& queue, NetServices& services,
		OtaService& ota, const char* device_name);

	/// Removes an old UI image from the storage partition, reads the stored password and tokens and starts the server. Call on the
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
	/// The blob store the factory reset empties.
	void set_blob_store(BlobStore* blobs) { blobs_ = blobs; }
	/// The storage figures of the "storage" status and the image's storable (homecontrol #74).
	void set_storage_stats(StorageStats* stats) { api_.set_storage_stats(stats); }
	/// The diagnostics whose core dump DELETE /api/coredump erases.
	void set_diagnostics(EspDiagnostics* diagnostics) { diagnostics_ = diagnostics; }
	/// The JSON API and the live feed, for the E-Home connection (doc/ehome.md; on the loop).
	WebApi& api() { return api_; }
	LiveFeed& feed() { return feed_; }
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
	const char* ui_version() const override;
	uint16_t websocket_port() const override { return 0; }
	HostResult system_action_possible(SystemAction action, const char** reason) const override;
	void system_action(SystemAction action) override;
	HostResult start_update(bool force, const char* url, const char** reason) override;
	HostResult start_upload(const char* token, const char** reason) override;
	HostResult clear_coredump(const char** reason) override;
	bool set_platform_log_level(const char* tag, LogLevel level) override;
	bool has_platform_log() const override { return true; }

	// WsTransport
	bool ws_send(uint32_t client, const char* text, size_t length) override;
	bool ws_send_bulk(uint32_t client, const char* text, size_t length) override;
	void ws_close(uint32_t client, uint16_t code, const char* reason) override;
	char* ws_claim(bool bulk) override;
	void ws_commit(const uint32_t* clients, size_t count, size_t length) override;
	void ws_abandon() override { ring_.abandon(); }

private:
	struct Job;

	static esp_err_t api_handler(httpd_req_t* req);
	static esp_err_t ws_handler(httpd_req_t* req);
	static esp_err_t ws_opened(httpd_req_t* req);
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
	esp_err_t handle_ws_open(httpd_req_t* req);
	esp_err_t handle_file(httpd_req_t* req);
	void handle_close(int fd);
	void drain();
	/// Starts a drain on the httpd task unless one is queued; `refused`: a send did not fit, the
	/// drain posts writable() afterwards.
	void kick(bool refused);
	bool run_on_loop();
	/// Removes the UI directory an older firmware left in the storage partition (kLegacyUiDir).
	void remove_legacy_ui();
	/// The UI file of the path ("/index.html"), nullptr if the UI has none.
	static const ui::UiFile* find_ui_file(const char* path);
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

	/// The send ring (writer: the loop, reader: the httpd task's drain).
	WsSendRing ring_;
	std::atomic<bool> drain_queued_{false};
	std::atomic<bool> starved_{false};

	ConsoleCommand console_command_;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_WEB_SERVER_H
