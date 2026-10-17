/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_LANGUAGE_PACKS_H
#define HC_LANGUAGE_PACKS_H

#include <cstddef>
#include <cstdint>

#include "hc/backoff.h"
#include "hc/connectivity.h"
#include "hc/event_loop.h"
#include "hc/json.h"
#include "hc/log.h"
#include "hc/network_settings.h"
#include "hc/platform/blob_store.h"
#include "hc/platform/http_fetcher.h"
#include "hc/platform/settings.h"
#include "hc/time_service.h"
#include "hc/update_check.h"

namespace hc {

/// One language the release's i18n manifest offers.
struct LanguageOffer {
	char code[setting::kMaxUiLangLength + 1] = {};
	/// The language's own name ("Deutsch"), as the manifest gives it.
	char name[24] = {};
};

/// What the console and the web UI show about the language packs.
enum class LanguagePackState : uint8_t {
	builtin,     ///< ui.lang is the base language (English): nothing to download.
	waiting,     ///< A sync is due but a condition is missing (detail: which).
	fetching,    ///< The manifest or the pack is being fetched.
	installed,   ///< The release's pack for ui.lang is stored on the device.
	not_offered, ///< The release offers no pack for ui.lang (the UI's bundled pack, if any, is used).
	failed,      ///< The last pass failed (detail: why); retried after a backoff.
};

/// "builtin", "waiting", "fetching", "installed", "not_offered", "failed".
const char* language_pack_state_name(LanguagePackState state);

/// The language packs that ship after the firmware (issue #24, doc/i18n.md): the phoneblock
/// dongle's approach. Each release publishes its own i18n bundle next to its firmware on the update
/// server, `<update.url>/<version>/i18n/manifest.json`:
///
///     {"version": "0.2.0",
///      "packs": {"de": {"name": "Deutsch", "path": "lang-de.json.gz", "sha256": "<64 hex>",
///                       "size": 5321}}}
///
/// `version` must be the running firmware's (without build metadata), `path` a plain file name in
/// the same directory, `size` at most kMaxPack. The pack is the gzip-compressed key/string JSON the
/// browser loads; the device stores it as the blob `i18n.<lang>.json.gz` (blob_name()) and the web
/// server serves it at `/i18n/<lang>.json` with Content-Encoding: gzip. The manifest is not signed:
/// the strings are display text only, the SHA-256 protects against a truncated or corrupted
/// transfer, HTTPS with the certificate bundle is the transport trust, and the UI treats every pack
/// string as untrusted text (never as HTML).
///
/// The core decides when: a sync pass runs kStartDelayMs after start() (every boot: the manifest
/// is per version, so an OTA update brings the new release's pack), and kChangeDelayMs after
/// language_changed() (ui.lang changed). A pass needs the network up, the wall clock synchronized
/// (TLS checks certificate dates) and an update source (update.url); while one is missing the pass
/// waits, polling every kReadyPollMs, and the state names the reason. A pass fetches the manifest,
/// records the offered languages, and then:
///
/// - ui.lang is English (the base, built into the UI): nothing to download; stored packs are
///   removed;
/// - the manifest offers no valid pack for ui.lang: not_offered, stored packs are removed (a pack
///   of an older release must not shadow the new UI's bundled one);
/// - the stored pack has the manifest's SHA-256: installed, nothing fetched;
/// - otherwise the pack is fetched, its size and SHA-256 checked against the manifest (a mismatch
///   is refused and the stored pack kept), stored, and every other pack removed.
///
/// A failure is logged with its reason and retried after a Backoff of kRetryInitialMs doubling up
/// to kRetryCapMs; a success ends the pass (no periodic re-sync: a release's bundle changes only
/// with a new firmware version). Manifest entries that are malformed (a code that is not a
/// language tag, a path with a directory, a bad checksum or size) are refused one by one with a
/// log line; the others stay usable.
///
/// Memory: fixed, about 0.7 KB plus the manifest text the fetcher delivers. Threading: the event
/// loop's thread.
class LanguagePacks : public FetchReceiver {
public:
	/// The longest manifest accepted.
	static constexpr size_t kMaxManifest = 4096;
	/// The largest pack accepted (gzip-compressed bytes): the device's fetch buffer, a BlobStore blob.
	static constexpr size_t kMaxPack = 8192;
	/// Languages remembered from the manifest.
	static constexpr size_t kMaxOffers = 16;
	/// The blob name prefix of the packs.
	static constexpr const char* kBlobPrefix = "i18n.";

