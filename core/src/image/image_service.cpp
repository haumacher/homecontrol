/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/image/image_service.h"
#include "hc/status_text.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

#include "hc/image/validator.h"
#include "hc/mqtt.h"
#include "hc/network_settings.h"
#include "hc/signature.h"

namespace hc {
namespace image {

namespace {

constexpr const char* kTag = "hc.image";

const char* report_text(Report::Kind kind) {
	switch (kind) {
		case Report::Kind::format: return "the body is not a binary runtime image";
		case Report::Kind::version: return "the image's format is not one this firmware reads";
		case Report::Kind::too_large: return "the image is larger than the device's image slot";
		case Report::Kind::unsigned_image: return "the device holds the installation key: the image must be signed";
		case Report::Kind::damaged: return "the image was damaged on the way or in the slot";
		case Report::Kind::bad_signature: return "the image's signature does not verify under the installation key";
		case Report::Kind::no_memory: return "the device has not enough memory for the image";
		case Report::Kind::apply: return "the image was valid but could not be applied";
		case Report::Kind::busy: return "another image is being received; deploy again in a moment";
		default: return "the image is invalid";
	}
}

void log_report(const Report& report, Source source, bool untouched, bool loaded) {
	Log::warn(kTag, "image from %s refused (%s): %u problem%s; %s", source_name(source), report_kind_name(report.kind),
		static_cast<unsigned>(report.total), report.total == 1 ? "" : "s",
		untouched ? (loaded ? "the running image stays" : "the device keeps no image") : "the device has no image");
	for (size_t i = 0; i < report.count; ++i) {
		Log::warn(kTag, "  %s: %s", report.refusals[i].path[0] != '\0' ? report.refusals[i].path : "(image)",
			report.refusals[i].reason);
	}
}

} // namespace

/// The values of the datapoints across a swap (spec/runtime-image.md 12), and what the rule engine
/// carries; kept until an image has been applied. The entries in chunks of kPer and an index sorted
/// by hash (homecontrol #70: no block of the values' size beside the new image).
struct ImageService::SwapTable {
	struct Entry {
		uint64_t hash;
		uint64_t updated_ms;
		Value value;
		Quality quality;
		bool pending; ///< A command held for sending (DatapointModel pending mark).
		bool dropped;
	};
	static constexpr size_t kPer = 16;
	Entry** chunks = nullptr;
	size_t chunk_count = 0;
	uint16_t* order = nullptr; ///< Entry indices sorted by hash.
	size_t count = 0;
	rules::Engine::Carry* carry = nullptr;

