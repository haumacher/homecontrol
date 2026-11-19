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
#include "hc/image/binary.h"
#include "hc/image/load_probe.h"
#include "hc/image/loader.h"
#include "hc/image/model_view.h"
#include "hc/image/rules_clock.h"
#include "hc/json.h"
#include "hc/knx/adapter.h"
#include "hc/knx/time_master.h"
#include "hc/knx/tunnel.h"
#include "hc/platform/blob_store.h"
#include "hc/platform/image_slot.h"
#include "hc/storage_stats.h"
#include "hc/technology.h"
#include "hc/rules/engine.h"
#include "hc/scheduler.h"
#include "hc/value_store.h"

namespace hc {

class Mqtt;
namespace image {

/// The runtime image on the device (doc/image.md, spec/runtime-image.md 6 and 7, issue #87): the
/// binary image in the platform's ImageSlot, validated (validate()) and applied in place. It owns the
/// components an image configures beyond the datapoint model and the KNX adapter: the scheduler, the
/// rule engine (with its time source), the block runtime, the value store, the history and the KNX
/// time master.
///
/// A deploy (receive_begin() .. receive_write() .. receive_end(), or load() for a body at hand):
///
///  1. the header is checked (10.1); a refusal leaves the running image untouched;
///  2. the rules' queued events are processed, the value store and the history write what they hold,
///     the values, the rule and block runtime are kept by id hash (SwapTable, section 12); the
///     components stop and the slot is unmapped: the device runs without an image;
///  3. the body is written into the slot as it arrives (erased sector by sector);
///  4. the slot is mapped; 5. checked: CRC, signature, structure (section 10);
///  6..12. the components are configured from the mapped image (Loader), the kept values and runtime
///     follow their ids, the rules and blocks start (reason `image`).
///
/// A refusal or failure after step 2 leaves no image (I9); the swap table stays until an image is
/// applied, so that E-Home's next deployment finds the values. At boot the slot's image is checked
/// (without the signature, I11) and applied with reason `boot`.
///
/// Dynamic datapoints (doc/image.md "Dynamic datapoints"): registered DynamicSpecProviders (the
/// Zigbee adapter) add datapoints of their own behind the image's. When a provider's specs change,
/// the model is reloaded with the image's datapoints from the slot, at most once per
/// kReloadSpacingMs, and every component is bound again; the rules continue.
///
/// Threading: everything on the event loop's thread, except receive_write() (the receiver's thread
/// while a receive is open) and receiving().
class ImageService : public LedInput, private ScheduleListener, private QuietReports, private DynamicSpecListener {
public:
	/// Most providers registered at the same time.
	static constexpr size_t kMaxProviders = 4;
	/// Shortest time between two model reloads for provider changes.
	static constexpr uint32_t kReloadSpacingMs = 1000;

	/// `slot` is the platform's image slot; `tunnel` (may be null) tells the LED whether KNX is up;
	/// `adapter` is the platform's KNX adapter (also when no gateway is configured).
	ImageService(Controller& controller, DatapointModel& model, BlobStore& blobs, ImageSlot& slot,
		knx::KnxAdapter& adapter, const knx::KnxTunnel* tunnel, const HistoryLimits& history_limits = HistoryLimits(),
		const ValueStoreLimits& value_limits = ValueStoreLimits());
	~ImageService();

	ImageService(const ImageService&) = delete;
	ImageService& operator=(const ImageService&) = delete;

	/// Registers with the time service and the controller's LED, removes the JSON images firmware up
	/// to 1.2 kept in the blob store, and loads the slot's image (Source::boot; an erased slot is no
	/// image, a refused one is reported). Call once, after Controller::start().
	void start();

