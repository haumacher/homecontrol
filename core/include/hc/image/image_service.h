/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_IMAGE_IMAGE_SERVICE_H
#define HC_IMAGE_IMAGE_SERVICE_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hc/blocks/block_runtime.h"
#include "hc/controller.h"
#include "hc/datapoint.h"
#include "hc/event_loop.h"
#include "hc/external_values.h"
#include "hc/history.h"
#include "hc/image/dynamic_specs.h"
#include "hc/image/image.h"
#include "hc/image/image_body.h"
#include "hc/image/image_store.h"
#include "hc/image/load_probe.h"
#include "hc/image/loader.h"
#include "hc/image/model_view.h"
#include "hc/image/rules_clock.h"
#include "hc/json.h"
#include "hc/knx/adapter.h"
#include "hc/knx/time_master.h"
#include "hc/knx/tunnel.h"
#include "hc/platform/blob_store.h"
#include "hc/platform/image_staging.h"
#include "hc/platform/worker.h"
#include "hc/rules/engine.h"
#include "hc/scheduler.h"
#include "hc/value_store.h"

namespace hc {

class Mqtt;
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
///  5. the rule engine loads the new rules (rules kept by id keep their state, timers, overrides),
///     then the block runtime the new blocks (records and overrides kept by block id, spec/blocks.md 8);
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
/// The store off the loop (homecontrol #68, doc/image.md "Storing"): writing the text to the flash
/// takes a few hundred milliseconds and needs no parsed state. With a Worker (set_worker()) and the
/// received body handed over (load()'s `holder`), the service keeps the body and the worker writes
/// it while the loop runs on; the image runs from the end of load(), the answer to the deployer
/// waits for the store (`stored` callback, store_answer()). Meanwhile a further load that would
/// store, and an unload, are refused as busy (nothing is looked at); a reload for the providers
/// that fails returns to the image being stored (its text is at hand). ImageStore writes the new
/// generation first and switches the manifest last, so a restart during the store loads the
/// previous image. Without a worker, a holder, or when the worker cannot start, the store runs
/// within load() as before.
///
/// Dynamic datapoints (doc/image.md "Dynamic datapoints"): registered DynamicSpecProviders (the
/// Zigbee adapter) add datapoints of their own. The model is the image's datapoints, at the
/// image's positions, followed by every provider's specs; a name that appears twice refuses the
/// composition (an image is refused, a reload keeps the old model), naming the provider. When a
/// provider's specs change, the model is reloaded with keep_values at most once per
/// kReloadSpacingMs, and every component is bound again as in a swap (KNX mapping, time master,
/// value store, history, schedules, external values, the providers); the rules continue: the
/// image's datapoints keep their ids, the providers' follow them. Without an image the model
/// holds the providers' datapoints alone. With a provider registered the service keeps the
/// applied plan's tables (the "load tables" of the log) for these reloads.
///
/// Threading: everything on the event loop's thread, except store_pending() (any thread) and the
/// store's job, which runs on the worker's thread and touches only the held body and the ImageStore.
class ImageService : public LedInput, private ScheduleListener, private QuietReports, private DynamicSpecListener {
public:
	/// Most providers registered at the same time.
	static constexpr size_t kMaxProviders = 4;
	/// Shortest time between two model reloads for provider changes (a joining device changes its
	/// specs several times within a second: they are coalesced into one reload).
	static constexpr uint32_t kReloadSpacingMs = 1000;

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
	///
	/// `signature` (64 bytes, or nullptr for none) is the image's detached Ed25519 signature (issue
	/// #35, spec/api.md "Signed runtime images"). While the device holds the installation key
	/// (setting ehome.key) every load but Source::boot and Source::restore (the stored image passed
	/// this check when it was stored) needs it: without one the image is refused as `unsigned_image`,
	/// with one that does not verify as `bad_signature`, both with the key's fingerprint in the
	/// reason, before anything is parsed or written. An image above kMaxImageBytes is refused as too
	/// large first. Without a key a signature is not checked (as before the key pairing).
	///
	/// `holder` (optional) is the ImageBody that holds `text` (its text()). With `store`, a holder and
	/// a worker, the service takes the holder over (ImageBody::take) and stores off the loop: load()
	/// returns true when the image runs, store_pending() is true, stored_ok() false with store_error()
	/// "being stored"; when the store has finished, on the loop, the body is released, the outcome is in
	/// stored_ok()/store_error() and store_answer(), and `stored` (if given) is called. Otherwise the
	/// holder stays with the caller and the store happens within the call. While a store is pending a
	/// load with `store` is refused at once (Report::Kind::busy), the running image stays.
	bool load(const char* text, size_t length, Source source, bool store, const uint8_t* signature = nullptr,
		ImageBody* holder = nullptr, Callback stored = Callback());