	~SwapTable() {
		release();
		rules::Engine::free_carry(carry);
	}
	void release() {
		for (size_t k = 0; k < chunk_count; ++k) {
			delete[] chunks[k];
		}
		delete[] chunks;
		delete[] order;
		chunks = nullptr;
		order = nullptr;
		chunk_count = 0;
		count = 0;
	}
	Entry& at(size_t i) const { return chunks[i / kPer][i % kPer]; }
	bool allocate(size_t n) {
		if (n == 0) {
			return true;
		}
		chunk_count = (n + kPer - 1) / kPer;
		chunks = new (std::nothrow) Entry*[chunk_count]();
		order = new (std::nothrow) uint16_t[n];
		if (chunks == nullptr || order == nullptr) {
			delete[] chunks;
			chunks = nullptr;
			chunk_count = 0;
			return false;
		}
		for (size_t k = 0; k < chunk_count; ++k) {
			chunks[k] = new (std::nothrow) Entry[kPer];
			if (chunks[k] == nullptr) {
				return false; // release() frees what was taken
			}
		}
		return true;
	}
	void sort() {
		for (size_t i = 0; i < count; ++i) {
			order[i] = static_cast<uint16_t>(i);
		}
		std::sort(order, order + count, [this](uint16_t a, uint16_t b) { return at(a).hash < at(b).hash; });
		for (size_t i = 1; i < count; ++i) {
			Entry& a = at(order[i - 1]);
			Entry& b = at(order[i]);
			if (a.hash == b.hash) {
				Log::warn(kTag, "swap: 2 datapoints share the hash 0x%016" PRIx64 "; their values are not carried", a.hash);
				a.dropped = true;
				b.dropped = true;
			}
		}
	}
	const Entry* find(uint64_t hash) const {
		size_t lo = 0, hi = count;
		while (lo < hi) {
			size_t mid = (lo + hi) / 2;
			if (at(order[mid]).hash < hash) {
				lo = mid + 1;
			} else {
				hi = mid;
			}
		}
		return lo < count && at(order[lo]).hash == hash && !at(order[lo]).dropped ? &at(order[lo]) : nullptr;
	}
};

ImageService::ImageService(Controller& controller, DatapointModel& model, BlobStore& blobs, ImageSlot& slot,
	knx::KnxAdapter& adapter, const knx::KnxTunnel* tunnel, const HistoryLimits& history_limits,
	const ValueStoreLimits& value_limits)
	: controller_(controller),
	  model_(model),
	  blobs_(blobs),
	  slot_(slot),
	  adapter_(adapter),
	  tunnel_(tunnel),
	  history_limits_(history_limits),
	  rules_clock_(controller.time()),
	  scheduler_(controller.loop(), controller.time(), controller.settings(), &model),
	  engine_(controller.loop(), model, controller.clock(), controller.settings(), rules_clock_),
	  blocks_(controller.loop(), model, controller.clock(), controller.settings(), blobs, engine_),
	  value_store_(controller.loop(), controller.clock(), model, blobs, value_limits),
	  history_(controller.loop(), controller.clock(), model, blobs, history_limits, &controller.time_zone_platform()),
	  time_master_(controller.loop(), controller.time(), model, adapter) {
	scheduler_.defer_time_reports(true); // published quietly after each load (bind_technologies)
	engine_.set_timeline_store(&blobs);   // the inhabitant's timelines (spec/rules.md 8.7)
	scheduler_.set_quiet_reports(this);   // the outputs' first values after a load (issue #67)
}

ImageService::~ImageService() {
	if (reload_pending_) {
		controller_.loop().cancel(reload_timer_);
	}
	for (size_t i = 0; i < provider_count_; ++i) {
		providers_[i]->set_spec_listener(nullptr);
	}
	delete swap_;
	delete dynamic_refusal_;
	if (mapped_ != nullptr) {
		slot_.unmap();
	}
	if (started_) {
		controller_.set_led_input(nullptr);
		controller_.time().remove_listener(scheduler_);
		controller_.time().remove_listener(time_master_);
		controller_.time().remove_listener(engine_);
		scheduler_.remove_listener(*this);
	}
}

void ImageService::remove_json_images() {
	// The JSON images of firmware up to 1.2 (spec/runtime-image.md 7): `image.idx` and the chunks
	// `image.a.<n>`, `image.b.<n>`. Collected first, removed after the visit.
	struct Collect : BlobVisitor {
		char names[16][24] = {};
		size_t count = 0;
		bool more = false;
		void blob(const char* name, size_t) override {
			bool ours = std::strcmp(name, "image.idx") == 0 || std::strncmp(name, "image.a.", 8) == 0 ||
				std::strncmp(name, "image.b.", 8) == 0;
			if (!ours) {
				return;
			}
			if (count < 16 && std::strlen(name) < sizeof(names[0])) {
				std::snprintf(names[count++], sizeof(names[0]), "%s", name);
			} else {
				more = true;
			}
		}
	};
	size_t removed = 0;
	for (int round = 0; round < 16; ++round) {
		Collect collect;
		blobs_.visit(collect);
		for (size_t i = 0; i < collect.count; ++i) {
			if (blobs_.remove(collect.names[i]) == BlobStatus::ok) {
				++removed;
			}
		}
		if (!collect.more) {
			break;
		}
	}
	if (removed > 0) {
		Log::info(kTag, "removed %u blob(s) of the JSON image of firmware up to 1.2 (the image lives in its slot now)",
			static_cast<unsigned>(removed));
	}
}

void ImageService::start() {
	if (started_) {
		Log::error(kTag, "refusing start: the image service runs already");
		return;
	}
	started_ = true;
	if (!controller_.time().add_listener(scheduler_)) {
		Log::error(kTag, "schedules will not fire: no time listener slot for the scheduler");
	}
	if (!controller_.time().add_listener(engine_, rules::Engine::kTimeEvents)) {
		Log::error(kTag, "the rules' timelines will not follow the clock: no time listener slot");
	}
	if (!controller_.time().add_listener(time_master_, knx::TimeMaster::kTimeEvents)) {
		Log::error(kTag, "the KNX time master will not broadcast: no time listener slot");
	}
	scheduler_.add_listener(*this);
	controller_.set_led_input(this);
	remove_json_images();
	size_t size = slot_.size();
	if (size == 0) {
		Log::error(kTag, "no image slot: the device cannot hold a runtime image");
	} else {
		const uint8_t* bytes = slot_.map();
		if (bytes == nullptr) {
			Log::error(kTag, "the image slot cannot be mapped: the device runs without an image");
		} else {
			bool erased = false;
			report_.clear();
			if (check_slot_header(bytes, size, erased, report_)) {
				apply_slot(reinterpret_cast<const bin::Header*>(bytes)->length, Source::boot, false);
			} else if (erased) {
				slot_.unmap();
				Log::info(kTag, "no runtime image in the slot (%u KB): PUT /api/image deploys one (doc/image.md)",
					static_cast<unsigned>(size / 1024));
			} else {
				slot_.unmap();
				log_report(report_, Source::boot, false, false);
			}
		}
	}
	if (!loaded_ && provider_count_ > 0) {
		reload_dynamic("start without an image");
	}
}

// ---- Receiving (spec/runtime-image.md 6) --------------------------------------------------------

bool ImageService::receive_begin(const uint8_t* head, size_t available, size_t length, Source source) {
	if (receiving()) {
		// The open receive's own report is written by its receive_end(), which starts from a clear one.
		Log::warn(kTag, "image from %s refused: another image is being received", source_name(source));
		report_.clear();
		report_.add(Report::Kind::busy, "", "another image is being received: deploy again in a moment");
		return false;
	}
	report_.clear();
	if (length > slot_.size()) {
		report_.add(Report::Kind::too_large, "header.length", "the image has %u bytes; the device holds at most %u (its image slot)",
			static_cast<unsigned>(length), static_cast<unsigned>(slot_.size()));
		log_report(report_, source, true, loaded_);
		return false;
	}
	if (!check_header(head, available, length, report_)) {
		log_report(report_, source, true, loaded_);
		return false;
	}
	bin::Header h;
	std::memcpy(&h, head, sizeof(h)); // the receiver's buffer need not be aligned
	uint8_t key[kEd25519KeySize];
	KeyState state = load_installation_key(controller_.settings(), key);
	if (state == KeyState::damaged) {
		report_.add(Report::Kind::bad_signature, "signature", "the installation key stored on the device is damaged");
		log_report(report_, source, true, loaded_);
		return false;
	}
	if (state == KeyState::ok && (h.flags & bin::kFlagSigned) == 0) {
		char fingerprint[kFingerprintSize];
		key_fingerprint(key, fingerprint);
		report_.add(Report::Kind::unsigned_image, "header.flags",
			"the image is not signed; the device accepts only images signed with the installation key %s", fingerprint);
		log_report(report_, source, true, loaded_);
		return false;
	}
	Log::info(kTag, "receiving an image of %u bytes from %s (format %u.%u, %s)", static_cast<unsigned>(length),
		source_name(source), static_cast<unsigned>(h.major), static_cast<unsigned>(h.minor),
		(h.flags & bin::kFlagSigned) != 0 ? "signed" : "unsigned");
	stop_image(true);
	slot_.begin_write();
	receive_length_ = length;
	received_ = 0;
	receive_failed_ = false;
	receive_error_ = nullptr;
	receive_source_ = source;
	receiving_.store(true, std::memory_order_release);
	return true;
}

bool ImageService::receive_write(const void* data, size_t length) {
	if (!receiving() || receive_failed_) {
		return false;
	}
	if (length > receive_length_ - received_) {
		receive_failed_ = true;
		receive_error_ = "the body is longer than the header says";
		return false;
	}
	const char* error = nullptr;
	if (!slot_.write(received_, data, length, &error)) {
		receive_failed_ = true;
		receive_error_ = error != nullptr ? error : "the slot refused a write";
		return false;
	}
	received_ += length;
	return true;
}

bool ImageService::receive_end() {
	if (!receiving()) {
		return false;
	}
	receiving_.store(false, std::memory_order_release);
	report_.clear();
	if (receive_failed_) {
		report_.add(receive_error_ != nullptr && std::strstr(receive_error_, "longer") != nullptr ? Report::Kind::format
																							   : Report::Kind::apply,
			"", "%s (%u of %u bytes written)", receive_error_ != nullptr ? receive_error_ : "the slot refused a write",
			static_cast<unsigned>(received_), static_cast<unsigned>(receive_length_));
		log_report(report_, receive_source_, false, false);
		if (changed_) {
			changed_();
		}
		return false;
	}
	if (received_ < receive_length_) {
		report_.add(Report::Kind::format, "header.length", "the header says %u bytes, the body has %u",
			static_cast<unsigned>(receive_length_), static_cast<unsigned>(received_));
		log_report(report_, receive_source_, false, false);
		if (changed_) {
			changed_();
		}
		return false;
	}
	return apply_slot(receive_length_, receive_source_, true);
}

void ImageService::receive_abort() {
	if (!receiving()) {
		return;
	}
	receiving_.store(false, std::memory_order_release);
	report_.clear();
	report_.add(Report::Kind::format, "header.length", "the header says %u bytes, the body has %u",
		static_cast<unsigned>(receive_length_), static_cast<unsigned>(received_));
	log_report(report_, receive_source_, false, false);
	if (changed_) {
		changed_();
	}
}

bool ImageService::load(const uint8_t* bytes, size_t length, Source source) {
	size_t head = length < bin::kHeaderSize ? length : bin::kHeaderSize;
	if (!receive_begin(bytes, head, length, source)) {
		return false;
	}
	receive_write(bytes, length);
	return receive_end();
}

ImageService::SwapTable* ImageService::capture_values(const SwapTable* merge) {
	SwapTable* next = new (std::nothrow) SwapTable();
	size_t n = 0;
	for (size_t i = 0; i < model_.size(); ++i) {
		n += model_.get(static_cast<uint16_t>(i))->quality() != Quality::unknown ? 1 : 0;
	}
	size_t old = merge != nullptr ? merge->count : 0;
	if (next == nullptr || !next->allocate(n + old)) {
		Log::error(kTag, "no memory to keep %u values across the swap: they start unknown", static_cast<unsigned>(n + old));
		delete next;
		return nullptr;
	}
	for (size_t i = 0; i < model_.size(); ++i) {
		const Datapoint* dp = model_.get(static_cast<uint16_t>(i));
		if (dp->quality() == Quality::unknown) {
			continue;
		}
		SwapTable::Entry& e = next->at(next->count++);
		e.hash = bin::fnv1a(dp->name());
		e.updated_ms = dp->updated_ms();
		e.value = dp->value();
		e.quality = dp->quality();
		e.pending = dp->pending();
		e.dropped = false;
	}
	size_t own = next->count;
	for (size_t i = 0; i < old; ++i) {
		const SwapTable::Entry& e = merge->at(i);
		bool present = false;
		for (size_t k = 0; k < own && !present; ++k) {
			present = next->at(k).hash == e.hash;
		}
		if (!present) {
			next->at(next->count++) = e;
		}
	}
	next->sort();
	return next;
}

size_t ImageService::restore_values(const SwapTable* table) {
	size_t carried = 0;
	for (size_t i = 0; table != nullptr && i < model_.size(); ++i) {
		const Datapoint* dp = model_.get(static_cast<uint16_t>(i));
		const SwapTable::Entry* e = table->find(bin::fnv1a(dp->name()));
		if (e != nullptr && e->value.type() == dp->type() &&
			model_.carry(static_cast<uint16_t>(i), e->value, e->quality, e->updated_ms, e->pending) == DatapointStatus::ok) {
			++carried;
		}
	}
	return carried;
}

void ImageService::stop_image(bool keep) {
	engine_.flush();
	value_store_.flush();
	history_.flush();
	blocks_.flush();
	if (keep) {
		// The values of the model (the image's datapoints and the providers') by id hash (section 12).
		// A table a failed image left keeps its entries where the model has none of that hash (the
		// image's values; the model then holds only the providers').
		SwapTable* next = capture_values(swap_);
		if (loaded_) {
			rules::Engine::Carry* carry = engine_.detach();
			if (next != nullptr) {
				next->carry = carry;
			} else {
				rules::Engine::free_carry(carry);
			}
		} else if (next != nullptr && swap_ != nullptr) {
			next->carry = swap_->carry; // the rules of the image before the failed one
			swap_->carry = nullptr;
			rules::Engine::free_carry(engine_.detach());
		} else {
			rules::Engine::free_carry(engine_.detach());
		}
		if (next != nullptr || loaded_) {
			delete swap_;
			swap_ = next;
		}
	} else {
		rules::Engine::free_carry(engine_.detach());
	}
	adapter_.clear();
	time_master_.clear();
	scheduler_.clear();
	if (external_ != nullptr) {
		external_->clear();
	}
	if (mqtt_ != nullptr) {
		mqtt_->clear();
	}
	model_.clear();
	for (size_t i = 0; i < provider_count_; ++i) {
		providers_[i]->model_cleared();
	}
	dynamic_count_ = 0;
	model_view_.clear();
	view_.close();
	if (mapped_ != nullptr) {
		slot_.unmap();
		mapped_ = nullptr;
	}
	loaded_ = false;
	has_knx_ = false;
	info_ = ImageInfo();
	++token_;
	// The providers' datapoints come back with the new image (their values through the table), or
	// with the reload after a refusal; a generic interim would reconfigure their devices.
	if (changed_) {
		changed_();
	}
}

void ImageService::drop_swap() {
	delete swap_;
	swap_ = nullptr;
}

bool ImageService::apply_slot(size_t length, Source source, bool check_signature) {
	uint64_t begin = controller_.clock().monotonic_ms();
	report_.clear();
	(void)length;
	mapped_ = slot_.map();
	if (mapped_ == nullptr) {
		report_.add(Report::Kind::apply, "", "the image slot cannot be mapped");
		log_report(report_, source, false, false);
		if (changed_) {
			changed_();
		}
		return false;
	}
	auto refuse = [&]() {
		log_report(report_, source, false, false);
		view_.close();
		slot_.unmap();
		mapped_ = nullptr;
		blocks_.clear(false); // the records and overrides stay for the image deployed next
		if (provider_count_ > 0) {
			reload_dynamic("no image");
			restore_values(swap_);
		}
		if (changed_) {
			changed_();
		}
		return false;
	};
	if (!check_crc(mapped_, report_)) {
		return refuse();
	}
	const bin::Header& h = *reinterpret_cast<const bin::Header*>(mapped_);
	if (check_signature && (h.flags & bin::kFlagSigned) != 0) {
		uint8_t key[kEd25519KeySize];
		if (load_installation_key(controller_.settings(), key) == KeyState::ok) {
			size_t signed_end = h.length - bin::kSignatureSize;
			if (!image_signature_valid(reinterpret_cast<const char*>(mapped_), signed_end, mapped_ + signed_end, key)) {
				char fingerprint[kFingerprintSize];
				key_fingerprint(key, fingerprint);
				report_.add(Report::Kind::bad_signature, "signature",
					"the signature does not verify under the installation key %s", fingerprint);
				return refuse();
			}
		}
	}
	if (!check_structure(mapped_, report_)) {
		return refuse();
	}
	if (probe_ != nullptr && !probe_open_) {
		probe_open_ = true;
		probe_->load_begun();
	}
	phase("validated");
	uint64_t t_validated = controller_.clock().monotonic_ms();
	view_.open(mapped_);
	Plan* plan = new (std::nothrow) Plan();
	if (plan == nullptr) {
		report_.add(Report::Kind::no_memory, "", "no memory to start the load");
		return refuse();
	}
	char language[setting::kMaxUiLangLength + 1];
	read_ui_lang(controller_.settings(), language, kTag); // labels in the device's language (spec/rules.md 2.2)
	Loader loader(controller_.clock(), history_limits_.cap_bytes, language);
	loader.set_archive_cap(history_limits_.archive_cap_bytes);
	loader.set_probe(probe_);
	if (!loader.build(view_, *plan, report_)) {
		delete plan;
		return refuse();
	}
	uint64_t t_plan = controller_.clock().monotonic_ms();
	if (!apply(*plan, source)) {
		delete plan;
		view_.close();
		slot_.unmap();
		mapped_ = nullptr;
		return false;
	}
	// The components hold what they need (copies, or pointers into the slot): the plan's tables go.
	// A reload for the providers builds them again from the slot (no parse; reload_dynamic()).
	bool has_mqtt = plan->has_mqtt;
	size_t tables = plan->arena.bytes();
	delete plan;
	drop_swap(); // applied: the values found their datapoints
	uint64_t t_apply = controller_.clock().monotonic_ms();
	if (mqtt_ != nullptr) {
		const char* mqtt_error = nullptr;
		if (has_mqtt && !mqtt_->configure_image(view_, &mqtt_error)) {
			Log::error(kTag, "the MQTT client refused the image's mqtt record: %s; MQTT is off until the next load",
				mqtt_error != nullptr ? mqtt_error : "?");
			mqtt_->clear();
		} else if (!has_mqtt) {
			mqtt_->clear();
		}
	}
	model_view_.build(view_, model_);
	++token_;
	end_probe(true);
	MemoryInfo memory = controller_.system().memory();
	Log::info(kTag, "image '%s' revision %u (%s, format %u.%u, schema %s) loaded from %s in %u ms: %u datapoints (%u KNX), "
		"%u rules, %u schedules, %u functions, %u locations, %u history rings%s; %u bytes in the slot, tables %u B",
		info_.project_id, static_cast<unsigned>(info_.revision), info_.name[0] != '\0' ? info_.name : "unnamed",
		static_cast<unsigned>(info_.format_major), static_cast<unsigned>(info_.format_minor), info_.schema_version,
		source_name(source), static_cast<unsigned>(controller_.clock().monotonic_ms() - begin),
		static_cast<unsigned>(info_.datapoints), static_cast<unsigned>(info_.knx_datapoints),
		static_cast<unsigned>(info_.rules), static_cast<unsigned>(info_.schedules),
		static_cast<unsigned>(info_.functions), static_cast<unsigned>(info_.locations),
		static_cast<unsigned>(info_.history_rings), info_.time_master ? ", time master" : "",
		static_cast<unsigned>(info_.bytes), static_cast<unsigned>(tables));
	if (blocks_.size() > 0) {
		blocks::BlockMemory bm = blocks_.memory();
		Log::info(kTag, "  %u block%s: runtime %u B, compiled %u B", static_cast<unsigned>(blocks_.size()),
			blocks_.size() == 1 ? "" : "s", static_cast<unsigned>(bm.runtime), static_cast<unsigned>(bm.set));
	}
	Log::info(kTag, "  heap after the load %u B free, lowest since boot %u B", static_cast<unsigned>(memory.free_bytes),
		static_cast<unsigned>(memory.min_free_bytes));
	Log::info(kTag, "  steps: checks %u ms, tables %u ms, apply %u ms", static_cast<unsigned>(t_validated - begin),
		static_cast<unsigned>(t_plan - t_validated), static_cast<unsigned>(t_apply - t_plan));
	if (info_.zigbee_datapoints > 0 || dynamic_count_ > 0) {
		Log::info(kTag, "  %u Zigbee binding(s); %u datapoint(s) of the providers",
			static_cast<unsigned>(info_.zigbee_datapoints), static_cast<unsigned>(dynamic_count_));
	}
	if (changed_) {
		changed_();
	}
	return true;
}

void ImageService::end_probe(bool ok) {
	if (probe_ != nullptr && probe_open_) {
		probe_open_ = false;
		probe_->load_ended(ok);
	}
}

void ImageService::apply_settings(const Plan& plan) {
	Settings& settings = controller_.settings();
	if (plan.tz != nullptr && std::strcmp(controller_.time_zone(), plan.tz) != 0) {
		const char* error = nullptr;
		if (controller_.set_time_zone(plan.tz, &error)) {
			Log::info(kTag, "time zone set by the image: %s", plan.tz);
		}
	}
	if (plan.has_location) {
		GeoLocation current;
		bool same = controller_.time().location(current) &&
			std::fabs(current.latitude - plan.location.latitude) < 5e-7 &&
			std::fabs(current.longitude - plan.location.longitude) < 5e-7 &&
			std::fabs(current.elevation_m - plan.location.elevation_m) < 0.5;
		if (!same) {
			const char* error = nullptr;
			if (controller_.time().set_location(plan.location, &error)) {
				Log::info(kTag, "location set by the image: %.6f, %.6f, %.0f m", plan.location.latitude,
					plan.location.longitude, plan.location.elevation_m);
			} else {
				Log::error(kTag, "the image's location was not stored: %s", error != nullptr ? error : "?");
			}
		}
	}
	if (plan.ntp != nullptr) {
		char current[Settings::kMaxStringLength + 1] = {};
		if (settings.get_string(setting::kNtpServer, current, sizeof(current)) != SettingsStatus::ok ||
			std::strcmp(current, plan.ntp) != 0) {
			SettingsStatus status = settings.set_string(setting::kNtpServer, plan.ntp);
			if (status == SettingsStatus::ok) {
				Log::info(kTag, "NTP server set by the image: %s", plan.ntp);
				if (ntp_changed_) {
					ntp_changed_();
				}
			} else {
				Log::error(kTag, "the image's NTP server was not stored: %s", settings_status_name(status));
			}
		}
	}
}

struct ImageService::Composed {
	struct Part {
		DynamicSpecProvider* provider;
		const DynamicSpec* specs;
		size_t first;
		size_t count;
	};

