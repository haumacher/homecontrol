/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_OTA_SERVICE_H
#define HC_ESP_OTA_SERVICE_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hc/controller.h"
#include "hc/platform/http_fetcher.h"
#include "hc/update_check.h"
#include "loop_queue.h"

namespace hc {
namespace esp {

/// The application's part of the OTA update with asymmetric partitions (doc/ota.md):
///
/// - Confirms a new image (esp_ota_mark_app_valid_cancel_rollback) once the network is up, or
///   kConfirmAfterMs after start without it; until then the bootloader would fall back to the
///   updater at the next reset. After the confirmation it reports and erases a leftover update
///   instruction (the update completed, or the updater gave up and booted this image).
/// - Checks the update source (setting update.url) kFirstCheckMs after start and every
///   kCheckPeriodMs, and on request (console `update check`, `update now`): fetches
///   `<update.url>/manifest.json` and its signature `manifest.sig` on its lane of the one fetch
///   worker (set_fetcher(); FetchService, issue #45 item 13: second in line after the console's image
///   download), refuses a
///   manifest the release key did not sign (hc::verify_manifest_signature), decides with
///   hc::check_update(), and for an applicable update stores the instruction for the updater, makes
///   the updater (the factory partition) the boot partition and restarts after kRestartGraceMs.
///
/// Everything except the HTTP fetch runs on the event loop.
class OtaService : public ConnectivityListener, private FetchReceiver {
public:
	static constexpr uint32_t kConfirmAfterMs = 60000;
	static constexpr uint32_t kFirstCheckMs = 15u * 60u * 1000u;
	static constexpr uint32_t kCheckPeriodMs = 24u * 60u * 60u * 1000u;
	static constexpr uint32_t kRestartGraceMs = 2000;

	explicit OtaService(LoopQueue& queue);
	/// The fetcher of the update check (the `update` lane of the FetchService; before init()).
	void set_fetcher(HttpFetcher* fetcher) { fetcher_ = fetcher; }

	/// Registers with the controller's connectivity and loop, logs the partition the application
	/// runs from. Call on the event loop before Controller::start().
	bool init(Controller& controller);

	void connectivity_changed(const Connectivity& connectivity, LinkState previous) override;

	/// Starts a check against `base` (nullptr: the setting update.url). With `force` the
	/// manifest's image is installed even if it is not newer. Returns false with the reason if the
	/// check cannot start (no URL, network down, a check running, not running from ota_0); the
	/// outcome is logged.
	bool check(bool force, const char* base, const char** error);

	/// Prepares a firmware upload from the browser (doc/ota.md "Upload from the browser"): refuses
	/// (false with the reason) when the application does not run from ota_0, an update is under
	/// way, or the updater in the factory partition predates uploads; otherwise stores the upload
	/// instruction with `token` and makes the updater the boot partition. The caller (the web API)
	/// restarts the device after its answer.
	bool start_upload(const char* token, const char** error);

	/// Validates and stores update.url (nullptr or "" erases it: checks disabled).
	bool set_url(const char* url, const char** error);

	/// Prints the update state to stdout (console `update status`).
	void print_status() const;

	// The update state for the web API's status (doc/web.md); event loop only.
	bool confirmed() const { return confirmed_; }
	bool checking() const { return busy_.load(); }
	/// The outcome of the last check ("none since start" before the first), and when (0: never).
	const char* last_result() const { return last_; }
	uint64_t last_result_ms() const { return last_at_ms_; }
	/// When the next periodic check is due (monotonic ms; meaningful while update.url is set).
	uint64_t next_check_ms() const { return next_check_ms_; }

private:
	// FetchReceiver: the manifest, then its signature.
	bool fetch_begin(int http_status, size_t content_length) override;
	bool fetch_chunk(const char* data, size_t size) override;
	void fetch_end(const FetchResult& result) override;
	void periodic_check();
	void confirm();
	void confirm_timeout();
	void report_upload(Settings& settings);
	void finish_check();
	void begin_update(const UpdateManifest& manifest, const char* image_url);
	void restart_into_updater();
	void set_last(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

	LoopQueue& queue_;
	Controller* controller_ = nullptr;
	bool from_ota0_ = false;
	bool confirmed_ = false;
	bool restart_pending_ = false;

	/// The check in flight (both documents).
	std::atomic<bool> busy_{false};
	HttpFetcher* fetcher_ = nullptr;
	enum class Stage : uint8_t { manifest, signature };
	Stage stage_ = Stage::manifest;
	int status_ = 0;    ///< The HTTP status of the document being fetched (0: none yet).
	bool full_ = false; ///< The document filled its buffer (the rest is cut).
	bool force_ = false;
	char base_[Settings::kMaxStringLength + 1] = {};
	char manifest_url_[Settings::kMaxStringLength + 16] = {};
	char response_[UpdateManifest::kMaxLength + 1] = {};
	size_t response_length_ = 0;
	/// `<base>/manifest.sig` and what it held (doc/ota.md "Manifest signature").
	char signature_url_[Settings::kMaxStringLength + 16] = {};
	char signature_[kMaxManifestSignature] = {};
	size_t signature_length_ = 0;
	bool signature_missing_ = false; ///< 404: the manifest is not signed
	char fetch_error_[96] = {};

	char last_[160] = "none since start";
	uint64_t last_at_ms_ = 0;
	uint64_t next_check_ms_ = 0;
};

} // namespace esp
} // namespace hc

#endif // HC_ESP_OTA_SERVICE_H