	/// Loads the stored image (the boot load, and the restore after a failed apply). Its text is put
	/// together in the stored staging (set_stored_staging()), otherwise in a heap buffer. False without
	/// one or when it fails (logged).
	bool load_stored(Source source);

	/// Removes the image: no datapoints, no rules, no schedules; the persisted values, rings,
	/// schedule records and parameter overrides of the image are deleted, the stored text removed.
	/// Refused (false, `error`) while a store is pending.
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

	/// The worker that stores off the loop (homecontrol #68; nullptr: within load()). The worker
	/// outlives the service, or has no job in flight when the service goes.
	void set_worker(Worker* worker) { worker_ = worker; }
	/// Whether a store runs off the loop. Atomic: a receiver on another thread may ask before it takes
	/// a body (WebApi::body_refusal()); the answer may be stale by then, load() decides.
	bool store_pending() const { return store_pending_.load(std::memory_order_acquire); }
	/// The ticket of the last store started off the loop (0: none since start); a deferred answer
	/// asks store_answer() with it.
	uint32_t store_ticket() const { return answer_.ticket; }

	/// The answer to a deploy whose store ran off the loop.
	enum class StoreAnswer : uint8_t {
		pending,    ///< The store of `ticket` runs: ask again.
		ready,      ///< Written: the deployed image's write_info() members, `stored` its outcome.
		superseded, ///< A later store began meanwhile: this deploy's outcome is gone (nothing written).
	};
	/// Writes the members of write_info() for the deploy of `ticket` (its image's metadata and counts
	/// as loaded, `stored` and `storeError` as the store ended) into the open object of `json`.
	StoreAnswer store_answer(uint32_t ticket, JsonWriter& json) const;

	/// For GET /api/image: the stored text's size, a token that changes with it, and its bytes.
	bool stored(size_t& bytes) const { return store_.stored(bytes); }
	uint32_t store_token() const { return store_.token(); }
	bool read_stored(uint32_t token, size_t offset, char* buffer, size_t size, size_t& length) const {
		return store_.read_part(token, offset, buffer, size, length);
	}

	/// The largest image the device can store and load again (homecontrol #52, spec/runtime-image.md
	/// 7): the least of what the blob store takes beside the stored image, keeping free the flash the
	/// history's archives still take as they fill (`storage`), and what the stored staging holds beside
	/// a failed image of the same size (`staging`, ImageStaging::shared_capacity() less the byte of the
	/// NUL a body adds, ImageBody::begin()). A part the device
	/// cannot tell (no free_space(), no staging or one that is off: the heap) is kMaxImageBytes.
	struct Storable {
		size_t bytes = 0;
		size_t storage = 0;
		size_t staging = 0;
	};
	Storable storable() const;

	/// The largest image the device can receive (homecontrol #52): with a staging area for received
	/// images its capacity less the byte of the NUL (a larger body would need a heap block of its size,
	/// which the C6 never has), otherwise kMaxImageBytes.
	size_t receivable() const;

	/// The members of the "image" object of /api/status; `full` adds those only GET /api/status carries
	/// (`storable`: it asks the file system for its free space, not for every WebSocket status).
	void write_status(JsonWriter& json, bool full = false) const;
	/// The answer to a successful PUT: the metadata and the counts.
	void write_info(JsonWriter& json) const;
	/// The answer to a refused PUT: {"error", "code", "refusals": [{"path", "reason"}], "total"}.
	void write_report(JsonWriter& json) const;

	/// Called when the image changed ntp.server (the platform restarts SNTP).
	/// The external values (weather and other HTTP sources, doc/external.md) the image's `external`
	/// member configures after every load and clears on unload; nullptr: the member is ignored.
	void set_external(ExternalValues* external) { external_ = external; }
	/// The MQTT client (doc/mqtt.md) the image's `mqtt` member configures after every load (an image
	/// without the member clears it), that follows the model's dynamic reloads and is cleared on
	/// unload; nullptr: the member is validated and otherwise ignored.
	void set_mqtt(Mqtt* mqtt) { mqtt_ = mqtt; }