	/// Step 1 and 2 of a deploy: checks the header in `head` (the body's first `available` bytes, at
	/// least min(32, length)) against the announced `length` (10.1), then stops the running image,
	/// keeping its values (section 12). False with report() when refused (the running image
	/// untouched). True: write the body from its first byte with receive_write(), then
	/// receive_end() (or receive_abort()).
	bool receive_begin(const uint8_t* head, size_t available, size_t length, Source source);
	/// Step 3: the next `length` bytes of the body into the slot. Any thread while a receive is open.
	/// False when the slot refused or the body overruns: receive_end() then reports it.
	bool receive_write(const void* data, size_t length);
	/// Steps 4 to 12: maps, checks (with the signature), applies. False with report() (the device has
	/// no image then).
	bool receive_end();
	/// The body broke off: no image; reported as `image.format` (the bytes received).
	void receive_abort();
	/// Whether a receive is open (any thread).
	bool receiving() const { return receiving_.load(std::memory_order_acquire); }

	/// A deploy of a body at hand (receive_begin, receive_write, receive_end).
	bool load(const uint8_t* bytes, size_t length, Source source);

	/// Removes the image: no datapoints, no rules, no schedules; erases the slot and deletes the
	/// persisted values, rings, schedule records and parameter overrides of the image. False with
	/// `error` while a receive is open or when the slot could not be erased (unloaded anyway).
	bool unload(const char** error);

	/// Writes what the value store and the history hold pending (before a reboot).
	void flush();

	bool loaded() const { return loaded_; }
	const ImageInfo& info() const { return info_; }
	Source source() const { return source_; }
	/// The report of the last load that failed (cleared by a successful one).
	const Report& report() const { return report_; }
	/// The slot's size: the largest image the device takes (`image.slot`).
	size_t slot_size() const { return slot_.size(); }
	/// The loaded image in place (open() only while one is loaded).
	const bin::View& image() const { return view_; }
	/// Changes with every load and unload (a streamed GET /api/image or /api/pages aborts on it).
	uint32_t token() const { return token_; }

	/// The members of the "image" object of /api/status (`full`: also those only GET /api/status
	/// carries).
	void write_status(JsonWriter& json, bool full = false) const;
	/// The answer to a successful PUT: the metadata and the counts.
	void write_info(JsonWriter& json) const;
	/// The answer to a refused PUT: {"error", "code", "refusals": [{"path", "reason"}], "total", "loaded"}.
	void write_report(JsonWriter& json) const;

	/// The external values (weather and other HTTP sources, doc/external.md) the image's external
	/// sources configure after every load and clear on unload; nullptr: they are ignored.
	void set_external(ExternalValues* external) { external_ = external; }
	/// The MQTT client (doc/mqtt.md) the image's `mqtt` record configures after every load; nullptr:
	/// ignored.
	void set_mqtt(Mqtt* mqtt) { mqtt_ = mqtt; }

	/// Told the phases of every load (doc/image.md "Memory of a swap"); nullptr: none.
	void set_load_probe(LoadProbe* probe) { probe_ = probe; }

	/// Called when the image changed ntp.server (the platform restarts SNTP).
	void set_ntp_changed(Callback callback) { ntp_changed_ = callback; }
	/// Called after every change of the loaded image (load, swap, unload): the model's ids may have
	/// changed (the web API resyncs its live feed).
	void set_changed(Callback callback) { changed_ = callback; }

	/// Registers a provider of dynamic datapoints (before or after start()). Its specs join the model
	/// with the next load or reload. False when full or registered.
	bool add_provider(DynamicSpecProvider& provider);
	/// Unregisters it; the model is reloaded without its datapoints.
	bool remove_provider(DynamicSpecProvider& provider);
	/// Datapoints in the model that come from providers (after the image's).
	size_t dynamic_datapoints() const { return dynamic_count_; }
	/// Model reloads done for provider changes since start.
	uint32_t dynamic_reloads() const { return dynamic_reloads_; }
	/// Whether a reload for a provider change is scheduled.
	bool reload_pending() const { return reload_pending_; }
	/// Why the last reload for a provider change was refused; nullptr when the last one succeeded.
	const Report::Refusal* dynamic_refusal() const;

	/// Whether values are kept for the next image (the swap table of section 12, after a failed one).
	bool swap_kept() const { return swap_ != nullptr; }

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