	Arena arena;
	const DatapointSpec* specs = nullptr; ///< The plan's own table when the providers add none.
	size_t count = 0;
	size_t image_count = 0;
	Part parts[kMaxProviders] = {};
	size_t part_count = 0;
};

ImageBindings ImageService::bindings_of(const Plan* plan) const {
	ImageBindings bindings;
	if (plan != nullptr) {
		bindings.zigbee = plan->zigbee;
		bindings.zigbee_count = plan->zigbee_count;
	}
	return bindings;
}

bool ImageService::compose(const Plan* plan, Composed& out, Report& report) {
	out.image_count = plan != nullptr ? plan->datapoint_count : 0;
	size_t total = out.image_count;
	ImageBindings bindings = bindings_of(plan);
	for (size_t i = 0; i < provider_count_; ++i) {
		DynamicSpecProvider* provider = providers_[i];
		const DynamicSpec* specs = nullptr;
		size_t count = 0;
		const char* error = nullptr;
		if (!provider->dynamic_specs(bindings, specs, count, &error)) {
			report.add(Report::Kind::no_memory, "", "the provider '%s' cannot build its datapoints: %s",
				provider->provider_name(), error != nullptr ? error : "?");
			return false;
		}
		out.parts[out.part_count++] = Composed::Part{provider, count > 0 ? specs : nullptr, total, count};
		total += count;
	}
	if (total > DatapointModel::kMaxDatapoints) {
		report.add(Report::Kind::apply, "", "the image and the providers need %u datapoints, more than the model holds",
			static_cast<unsigned>(total));
		return false;
	}
	if (total == out.image_count) {
		out.specs = out.image_count > 0 ? plan->datapoints : nullptr;
		out.count = total;
		return true;
	}
	DatapointSpec* composed = out.arena.array<DatapointSpec>(total);
	uint32_t* order = out.arena.array<uint32_t>(total);
	if (total > 0 && (composed == nullptr || order == nullptr)) {
		report.add(Report::Kind::no_memory, "", "no memory to compose %u datapoints", static_cast<unsigned>(total));
		return false;
	}
	out.specs = composed;
	for (size_t i = 0; i < out.image_count; ++i) {
		composed[i] = plan->datapoints[i];
	}
	bool ok = true;
	for (size_t p = 0; p < out.part_count; ++p) {
		const Composed::Part& part = out.parts[p];
		for (size_t k = 0; k < part.count; ++k) {
			composed[part.first + k] = part.specs[k].spec;
			if (part.specs[k].spec.name == nullptr) {
				report.add(Report::Kind::apply, "", "the provider '%s' gave a datapoint without a name",
					part.provider->provider_name());
				ok = false;
			}
		}
	}
	out.count = total;
	if (!ok) {
		return false;
	}
	for (size_t i = 0; i < total; ++i) {
		order[i] = static_cast<uint32_t>(i);
	}
	const DatapointSpec* specs = out.specs;
	std::sort(order, order + total, [specs](uint32_t a, uint32_t b) {
		int cmp = std::strcmp(specs[a].name, specs[b].name);
		return cmp < 0 || (cmp == 0 && a < b);
	});
	auto provider_of = [&out](uint32_t index) -> const char* {
		for (size_t p = 0; p < out.part_count; ++p) {
			if (index >= out.parts[p].first && index < out.parts[p].first + out.parts[p].count) {
				return out.parts[p].provider->provider_name();
			}
		}
		return "?";
	};
	for (size_t k = 1; k < total; ++k) {
		uint32_t a = order[k - 1];
		uint32_t b = order[k];
		if (std::strcmp(specs[a].name, specs[b].name) != 0) {
			continue;
		}
		ok = false;
		if (a < out.image_count) {
			char path[32];
			std::snprintf(path, sizeof(path), "datapoints[%u].name", static_cast<unsigned>(a));
			report.add(Report::Kind::apply, path, "'%s' is also a datapoint of the provider '%s'", specs[a].name,
				provider_of(b));
		} else {
			report.add(Report::Kind::apply, "", "'%s' is a datapoint of the provider '%s' and of the provider '%s'",
				specs[a].name, provider_of(a), provider_of(b));
		}
	}
	return ok;
}

bool ImageService::NoZigbee::command(const Datapoint& datapoint, const Value& value) {
	(void)value;
	Log::warn(kTag, "command to '%s' refused: it is bound to Zigbee, and this platform has no Zigbee adapter",
		datapoint.name());
	return false;
}

bool ImageService::NoZigbee::read(const Datapoint& datapoint) {
	Log::warn(kTag, "read of '%s' refused: it is bound to Zigbee, and this platform has no Zigbee adapter",
		datapoint.name());
	return false;
}

void ImageService::bind_providers(const Plan* plan, const Composed& composed) {
	for (size_t p = 0; p < composed.part_count; ++p) {
		const Composed::Part& part = composed.parts[p];
		for (size_t k = 0; k < part.count; ++k) {
			Technology* technology = part.specs[k].technology;
			if (technology != nullptr &&
				model_.bind(static_cast<uint16_t>(part.first + k), *technology) != DatapointStatus::ok) {
				Log::error(kTag, "datapoint '%s' of the provider '%s' not bound to %s (the model logged why)",
					part.specs[k].spec.name, part.provider->provider_name(), technology->name());
			}
		}
	}
	dynamic_count_ = composed.count - composed.image_count;
	ImageBindings bindings = bindings_of(plan);
	for (size_t i = 0; i < provider_count_; ++i) {
		providers_[i]->model_loaded(bindings);
	}
	// What no provider took (no Zigbee adapter on this platform): absent, refusing commands.
	size_t orphans = 0;
	for (size_t i = 0; i < bindings.zigbee_count; ++i) {
		const Datapoint* dp = model_.find(bindings.zigbee[i].name);
		if (dp != nullptr && dp->technology() == nullptr && model_.bind(dp->id(), no_zigbee_) == DatapointStatus::ok) {
			++orphans;
		}
	}
	if (orphans > 0) {
		Log::warn(kTag, "%u Zigbee-bound datapoint(s) without a Zigbee adapter on this platform: no device values, "
			"commands refused", static_cast<unsigned>(orphans));
	}
	last_model_load_ms_ = controller_.clock().monotonic_ms();
	model_loaded_once_ = true;
}

bool ImageService::bind_persistence(const Plan& plan, Source source) {
	DatapointStatus status = value_store_.load();
	if (status != DatapointStatus::ok) {
		return fail_applied(source, "datapoints", "the value store refused: %s", datapoint_status_name(status));
	}
	HistoryStatus history_status = history_.load(plan.history, plan.history_count, plan.history_budget, plan.archive_budget);
	if (history_status != HistoryStatus::ok) {
		return fail_applied(source, "history", "the history refused: %s", history_status_name(history_status));
	}
	return true;
}

bool ImageService::bind_technologies(const Plan& plan, Source source) {
	const char* error = nullptr;
	if (!adapter_.configure(plan.mappings, plan.mapping_count, &error)) {
		return fail_applied(source, "knx.bindings", "the KNX adapter refused the mapping: %s", error);
	}
	if (plan.has_time_master) {
		if (!time_master_.configure(plan.time_master, &error)) {
			return fail_applied(source, "knx.timeMaster", "the time master refused: %s", error);
		}
		size_t total = plan.mapping_count + knx::TimeMaster::kMaxOutputs;
		knx::KnxMapping* merged = new (std::nothrow) knx::KnxMapping[total];
		if (merged == nullptr) {
			return fail_applied(source, "knx.bindings", "no memory for the merged KNX mapping");
		}
		for (size_t i = 0; i < plan.mapping_count; ++i) {
			merged[i] = plan.mappings[i];
		}
		size_t n = plan.mapping_count + time_master_.mappings(merged + plan.mapping_count, knx::TimeMaster::kMaxOutputs);
		bool configured = adapter_.configure(merged, n, &error);
		delete[] merged;
		if (!configured) {
			return fail_applied(source, "knx.timeMaster", "the KNX adapter refused the time master's mapping: %s", error);
		}
	} else {
		time_master_.clear();
	}
	ScheduleStatus schedule_status = scheduler_.load(plan.schedules, plan.schedule_count);
	if (schedule_status != ScheduleStatus::ok) {
		return fail_applied(source, "schedules", "the scheduler refused: %s", schedule_status_name(schedule_status));
	}
	engine_.quietly([this] { scheduler_.publish_times(); });
	if (external_ != nullptr) {
		if (plan.external_count > 0) {
			if (!external_->configure(plan.external, plan.external_count, &error)) {
				return fail_applied(source, "external.sources", "the external values refused: %s", error);
			}
		} else {
			external_->clear();
		}
	}
	return true;
}

bool ImageService::apply(const Plan& constant_plan, Source source) {
	Plan& plan = const_cast<Plan&>(constant_plan);
	Composed composed;
	if (!compose(&plan, composed, report_)) {
		log_report(report_, source, false, false);
		blocks_.clear(false);
		if (provider_count_ > 0) {
			reload_dynamic("no image");
			restore_values(swap_);
		}
		if (changed_) {
			changed_();
		}
		return false;
	}
	phase("compose");
	DatapointStatus status = model_.load(composed.specs, composed.count);
	if (status != DatapointStatus::ok) {
		report_.add(status == DatapointStatus::out_of_memory ? Report::Kind::no_memory : Report::Kind::apply,
			"datapoints", "the datapoint model refused the image: %s", datapoint_status_name(status));
		log_report(report_, source, false, false);
		blocks_.clear(false);
		if (provider_count_ > 0) {
			reload_dynamic("no image");
			restore_values(swap_);
		}
		if (changed_) {
			changed_();
		}
		return false;
	}
	// The values of the swap (spec 12): a datapoint whose id hash and value type are in the table.
	if (swap_ != nullptr && swap_->count > 0) {
		size_t carried = restore_values(swap_);
		Log::info(kTag, "swap: %u of %u kept values found their datapoint", static_cast<unsigned>(carried),
			static_cast<unsigned>(swap_->count));
	}
	phase("model");
	bind_providers(&plan, composed);
	if (reload_pending_) {
		controller_.loop().cancel(reload_timer_);
		reload_pending_ = false;
	}
	apply_settings(plan);
	if (!bind_persistence(plan, source)) {
		return false;
	}
	phase("persistence");
	rules::LoadResult result = engine_.adopt(std::move(plan.rule_set), swap_ != nullptr ? swap_->carry : nullptr);
	if (!result.ok()) {
		Report::Kind kind = result.error == rules::LoadError::out_of_memory ? Report::Kind::no_memory : Report::Kind::apply;
		fail_applied(source, result.path, "rule '%s': %s: %s", result.rule, rules::load_error_name(result.error),
			result.text);
		report_.kind = kind;
		return false;
	}
	phase("engine");
	if (!blocks_.adopt(std::move(plan.block_set))) {
		fail_applied(source, "blocks", "no memory for the blocks' runtime");
		report_.kind = Report::Kind::no_memory;
		return false;
	}
	phase("block runtime");
	if (!bind_technologies(plan, source)) {
		return false;
	}
	phase("technologies");
	info_ = plan.info;
	source_ = source;
	loaded_ = true;
	has_knx_ = plan.mapping_count > 0 || plan.has_time_master;
	loaded_ms_ = controller_.clock().monotonic_ms();
	accept_dynamic();
	rules::StartReason reason = rules_started_ ? rules::StartReason::image : rules::StartReason::boot;
	rules_started_ = true;
	engine_.start(reason);
	Log::info(kTag, "image applied; rules started (%s)", rules::start_reason_name(reason));
	phase("start");
	return true;
}

const Report::Refusal* ImageService::dynamic_refusal() const {
	static const Report::Refusal kNoMemory = {"", "no memory to keep the reason of the refused reload"};
	if (!dynamic_refused_) {
		return nullptr;
	}
	return dynamic_refusal_ != nullptr ? dynamic_refusal_ : &kNoMemory;
}

void ImageService::refuse_dynamic(const char* path, const char* format, ...) {
	dynamic_refused_ = true;
	if (dynamic_refusal_ == nullptr) {
		dynamic_refusal_ = new (std::nothrow) Report::Refusal;
		if (dynamic_refusal_ == nullptr) {
			return;
		}
	}
	std::snprintf(dynamic_refusal_->path, sizeof(dynamic_refusal_->path), "%s", path);
	va_list args;
	va_start(args, format);
	std::vsnprintf(dynamic_refusal_->reason, sizeof(dynamic_refusal_->reason), format, args);
	va_end(args);
}

void ImageService::accept_dynamic() {
	dynamic_refused_ = false;
	delete dynamic_refusal_;
	dynamic_refusal_ = nullptr;
}

bool ImageService::reload_dynamic(const char* why) {
	if (model_.notifying()) {
		Log::error(kTag, "model reload (%s) refused from a datapoint listener", why);
		return false;
	}
	// The loaded image's tables, built again from the slot for this reload and freed with it.
	struct Tables {
		Plan* plan = nullptr;
		~Tables() { delete plan; }
	} tables;
	if (loaded_) {
		tables.plan = new (std::nothrow) Plan();
		Report refusals;
		Loader loader(controller_.clock(), history_limits_.cap_bytes);
		loader.set_archive_cap(history_limits_.archive_cap_bytes);
		loader.set_tables_only(true);
		if (tables.plan == nullptr || !loader.build(view_, *tables.plan, refusals)) {
			refuse_dynamic("", "no memory for the image's tables");
			Log::error(kTag, "model not reloaded (%s): the image's tables could not be built", why);
			return false;
		}
	}
	const Plan* plan = tables.plan;
	Report* report = new (std::nothrow) Report();
	if (report == nullptr) {
		Log::error(kTag, "model not reloaded (%s): no memory", why);
		return false;
	}
	Composed composed;
	if (!compose(plan, composed, *report)) {
		if (report->count > 0) {
			refuse_dynamic(report->refusals[0].path, "%s", report->refusals[0].reason);
		} else {
			refuse_dynamic("", "the datapoints do not compose");
		}
		Log::warn(kTag, "model not reloaded (%s): the datapoints do not compose; the model stays as it is", why);
		for (size_t i = 0; i < report->count; ++i) {
			Log::warn(kTag, "  %s: %s", report->refusals[i].path[0] != '\0' ? report->refusals[i].path : "(providers)",
				report->refusals[i].reason);
		}
		delete report;
		return false;
	}
	delete report;
	if (loaded_) {
		engine_.flush();
	}
	DatapointStatus status = model_.load(composed.specs, composed.count, true);
	if (status != DatapointStatus::ok) {
		refuse_dynamic("", "the datapoint model refused the reload: %s", datapoint_status_name(status));
		Log::error(kTag, "model not reloaded (%s): %s; the model stays as it is", why, dynamic_refusal()->reason);
		return false;
	}
	bind_providers(plan, composed);
	if (loaded_) {
		// The load dropped the engine's binding of the parameter datapoints and the block runtime's
		// of its mirrors.
		engine_.bind_parameters();
		blocks_.bind();
	}
	accept_dynamic();
	++dynamic_reloads_;
	if (plan != nullptr && (!bind_persistence(*plan, source_) || !bind_technologies(*plan, source_))) {
		return false; // torn down (fail_applied)
	}
	Log::info(kTag, "model reloaded (%s): %u datapoint(s) of the image, %u of the providers", why,
		static_cast<unsigned>(composed.image_count), static_cast<unsigned>(dynamic_count_));
	if (mqtt_ != nullptr && loaded_) {
		const char* mqtt_error = nullptr;
		if (!mqtt_->model_reloaded(&mqtt_error)) {
			Log::error(kTag, "the MQTT client did not follow the model reload: %s", mqtt_error != nullptr ? mqtt_error : "?");
		}
	}
	if (changed_) {
		changed_();
	}
	return true;
}

void ImageService::reload_due() {
	reload_pending_ = false;
	reload_dynamic("a provider's datapoints changed");
}

void ImageService::dynamic_specs_changed(DynamicSpecProvider& provider) {
	if (!started_) {
		return; // start() composes them
	}
	if (reload_pending_) {
		return; // coalesced
	}
	uint64_t now = controller_.clock().monotonic_ms();
	uint64_t at = now;
	if (model_loaded_once_ && last_model_load_ms_ + kReloadSpacingMs > now) {
		at = last_model_load_ms_ + kReloadSpacingMs;
	}
	reload_timer_ = controller_.loop().schedule_at(at, Callback::member<ImageService, &ImageService::reload_due>(this, "image.reload_due"));
	reload_pending_ = reload_timer_.valid();
	if (!reload_pending_) {
		Log::error(kTag, "the datapoints of '%s' changed but no timer is free for the reload", provider.provider_name());
		return;
	}
	Log::debug(kTag, "the datapoints of '%s' changed: model reload in %u ms", provider.provider_name(),
		static_cast<unsigned>(at - now));
}

bool ImageService::add_provider(DynamicSpecProvider& provider) {
	for (size_t i = 0; i < provider_count_; ++i) {
		if (providers_[i] == &provider) {
			Log::error(kTag, "provider '%s' registered twice", provider.provider_name());
			return false;
		}
	}
	if (provider_count_ >= kMaxProviders || provider.has_spec_listener()) {
		Log::error(kTag, "provider '%s' refused: %s", provider.provider_name(),
			provider.has_spec_listener() ? "it has a listener already" : "no provider slot left");
		return false;
	}
	providers_[provider_count_++] = &provider;
	provider.set_spec_listener(this);
	Log::info(kTag, "provider '%s' of dynamic datapoints registered", provider.provider_name());
	if (started_) {
		dynamic_specs_changed(provider);
	}
	return true;
}

bool ImageService::remove_provider(DynamicSpecProvider& provider) {
	size_t at = provider_count_;
	for (size_t i = 0; i < provider_count_; ++i) {
		if (providers_[i] == &provider) {
			at = i;
		}
	}
	if (at == provider_count_) {
		return false;
	}
	for (size_t i = at + 1; i < provider_count_; ++i) {
		providers_[i - 1] = providers_[i];
	}
	providers_[--provider_count_] = nullptr;
	provider.set_spec_listener(nullptr);
	provider.model_cleared();
	if (started_) {
		reload_dynamic("a provider was removed");
	}
	return true;
}

bool ImageService::fail_applied(Source source, const char* path, const char* format, ...) {
	char reason[Report::kMaxReason + 1];
	va_list args;
	va_start(args, format);
	std::vsnprintf(reason, sizeof(reason), format, args);
	va_end(args);
	char where[Report::kMaxPath + 1];
	std::snprintf(where, sizeof(where), "%s", path != nullptr ? path : "");
	Log::error(kTag, "image from %s failed while it was applied at %s: %s; the device has no image", source_name(source),
		where, reason);
	tear_down();
	if (provider_count_ > 0) {
		reload_dynamic("no image after a failed apply");
		restore_values(swap_);
	}
	report_.clear();
	report_.add(Report::Kind::apply, where, "%s", reason);
	if (changed_) {
		changed_();
	}
	return false;
}

void ImageService::tear_down() {
	// The persisted state stays for the image deployed next; so does the swap table (I9).
	engine_.flush();
	rules::Engine::free_carry(engine_.detach()); // a swap table of an image before stays (I9)
	blocks_.clear(false);
	adapter_.clear();
	time_master_.clear();
	scheduler_.clear();
	if (external_ != nullptr) {
		external_->clear();
	}
	if (mqtt_ != nullptr) {
		mqtt_->clear();
	}
	model_.clear();
	for (size_t i = 0; i < provider_count_; ++i) {
		providers_[i]->model_cleared();
	}
	dynamic_count_ = 0;
	model_view_.clear();
	view_.close();
	if (mapped_ != nullptr) {
		slot_.unmap();
		mapped_ = nullptr;
	}
	loaded_ = false;
	has_knx_ = false;
	info_ = ImageInfo();
	++token_;
}

bool ImageService::unload(const char** error) {
	if (receiving()) {
		*error = "an image is being received: remove it in a moment";
		Log::warn(kTag, "unload refused: an image is being received");
		return false;
	}
	bool had = loaded_;
	size_t length = loaded_ ? info_.bytes : 0;
	engine_.flush();
	drop_swap();
	SwapTable* values = provider_count_ > 0 ? capture_values(nullptr) : nullptr; // the providers' stay
	tear_down();
	blocks_.clear(true); // with their persisted records and overrides
	scheduler_.load(nullptr, 0);
	value_store_.load();
	history_.load(nullptr, 0, 0);
	if (provider_count_ > 0) {
		reload_dynamic("the image is removed");
		restore_values(values);
	}
	delete values;
	report_.clear();
	if (changed_) {
		changed_();
	}
	if (length == 0) {
		length = ImageSlot::kSector; // whatever the slot holds: its header goes
	}
	if (!slot_.erase(length, error)) {
		Log::error(kTag, "the image slot could not be erased: %s", *error != nullptr ? *error : "?");
		return false;
	}
	Log::info(kTag, "image %s: the device has no image (persisted values, rings and overrides removed)",
		had ? "unloaded" : "cleared");
	return true;
}

void ImageService::flush() {
	value_store_.flush();
	history_.flush();
	blocks_.flush(); // the blocks' state records (spec/blocks.md 8.1)
	engine_.persist_now();
}

bool ImageService::image_degraded() const {
	return loaded_ && has_knx_ && (tunnel_ == nullptr || !tunnel_->is_up());
}

void ImageService::schedule_fired(const char* id) {
	engine_.schedule_fired(id);
}

void ImageService::quietly(void (*report)(void* ctx), void* ctx) {
	engine_.quietly([report, ctx] { report(ctx); });
}

namespace {

void write_crc(JsonWriter& json, const char* name, uint32_t crc) {
	char text[12];
	std::snprintf(text, sizeof(text), "%08x", static_cast<unsigned>(crc));
	json.member(name, text);
}

void write_format(JsonWriter& json, const char* name, uint16_t major, uint16_t minor) {
	char text[16];
	std::snprintf(text, sizeof(text), "%u.%u", static_cast<unsigned>(major), static_cast<unsigned>(minor));
	json.member(name, text);
}

} // namespace

void ImageService::write_status(JsonWriter& json, bool full) const {
	(void)full;
	using namespace status_text;
	json.member("loaded", loaded_);
	json.member("slot", static_cast<uint32_t>(slot_.size()));
	auto write_loaded = [&]() {
		json.member("project", info_.project_id, kName)
			.member("revision", info_.revision)
			.member("name", info_.name, kName)
			.member("source", source_name(source_), kCode);
		char format[16];
		std::snprintf(format, sizeof(format), "%u.%u", static_cast<unsigned>(info_.format_major),
			static_cast<unsigned>(info_.format_minor));
		json.member("format", format, kTiny);
		json.member("schemaVersion", info_.schema_version, kShort).member("bytes", static_cast<uint32_t>(info_.bytes));
		char crc[12];
		std::snprintf(crc, sizeof(crc), "%08x", static_cast<unsigned>(info_.crc));
		json.member("crc", crc, kTiny);
		json.member("datapoints", static_cast<uint32_t>(info_.datapoints))
			.member("rules", static_cast<uint32_t>(info_.rules))
			.member("schedules", static_cast<uint32_t>(info_.schedules))
			.member("blocks", static_cast<uint32_t>(info_.blocks))
			.member("degraded", image_degraded());
	};
	auto write_refused = [&]() {
		json.member("refused", report_kind_code(report_.kind), kCode);
		if (json.bounding()) {
			json.member("reason", "", kText); // counted at its limit
		} else if (report_.count > 0) {
			char reason[Report::kMaxPath + Report::kMaxReason + 4];
			std::snprintf(reason, sizeof(reason), "%s: %s", report_.refusals[0].path, report_.refusals[0].reason);
			json.member("reason", reason, kText);
		}
	};
	if (json.bounding()) {
		// The larger of the exclusive branches (receiving is a boolean).
		JsonWriter::Mark start = json.mark();
		write_refused();
		size_t refused = json.length();
		json.reset(start);
		write_loaded();
		if (refused > json.length()) {
			json.reset(start);
			write_refused();
		}
	} else if (loaded_) {
		write_loaded();
	} else if (receiving()) {
		json.member("receiving", true);
	} else if (!report_.ok()) {
		write_refused();
	}
}

void ImageService::write_info(JsonWriter& json) const {
	write_info_of(json, info_, source_);
}

void ImageService::write_info_of(JsonWriter& json, const ImageInfo& info, Source source) {
	json.member("ok", true);
	json.key("image").begin_object();
	write_format(json, "format", info.format_major, info.format_minor);
	json.member("schemaVersion", info.schema_version)
		.member("projectId", info.project_id)
		.member("revision", info.revision)
		.member("generatedAt", info.generated_at)
		.member("generator", info.generator)
		.member("name", info.name)
		.member("bytes", static_cast<uint32_t>(info.bytes));
	write_crc(json, "crc", info.crc);
	json.member("signed", info.is_signed).end_object();
	json.key("counts").begin_object()
		.member("datapoints", static_cast<uint32_t>(info.datapoints))
		.member("knx", static_cast<uint32_t>(info.knx_datapoints))
		.member("rules", static_cast<uint32_t>(info.rules))
		.member("schedules", static_cast<uint32_t>(info.schedules))
		.member("locations", static_cast<uint32_t>(info.locations))
		.member("functions", static_cast<uint32_t>(info.functions))
		.member("historyRings", static_cast<uint32_t>(info.history_rings))
		.member("timeMaster", info.time_master)
		.end_object();
	json.member("source", source_name(source));
}

void ImageService::write_report(JsonWriter& json) const {
	json.member("error", report_text(report_.kind)).member("code", report_kind_code(report_.kind));
	json.key("refusals").begin_array();
	for (size_t i = 0; i < report_.count; ++i) {
		json.begin_object()
			.member("path", report_.refusals[i].path)
			.member("reason", report_.refusals[i].reason)
			.end_object();
	}
	json.end_array();
	json.member("total", static_cast<uint32_t>(report_.total));
	json.member("loaded", loaded_);
	if (loaded_) {
		json.member("revision", info_.revision);
	}
}

} // namespace image
} // namespace hc