	/// The platform's staging area for received images (homecontrol #66; none: heap buffers). The
	/// receivers of PUT /api/image put the body there (ImageBody), see WebApi::body_staging().
	void set_staging(ImageStaging* staging) { staging_ = staging; }
	ImageStaging* staging() const { return staging_; }

	/// The platform's staging area the stored image is read into to be loaded (homecontrol #71): at
	/// boot and when a failed apply returns to it. Not the one of set_staging(): a restore runs while
	/// the received image that failed is still in that one. The text is put together there through a
	/// piece of ImageStore::kReadPiece and parsed in place, so the load needs no heap block of the
	/// image's size; none, or when it declines (the reason is logged): a heap buffer.
	void set_stored_staging(ImageStaging* staging) { stored_staging_ = staging; }
	ImageStaging* stored_staging() const { return stored_staging_; }

	/// Told the phases of every load (homecontrol #70, doc/image.md "Memory of a swap"); nullptr: none.
	/// It outlives the service or is removed first.
	void set_load_probe(LoadProbe* probe) { probe_ = probe; }

	void set_ntp_changed(Callback callback) { ntp_changed_ = callback; }
	/// Called after every change of the loaded image (load, swap, restore, unload): the model's ids
	/// may have changed (the web API resyncs its live feed).
	void set_changed(Callback callback) { changed_ = callback; }

	/// Registers a provider of dynamic datapoints (before or after start()). Its specs join the model
	/// with the next load or reload (at once when the service runs). False when full or registered.
	/// The provider outlives the service or is removed first (the destructor unregisters from it).
	bool add_provider(DynamicSpecProvider& provider);
	/// Unregisters it; the model is reloaded without its datapoints. False if it was not registered.
	bool remove_provider(DynamicSpecProvider& provider);
	/// Datapoints in the model that come from providers (after the image's).
	size_t dynamic_datapoints() const { return dynamic_count_; }
	/// Model reloads done for provider changes since start.
	uint32_t dynamic_reloads() const { return dynamic_reloads_; }
	/// Whether a reload for a provider change is scheduled.
	bool reload_pending() const { return reload_pending_; }
	/// Why the last reload for a provider change was refused (its first refusal: the path of the
	/// image's datapoint, or "", and the reason); nullptr when the last one succeeded.
	/// Held on the heap while the refusal stands (with no heap for it: a fixed "no memory" refusal).
	const Report::Refusal* dynamic_refusal() const;

	// LedInput
	bool image_loaded() const override { return loaded_; }
	bool image_degraded() const override;

	Scheduler& scheduler() { return scheduler_; }
	rules::Engine& engine() { return engine_; }
	/// The block runtime (spec/blocks.md): the image's `blocks`, called in the engine's cascades.
	blocks::BlockRuntime& blocks() { return blocks_; }
	ValueStore& value_store() { return value_store_; }
	History& history() { return history_; }
	knx::TimeMaster& time_master() { return time_master_; }
	/// The platform's KNX adapter the service configures.
	knx::KnxAdapter& knx_adapter() { return adapter_; }
	/// The registered providers of dynamic datapoints (the monitor stream finds the Zigbee adapter).
	size_t provider_count() const { return provider_count_; }
	DynamicSpecProvider* provider(size_t index) const { return index < provider_count_ ? providers_[index] : nullptr; }

	/// What the UI needs of the loaded image beyond the components (GET /api/model); built after
	/// every successful load, empty without an image.
	const ModelView& view() const { return view_; }
	/// When the loaded image was applied (monotonic ms; with source() and info(): "deployed when and how").
	uint64_t loaded_ms() const { return loaded_ms_; }

private:
	/// The composed specs of a model load: the image's (`image_count`) and the providers'.
	struct Composed;