	/// What the UI needs of the loaded image beyond the components (GET /api/model).
	const ModelView& view() const { return model_view_; }
	/// When the loaded image was applied (monotonic ms).
	uint64_t loaded_ms() const { return loaded_ms_; }

private:
	/// The owner of the image's Zigbee-bound datapoints that no provider took (a platform without a
	/// Zigbee adapter: the Linux package, homecontrol #95): they keep no value from a device, and a
	/// command or a read is refused with that reason in the log (the API answers 409 dp.refused).
	class NoZigbee : public Technology {
	public:
		const char* name() const override { return "zigbee"; }
		bool command(const Datapoint& datapoint, const Value& value) override;
		bool read(const Datapoint& datapoint) override;
	};

	/// The composed specs of a model load: the image's (`image_count`) and the providers'.
	struct Composed;
	struct SwapTable;

	/// Step 2: takes the swap table and stops the components; the slot is unmapped.
	void stop_image(bool keep);
	/// Steps 4 to 12 on the slot's `length` bytes.
	bool apply_slot(size_t length, Source source, bool check_signature);
	bool apply(const Plan& plan, Source source);
	void end_probe(bool ok);
	void phase(const char* name) {
		if (probe_ != nullptr) {
			probe_->load_phase(name);
		}
	}
	static void write_info_of(JsonWriter& json, const ImageInfo& info, Source source);
	bool compose(const Plan* plan, Composed& out, Report& report);
	void bind_providers(const Plan* plan, const Composed& composed);
	bool bind_persistence(const Plan& plan, Source source);
	bool bind_technologies(const Plan& plan, Source source);
	bool reload_dynamic(const char* why);
	void reload_due();
	ImageBindings bindings_of(const Plan* plan) const;
	// DynamicSpecListener
	void dynamic_specs_changed(DynamicSpecProvider& provider) override;
	bool fail_applied(Source source, const char* path, const char* format, ...) HC_PRINTF_FORMAT(4, 5);
	void refuse_dynamic(const char* path, const char* format, ...) HC_PRINTF_FORMAT(3, 4);
	void accept_dynamic();
	void tear_down();
	void apply_settings(const Plan& plan);
	void drop_swap();
	/// The model's values by name hash, merged with `merge`'s entries of other hashes; nullptr
	/// without memory (logged).
	SwapTable* capture_values(const SwapTable* merge);
	/// Gives the model's datapoints the values of `table` (hash and type match); how many.
	size_t restore_values(const SwapTable* table);
	/// Removes the JSON images of firmware up to 1.2 from the blob store (spec/runtime-image.md 7).
	void remove_json_images();

	// ScheduleListener
	void schedule_fired(const char* id) override;
	void quietly(void (*report)(void* ctx), void* ctx) override;

	Controller& controller_;
	DatapointModel& model_;
	BlobStore& blobs_;
	ImageSlot& slot_;
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

	ImageInfo info_;
	Report report_;
	Source source_ = Source::boot;
	bool loaded_ = false;
	bool has_knx_ = false;
	bool rules_started_ = false;
	bool started_ = false;
	uint32_t token_ = 1;
	uint64_t loaded_ms_ = 0;
	/// The mapped slot and the image in it (while loaded).
	const uint8_t* mapped_ = nullptr;
	bin::View view_;
	/// The open receive (spec 6 step 3).
	std::atomic<bool> receiving_{false};
	size_t receive_length_ = 0;
	size_t received_ = 0;
	bool receive_failed_ = false;
	const char* receive_error_ = nullptr;
	Source receive_source_ = Source::api;
	SwapTable* swap_ = nullptr;
	ExternalValues* external_ = nullptr;
	Mqtt* mqtt_ = nullptr;
	LoadProbe* probe_ = nullptr;
	bool probe_open_ = false;
	ModelView model_view_;
	DynamicSpecProvider* providers_[kMaxProviders] = {};
	size_t provider_count_ = 0;
	NoZigbee no_zigbee_;
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