	/// First pass after start() (the network and SNTP come up meanwhile).
	static constexpr uint32_t kStartDelayMs = 20000;
	/// Pass after language_changed().
	static constexpr uint32_t kChangeDelayMs = 500;
	/// How often the conditions are checked while a pass waits for them (or for a busy fetcher).
	static constexpr uint32_t kReadyPollMs = 30000;
	/// Retry delays after a failure: 1, 2, 4, ... minutes up to an hour.
	static constexpr uint32_t kRetryInitialMs = 60000;
	static constexpr uint32_t kRetryCapMs = 3600000;
	/// A fetch without a result after this long counts as failed (guards against a lost result).
	static constexpr uint32_t kFetchTimeoutMs = 120000;

	/// `version` is the running firmware's version (hc::kVersion); build metadata is dropped.
	LanguagePacks(EventLoop& loop, const TimeSource& time, const Settings& settings, const Connectivity& connectivity,
		HttpFetcher& fetcher, BlobStore& store, const char* version);
	~LanguagePacks();

	LanguagePacks(const LanguagePacks&) = delete;
	LanguagePacks& operator=(const LanguagePacks&) = delete;

	/// Schedules the boot pass.
	void start();

	/// ui.lang changed: a pass follows shortly (a fetch in flight is discarded when it returns).
	void language_changed();

	/// Starts a pass now (the console's `lang sync`). Refused (false, `error`) while one is running.
	bool sync_now(const char** error);

	/// `i18n.<lang>.json.gz`; false if `lang` is not a valid tag or the name does not fit.
	static bool blob_name(const char* lang, char* out, size_t size);

	/// The manifest URL for the update source `base` and the release `version`:
	/// `<base>/<version without build>/i18n/manifest.json`. False if it does not fit.
	static bool manifest_url(const char* base, const char* version, char* out, size_t size);

	LanguagePackState state() const { return state_; }
	/// The reason of `waiting` and `failed`, a short note otherwise ("" if none).
	const char* detail() const { return detail_; }
	/// The language of the last pass.
	const char* lang() const { return lang_; }
	/// The release directory the device asks for ("0.2.0").
	const char* release() const { return release_; }
	/// Whether a pass is due or running.
	bool pending() const { return pending_ || phase_ != Phase::idle; }
	/// Languages the last manifest offered.
	size_t offer_count() const { return offer_count_; }
	const LanguageOffer& offer(size_t index) const { return offers_[index]; }

	/// The members of the "i18n" object of GET /api/settings (spec/api.md) into an open object:
	/// "lang", "state", "detail", "release", "offered" [{"code", "name"}].
	void write_json(JsonWriter& json) const;

	// FetchReceiver
	void fetch_done(const FetchResult& result) override;

private:
	enum class Phase : uint8_t { idle, manifest, pack };

	struct Entry {
		char code[setting::kMaxUiLangLength + 1];
		char name[24];
		char path[64];
		char sha256[65];
		uint32_t size;
	};

	void arm(uint64_t at);
	void timer_due();
	void run_pass();
	const char* blocked() const;
	bool begin_fetch(const char* url, size_t max_bytes);
	void manifest_arrived(const char* body, size_t length);
	void pack_arrived(const char* body, size_t length);
	bool parse_manifest(const char* body, size_t length, Entry& wanted, bool& found, const char** error);
	bool stored_sha256(const char* name, char (&hex)[65]) const;
	void prune(const char* keep);
	void succeed(LanguagePackState state, const char* fmt, ...) HC_PRINTF_FORMAT(3, 4);
	void fail(const char* fmt, ...) HC_PRINTF_FORMAT(2, 3);
	void wait(const char* reason);

	EventLoop& loop_;
	const TimeSource& time_;
	const Settings& settings_;
	const Connectivity& connectivity_;
	HttpFetcher& fetcher_;
	BlobStore& store_;
	Backoff backoff_;

	TimerHandle timer_;
	bool pending_ = false;
	/// ui.lang changed while a fetch was in flight: its result is discarded.
	bool restart_ = false;
	/// The manifest asked for the pack: fetched on the next timer (the fetcher is still busy
	/// delivering the manifest).
	bool pack_due_ = false;
	/// The stored packs were removed for the base language (once per language, not every poll).
	bool base_pruned_ = false;
	Phase phase_ = Phase::idle;
	uint64_t deadline_ = 0;
	LanguagePackState state_ = LanguagePackState::waiting;
	char detail_[160] = {};
	char lang_[setting::kMaxUiLangLength + 1] = {};
	char release_[Version::kMaxText + 1] = {};
	/// `<update.url>/<release>/i18n/`
	char base_[HttpFetcher::kMaxUrl + 1] = {};
	char url_[HttpFetcher::kMaxUrl + 1] = {};
	Entry wanted_ = {};
	LanguageOffer offers_[kMaxOffers];
	size_t offer_count_ = 0;
};

} // namespace hc

#endif // HC_LANGUAGE_PACKS_H
