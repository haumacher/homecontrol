/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_IMAGE_IMAGE_SERVICE_H
#define HC_IMAGE_IMAGE_SERVICE_H

#include <cstddef>
#include <cstdint>

#include "hc/controller.h"
#include "hc/datapoint.h"
#include "hc/event_loop.h"
#include "hc/external_values.h"
#include "hc/history.h"
#include "hc/image/image.h"
#include "hc/image/image_store.h"
#include "hc/image/loader.h"
#include "hc/image/model_view.h"
#include "hc/image/rules_clock.h"
#include "hc/json.h"
#include "hc/knx/adapter.h"
#include "hc/knx/time_master.h"
#include "hc/knx/tunnel.h"
#include "hc/platform/blob_store.h"
#include "hc/rules/engine.h"
#include "hc/scheduler.h"
#include "hc/value_store.h"

namespace hc {
namespace image {

/// The runtime image on the device (doc/image.md): validates an image, swaps it in atomically on
/// the event loop, stores it in the BlobStore and loads the stored one at start. It owns the
/// components an image configures beyond the datapoint model and the KNX adapter: the scheduler,
/// the rule engine (with its time source), the value store, the history and the KNX time master.
///
/// A load validates everything first (Loader: every refusal with its path and reason); a refused
/// image changes nothing. Then, on the loop, in one call:
///
///  1. the rules' queued events are processed (they carry the old datapoint ids);
///  2. the datapoint model is replaced, keeping value, quality and update time of every datapoint
///     whose id and type are unchanged (ticket #26);
///  3. the image's settings (tz, location, ntp.server) are written where they differ;
///  4. the value store and the history load (restores, orphan records removed);
///  5. the rule engine loads the new rules (rules kept by id keep their state, timers, overrides);
///  6. the KNX adapter gets the image's mapping, the time master its configuration, the adapter
///     the merged mapping (doc/knx.md "Time master");
///  7. the scheduler loads the entries (persisted state kept by id);
///  8. the rules start (reason `boot` for the first image since start, `image` after a swap).
///
/// The validation covers what these steps check, so they do not fail for the image's content;
/// they can still fail for memory. A failure from step 2 on leaves no old image to return to in
/// RAM: the service tears the new one down and loads the stored (previous) image again; without
/// one the device has no image. The text is stored only after a successful load.
///
/// Threading: everything on the event loop's thread.
class ImageService : public LedInput, private ScheduleListener {
public:
	/// `tunnel` (may be null) tells the LED whether KNX is up; `adapter` is the platform's KNX adapter
	/// (also when no gateway is configured).
	ImageService(Controller& controller, DatapointModel& model, BlobStore& blobs, knx::KnxAdapter& adapter,
		const knx::KnxTunnel* tunnel, const HistoryLimits& history_limits = HistoryLimits(),
		const ValueStoreLimits& value_limits = ValueStoreLimits());
	~ImageService();

	ImageService(const ImageService&) = delete;
	ImageService& operator=(const ImageService&) = delete;

	/// Registers with the time service and the controller's LED, removes stray image chunks and
	/// loads the stored image (Source::boot). Call once, after Controller::start().
	void start();

	/// Validates and applies `text` (not NUL-terminated; it may be freed after the call). With
	/// `store` the text is stored when the load succeeded (stored() and store_error() tell whether
	/// that worked). False when it was refused or failed: report() says why.
	bool load(const char* text, size_t length, Source source, bool store);

	/// Loads the stored image (the boot load). False without one or when it fails (logged).
	bool load_stored(Source source);

	/// Removes the image: no datapoints, no rules, no schedules; the persisted values, rings,
	/// schedule records and parameter overrides of the image are deleted, the stored text removed.
	bool unload(const char** error);

	/// Writes what the value store and the history hold pending (before a reboot).
	void flush();

	bool loaded() const { return loaded_; }
	const ImageInfo& info() const { return info_; }
	Source source() const { return source_; }
	/// The report of the last load that failed (cleared by a successful one).
	const Report& report() const { return report_; }
	/// Whether the last successful load was stored; the reason when not.
	bool stored_ok() const { return stored_ok_; }
	const char* store_error() const { return store_error_; }

	/// For GET /api/image: the stored text's size, a token that changes with it, and its bytes.
	bool stored(size_t& bytes) const { return store_.stored(bytes); }
	uint32_t store_token() const { return store_.token(); }
	bool read_stored(uint32_t token, size_t offset, char* buffer, size_t size, size_t& length) const {
		return store_.read_part(token, offset, buffer, size, length);
	}

	/// The members of the "image" object of /api/status.
	void write_status(JsonWriter& json) const;
	/// The answer to a successful PUT: the metadata and the counts.
	void write_info(JsonWriter& json) const;
	/// The answer to a refused PUT: {"error", "code", "refusals": [{"path", "reason"}], "total"}.
	void write_report(JsonWriter& json) const;

	/// Called when the image changed ntp.server (the platform restarts SNTP).
	/// The external values (weather and other HTTP sources, doc/external.md) the image's `external`
	/// member configures after every load and clears on unload; nullptr: the member is ignored.
	void set_external(ExternalValues* external) { external_ = external; }

	void set_ntp_changed(Callback callback) { ntp_changed_ = callback; }
	/// Called after every change of the loaded image (load, swap, restore, unload): the model's ids
	/// may have changed (the web API resyncs its live feed).
	void set_changed(Callback callback) { changed_ = callback; }

	// LedInput
	bool image_loaded() const override { return loaded_; }
	bool image_degraded() const override;

	Scheduler& scheduler() { return scheduler_; }
	rules::Engine& engine() { return engine_; }
	ValueStore& value_store() { return value_store_; }
	History& history() { return history_; }
	knx::TimeMaster& time_master() { return time_master_; }

	/// What the UI needs of the loaded image beyond the components (GET /api/model); built after
	/// every successful load, empty without an image.
	const ModelView& view() const { return view_; }
	/// When the loaded image was applied (monotonic ms; with source() and info(): "deployed when and how").
	uint64_t loaded_ms() const { return loaded_ms_; }

private:
	bool apply(Plan& plan, Source source);
	bool fail_applied(Source source, const char* path, const char* format, ...) HC_PRINTF_FORMAT(4, 5);
	void tear_down();
	void apply_settings(const Plan& plan);

	// ScheduleListener
	void schedule_fired(const char* id) override;

	Controller& controller_;
	DatapointModel& model_;
	knx::KnxAdapter& adapter_;
	const knx::KnxTunnel* tunnel_;
	HistoryLimits history_limits_;
	RulesClock rules_clock_;
	Scheduler scheduler_;
	rules::Engine engine_;
	ValueStore value_store_;
	History history_;
	knx::TimeMaster time_master_;
	ImageStore store_;

	ImageInfo info_;
	Report report_;
	Source source_ = Source::boot;
	bool loaded_ = false;
	bool has_knx_ = false;
	bool rules_started_ = false;
	bool started_ = false;
	bool restoring_ = false;
	bool stored_ok_ = false;
	char store_error_[64] = {};
	uint64_t loaded_ms_ = 0;
	ExternalValues* external_ = nullptr;
	ModelView view_;
	Callback ntp_changed_;
	Callback changed_;
};

} // namespace image
} // namespace hc

#endif // HC_IMAGE_IMAGE_SERVICE_H
