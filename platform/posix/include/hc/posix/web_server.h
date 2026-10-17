/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_POSIX_WEB_SERVER_H
#define HC_POSIX_WEB_SERVER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "hc/controller.h"
#include "hc/live_feed.h"
#include "hc/platform/crypto.h"
#include "hc/platform/web_host.h"
#include "hc/web_api.h"

namespace hc {

class BlobStore;
class LanguagePacks;
namespace posix {

/// Secure random bytes from getrandom() and the core's software PBKDF2.
class PosixCrypto : public Crypto {
public:
	bool random(uint8_t* out, size_t length) override;
	void pbkdf2_sha256(const uint8_t* password, size_t password_length, const uint8_t* salt, size_t salt_length,
		uint32_t iterations, uint8_t* out, size_t out_length) override;
};

/// Brings work from the server threads onto the event loop's thread (the core is single-threaded).
/// The main loop sleeps in wait() instead of nanosleep, so a job wakes it at once; run() blocks the
/// calling server thread until the loop has executed the job.
class LoopExecutor {
public:
	LoopExecutor();
	~LoopExecutor();
	LoopExecutor(const LoopExecutor&) = delete;
	LoopExecutor& operator=(const LoopExecutor&) = delete;

	/// Server threads: runs `job` on the loop thread and waits for it. False if the executor
	/// stopped (the job did not run).
	bool run(const std::function<void()>& job);

	/// Loop thread: waits up to `ms` for jobs, then runs the waiting ones.
	void wait(uint64_t ms);

	/// Loop thread: a descriptor that becomes readable when a job is waiting, so a main loop that
	/// sleeps in poll() over several descriptors (the KNX socket, …) wakes for jobs too. After
	/// poll() reports it readable, call run_pending().
	int wake_fd() const { return wake_pipe_[0]; }

	/// Loop thread: runs the waiting jobs (and clears the wake descriptor) without sleeping.
	void run_pending();

	/// Refuses further jobs and wakes everybody (at shutdown).
	void stop();

private:
	void signal_wake();
	void run_jobs(std::unique_lock<std::mutex>& lock);

	int wake_pipe_[2] = {-1, -1};
	struct Job {
		const std::function<void()>* fn;
		bool done = false;
	};

	std::mutex mutex_;
	std::condition_variable wake_;
	std::condition_variable done_;
	std::deque<Job*> jobs_;
	bool stopped_ = false;
};

/// The POSIX side of the web server (doc/web.md): cpp-httplib serves the JSON API (hc::WebApi) and
/// the UI files of `www_dir` (web/ui/dist, uncompressed, single-page fallback to index.html) on
/// `port`; a small listener on `port + 1` speaks WebSocket (/ws) with the core's codec
/// (hc::websocket) and feeds hc::LiveFeed. cpp-httplib has a WebSocket of its own, but it does not
/// hand over the raw socket, and the core codec is the one under test.
class WebServer : public WebHost, public WsTransport {
public:
	WebServer(Controller& controller, DatapointModel& model, LoopExecutor& executor, uint16_t port, std::string www_dir);
	~WebServer();

	/// Call on the loop thread before the loop runs: starts the authentication, the feed and the
	/// listener threads. Logs and returns false if a port cannot be bound.
	bool start();

	/// Stops the listeners and closes every connection.
	void stop();

	Auth& auth() { return auth_; }
	/// The runtime image behind /api/image and the status (call before start()).
	void set_image(image::ImageService* image) { api_.set_image(image); }
	/// The language packs behind ui.lang's sync and GET /i18n/<lang>.json, which serves the pack
	/// stored in `store` (call before start()).
	void set_language_packs(LanguagePacks* packs, const BlobStore* store) {
		api_.set_language_packs(packs);
		packs_store_ = store;
	}

	// WebHost (loop thread)
	const char* device_name() const override { return device_name_; }
	bool mdns_announced() const override { return false; }
	const char* ntp_server() const override { return ""; }
	void ntp_server_changed() override;
	bool set_update_url(const char* url, const char** error) override;
	void write_update_state(JsonWriter& json) const override;
	const char* ui_version() const override { return ui_version_; }
	uint16_t websocket_port() const override { return static_cast<uint16_t>(port_ + 1); }

	// WsTransport (loop thread)
	bool ws_send(uint32_t client, const char* text, size_t length) override;
	void ws_close(uint32_t client, uint16_t code, const char* reason) override;

private:
	struct Connection;

	void http_thread();
	void ws_accept_thread();
	void ws_connection_thread(std::shared_ptr<Connection> connection);
	std::shared_ptr<Connection> find(uint32_t id);
	bool send_frame(Connection& connection, uint8_t opcode, const uint8_t* payload, size_t length);

	PosixCrypto crypto_;
	Controller& controller_;
	LoopExecutor& executor_;
	Auth auth_;
	WebApi api_;
	LiveFeed feed_;
	uint16_t port_;
	std::string www_dir_;
	const BlobStore* packs_store_ = nullptr;
	char device_name_[64] = {};
	char ui_version_[24] = {};

	struct Impl;
	std::unique_ptr<Impl> impl_;
	std::thread http_thread_;
	std::thread ws_thread_;
	int ws_listener_ = -1;
	std::atomic<bool> stopping_{false};
	std::mutex connections_mutex_;
	std::map<uint32_t, std::shared_ptr<Connection>> connections_;
	uint32_t next_id_ = 1;
};

} // namespace posix
} // namespace hc

#endif // HC_POSIX_WEB_SERVER_H