	bool load_steps(const char* text, size_t length, Source source, bool store, const uint8_t* signature,
		ImageBody* holder, Callback stored);
	bool apply(Plan& plan, Source source);
	/// Tells the probe the outer load ended (once).
	void end_probe(bool ok);
	void phase(const char* name) {
		if (probe_ != nullptr) {
			probe_->load_phase(name);
		}
	}
	/// Starts the store of `text` off the loop with `holder`'s body; false (nothing taken) when it
	/// cannot (the reason logged): the caller stores within load().
	bool store_off_loop(const char* text, size_t length, ImageBody& holder, Callback stored);
	/// The worker's job (on its thread) and its end (on the loop).
	static void store_job(void* self);
	void store_done();
	/// Appends the store's limit to `error` (of `size` bytes) when an image of `length` exceeds it.
	void stored_beyond(size_t length, char* error, size_t size) const;
	/// Logs a warning when a stored image of `length` exceeds what the stored staging holds beside a
	/// failed image (Storable::staging).
	void warn_storable(size_t length) const;
	static void write_info_of(JsonWriter& json, const ImageInfo& info, Source source, bool stored,
		const char* store_error);
	/// The signature gate of load(): true when the image may be parsed.
	bool authorize(const char* text, size_t length, Source source, const uint8_t* signature);
	bool compose(const Plan* plan, Composed& out, Report& report);
	void bind_providers(const Plan* plan, const Composed& composed);
	bool bind_persistence(const Plan& plan, Source source);
	bool bind_technologies(const Plan& plan, Source source);
	bool reload_dynamic(const char* why);
	void reload_due();
	void drop_plan();
	ImageBindings bindings_of(const Plan* plan) const;
	// DynamicSpecListener
	void dynamic_specs_changed(DynamicSpecProvider& provider) override;
	bool fail_applied(Source source, const char* path, const char* format, ...) HC_PRINTF_FORMAT(4, 5);
	/// Records why a reload for a provider change was refused (dynamic_refusal()).
	void refuse_dynamic(const char* path, const char* format, ...) HC_PRINTF_FORMAT(3, 4);
	/// The last reload succeeded: the refusal goes.
	void accept_dynamic();
	void tear_down();
	void apply_settings(const Plan& plan);

	// ScheduleListener
	void schedule_fired(const char* id) override;
	void quietly(void (*report)(void* ctx), void* ctx) override;

	Controller& controller_;
	DatapointModel& model_;
	knx::KnxAdapter& adapter_;
	const knx::KnxTunnel* tunnel_;
	HistoryLimits history_limits_;
	RulesClock rules_clock_;
	Scheduler scheduler_;
	rules::Engine engine_;
	blocks::BlockRuntime blocks_;
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
	char store_error_[96] = {};
	uint64_t loaded_ms_ = 0;
	ExternalValues* external_ = nullptr;
	Mqtt* mqtt_ = nullptr;
	ImageStaging* staging_ = nullptr;
	ImageStaging* stored_staging_ = nullptr;
	Worker* worker_ = nullptr;
	LoadProbe* probe_ = nullptr;
	unsigned load_depth_ = 0; ///< load() within load() (a failed apply restores the stored image).
	bool probe_open_ = false; ///< The probe was told the outer load began and not yet that it ended.
	/// The store off the loop: the body it reads (taken from the receiver), the text in it, the outcome
	/// the job writes (read on the loop after `done`), the caller's callback.
	std::atomic<bool> store_pending_{false};
	ImageBody pending_body_;
	const char* pending_text_ = nullptr;
	size_t pending_length_ = 0;
	bool job_ok_ = false;
	const char* job_error_ = nullptr;
	uint32_t job_ms_ = 0;
	Callback stored_;
	/// The deploy whose store ran (or runs) off the loop, for its deferred answer: its image's
	/// metadata as loaded (the image may change before the answer is fetched), the outcome.
	struct Answer {
		uint32_t ticket = 0;
		ImageInfo info;
		Source source = Source::api;
		bool stored = false;
		char error[96] = {};
	} answer_;
	ModelView view_;
	DynamicSpecProvider* providers_[kMaxProviders] = {};
	size_t provider_count_ = 0;
	/// The applied image's plan, kept for provider reloads (only with a provider registered).
	Plan* plan_ = nullptr;
	size_t dynamic_count_ = 0;
	uint32_t dynamic_reloads_ = 0;
	bool reload_pending_ = false;
	uint64_t last_model_load_ms_ = 0;
	bool model_loaded_once_ = false;
	TimerHandle reload_timer_;
	bool dynamic_refused_ = false;
	Report::Refusal* dynamic_refusal_ = nullptr;
	Callback ntp_changed_;
	Callback changed_;
};

} // namespace image
} // namespace hc

#endif // HC_IMAGE_IMAGE_SERVICE_H
