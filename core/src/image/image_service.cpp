/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/image/image_service.h"

#include "hc/mqtt.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <new>

#include "hc/json_document.h"
#include "hc/network_settings.h"

namespace hc {
namespace image {

namespace {

constexpr const char* kTag = "hc.image";

/// The `rules` of an image without rules.
constexpr char kNoRules[] = "[]";

struct PlanSchedules : rules::ScheduleDirectory {
	explicit PlanSchedules(const Plan& p) : plan(p) {}
	bool has_schedule(const char* id) const override { return plan.has_schedule(id); }
	bool owns_output(const char* datapoint) const override { return plan.schedule_owns_output(datapoint); }
	bool technology_reads(const char* datapoint) const override { return plan.technology_reads(datapoint); }
	const Plan& plan;
};

struct NoSchedules : rules::ScheduleDirectory {
	bool has_schedule(const char*) const override { return false; }
};

const char* report_code(Report::Kind kind) {
	switch (kind) {
		case Report::Kind::syntax: return "image.bad_json";
		case Report::Kind::version: return "image.version";
		case Report::Kind::too_large: return "image.too_large";
		case Report::Kind::no_memory: return "image.no_memory";
		case Report::Kind::apply: return "image.apply_failed";
		default: return "image.invalid";
	}
}

const char* report_text(Report::Kind kind) {
	switch (kind) {
		case Report::Kind::syntax: return "the image is not valid JSON";
		case Report::Kind::version: return "the image's schema version is newer than this firmware reads";
		case Report::Kind::too_large: return "the image is larger than the device accepts";
		case Report::Kind::no_memory: return "the device has not enough memory for the image";
		case Report::Kind::apply: return "the image was valid but could not be applied";
		default: return "the image is invalid";
	}
}

} // namespace

ImageService::ImageService(Controller& controller, DatapointModel& model, BlobStore& blobs,
	knx::KnxAdapter& adapter, const knx::KnxTunnel* tunnel, const HistoryLimits& history_limits,
	const ValueStoreLimits& value_limits)
	: controller_(controller),
	  model_(model),
	  adapter_(adapter),
	  tunnel_(tunnel),
	  history_limits_(history_limits),
	  rules_clock_(controller.time()),
	  scheduler_(controller.loop(), controller.time(), controller.settings(), &model),
	  engine_(controller.loop(), model, controller.clock(), controller.settings(), rules_clock_),
	  value_store_(controller.loop(), controller.clock(), model, blobs, value_limits),
	  history_(controller.loop(), controller.clock(), model, blobs, history_limits),
	  time_master_(controller.loop(), controller.time(), model, adapter),
	  store_(blobs) {}

ImageService::~ImageService() {
	if (reload_pending_) {
		controller_.loop().cancel(reload_timer_);
	}
	for (size_t i = 0; i < provider_count_; ++i) {
		providers_[i]->set_spec_listener(nullptr);
	}
	delete plan_;
	delete dynamic_refusal_;
	if (started_) {
		controller_.set_led_input(nullptr);
		controller_.time().remove_listener(scheduler_);
		controller_.time().remove_listener(time_master_);
		scheduler_.remove_listener(*this);
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
	if (!controller_.time().add_listener(time_master_, knx::TimeMaster::kTimeEvents)) {
		Log::error(kTag, "the KNX time master will not broadcast: no time listener slot");
	}
	scheduler_.add_listener(*this);
	controller_.set_led_input(this);
	store_.collect_garbage();
	size_t bytes = 0;
	if (!store_.stored(bytes)) {
		Log::info(kTag, "no runtime image stored: PUT /api/image deploys one (doc/image.md)");
	} else {
		load_stored(Source::boot);
	}
	if (!loaded_ && provider_count_ > 0) {
		reload_dynamic("start without an image");
	}
}

bool ImageService::load_stored(Source source) {
	size_t bytes = 0;
	if (!store_.stored(bytes)) {
		return false;
	}
	char* text = new (std::nothrow) char[bytes > 0 ? bytes : 1];
	if (text == nullptr) {
		report_.clear();
		report_.add(Report::Kind::no_memory, "", "no memory to read the stored image (%u bytes)",
			static_cast<unsigned>(bytes));
		Log::error(kTag, "stored image not loaded: no memory for its %u bytes", static_cast<unsigned>(bytes));
		return false;
	}
	size_t length = 0;
	bool missing = false;
	const char* error = nullptr;
	bool ok = store_.read(text, bytes, length, missing, &error);
	if (!ok) {
		report_.clear();
		report_.add(Report::Kind::invalid, "", "the stored image cannot be read: %s", error);
		Log::error(kTag, "stored image not loaded: %s", error);
	} else {
		ok = load(text, length, source, false);
		if (ok) {
			stored_ok_ = true;
		}
	}
	delete[] text;
	return ok;
}

bool ImageService::load(const char* text, size_t length, Source source, bool store) {
	uint64_t begin = controller_.clock().monotonic_ms();
	Plan* plan = new (std::nothrow) Plan();
	if (plan == nullptr) {
		report_.clear();
		report_.add(Report::Kind::no_memory, "", "no memory to start the load");
		Log::error(kTag, "image (%s) not loaded: no memory", source_name(source));
		return false;
	}
	Loader loader(controller_.clock(), history_limits_.cap_bytes);
	if (!loader.parse(text, length, *plan, report_)) {
		Log::warn(kTag, "image from %s refused (%s): %u problem%s; the %s", source_name(source),
			report_kind_name(report_.kind), static_cast<unsigned>(report_.total), report_.total == 1 ? "" : "s",
			loaded_ ? "loaded image stays" : "device keeps no image");
		for (size_t i = 0; i < report_.count; ++i) {
			Log::warn(kTag, "  %s: %s", report_.refusals[i].path[0] != '\0' ? report_.refusals[i].path : "(image)",
				report_.refusals[i].reason);
		}
		delete plan;
		return false;
	}
	size_t arena = plan->arena.bytes();
	size_t arena_used = plan->arena.used();
	bool ok = apply(*plan, source);
	if (plan != plan_) {
		delete plan; // not kept for the providers' reloads
	}
	if (!ok) {
		return false;
	}
	// 7c: the MQTT client, from the applied text (its datapoints are resolved by name against the
	// model just loaded; without an `mqtt` member it is cleared). The loader checked the member, so
	// a refusal here is a resource problem: logged, and MQTT stands down rather than keep the old image's.
	if (mqtt_ != nullptr) {
		const char* mqtt_error = nullptr;
		if (!mqtt_->configure_image(text, length, &mqtt_error)) {
			Log::error(kTag, "the MQTT client refused the image's mqtt member: %s; MQTT is off until the next load",
				mqtt_error != nullptr ? mqtt_error : "?");
			mqtt_->clear();
		}
	}
	report_.clear();
	view_.build(text, length, model_);
	if (store) {
		const char* error = nullptr;
		stored_ok_ = store_.write(text, length, &error);
		std::snprintf(store_error_, sizeof(store_error_), "%s", stored_ok_ ? "" : error);
		if (!stored_ok_) {
			Log::error(kTag, "the image runs but is not stored (%s): after a restart the previous one loads", error);
		}
	} else if (source != Source::boot && source != Source::restore) {
		stored_ok_ = false;
		std::snprintf(store_error_, sizeof(store_error_), "not stored");
	}
	Log::info(kTag, "image '%s' revision %u (%s, schema %s) loaded from %s in %u ms: %u datapoints (%u KNX), %u rules, "
		"%u schedules, %u functions, %u locations, %u history rings%s; %u bytes, load tables %u of %u bytes used%s, "
		"UI model %u of %u bytes used",
		info_.project_id, static_cast<unsigned>(info_.revision), info_.name[0] != '\0' ? info_.name : "unnamed",
		info_.schema_version, source_name(source), static_cast<unsigned>(controller_.clock().monotonic_ms() - begin),
		static_cast<unsigned>(info_.datapoints), static_cast<unsigned>(info_.knx_datapoints),
		static_cast<unsigned>(info_.rules), static_cast<unsigned>(info_.schedules),
		static_cast<unsigned>(info_.functions), static_cast<unsigned>(info_.locations),
		static_cast<unsigned>(info_.history_rings), info_.time_master ? ", time master" : "",
		static_cast<unsigned>(length), static_cast<unsigned>(arena_used), static_cast<unsigned>(arena),
		plan_ != nullptr ? " (kept for the providers' reloads)" : "", static_cast<unsigned>(view_.used()),
		static_cast<unsigned>(view_.bytes()));
	if (info_.zigbee_datapoints > 0 || dynamic_count_ > 0) {
		Log::info(kTag, "  %u Zigbee binding(s); %u datapoint(s) of the providers", static_cast<unsigned>(info_.zigbee_datapoints),
			static_cast<unsigned>(dynamic_count_));
	}
	if (changed_) {
		changed_();
	}
	return true;
}

void ImageService::apply_settings(const Plan& plan) {
	Settings& settings = controller_.settings();
	if (plan.has_tz && std::strcmp(controller_.time_zone(), plan.tz) != 0) {
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
	if (plan.has_ntp) {
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
	DatapointSpec* specs = nullptr;
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
		report.add(Report::Kind::invalid, "", "the image and the providers need %u datapoints, more than the model holds",
			static_cast<unsigned>(total));
		return false;
	}
	out.specs = out.arena.array<DatapointSpec>(total);
	uint32_t* order = out.arena.array<uint32_t>(total);
	if (total > 0 && (out.specs == nullptr || order == nullptr)) {
		report.add(Report::Kind::no_memory, "", "no memory to compose %u datapoints", static_cast<unsigned>(total));
		return false;
	}
	for (size_t i = 0; i < out.image_count; ++i) {
		out.specs[i] = plan->datapoints[i];
	}
	bool ok = true;
	for (size_t p = 0; p < out.part_count; ++p) {
		const Composed::Part& part = out.parts[p];
		for (size_t k = 0; k < part.count; ++k) {
			out.specs[part.first + k] = part.specs[k].spec;
			if (part.specs[k].spec.name == nullptr) {
				report.add(Report::Kind::invalid, "", "the provider '%s' gave a datapoint without a name",
					part.provider->provider_name());
				ok = false;
			}
		}
	}
	out.count = total;
	if (!ok) {
		return false;
	}
	// Names are unique across the image and the providers.
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
			std::snprintf(path, sizeof(path), "datapoints[%u].id", static_cast<unsigned>(a));
			report.add(Report::Kind::invalid, path, "'%s' is also a datapoint of the provider '%s'", specs[a].name,
				provider_of(b));
		} else {
			report.add(Report::Kind::invalid, "", "'%s' is a datapoint of the provider '%s' and of the provider '%s'",
				specs[a].name, provider_of(a), provider_of(b));
		}
	}
	return ok;
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
	last_model_load_ms_ = controller_.clock().monotonic_ms();
	model_loaded_once_ = true;
}

bool ImageService::bind_persistence(const Plan& plan, Source source) {
	DatapointStatus status = value_store_.load();
	if (status != DatapointStatus::ok) {
		return fail_applied(source, "datapoints", "the value store refused: %s", datapoint_status_name(status));
	}
	HistoryStatus history_status = history_.load(plan.history, plan.history_count, plan.history_budget);
	if (history_status != HistoryStatus::ok) {
		return fail_applied(source, "history", "the history refused: %s", history_status_name(history_status));
	}
	return true;
}

bool ImageService::bind_technologies(const Plan& plan, Source source) {
	// 6: KNX.
	const char* error = nullptr;
	if (!adapter_.configure(plan.mappings, plan.mapping_count, &error)) {
		return fail_applied(source, "datapoints", "the KNX adapter refused the mapping: %s", error);
	}
	if (plan.has_time_master) {
		if (!time_master_.configure(plan.time_master, &error)) {
			return fail_applied(source, "knx.timeMaster", "the time master refused: %s", error);
		}
		size_t total = plan.mapping_count + knx::TimeMaster::kMaxOutputs;
		knx::KnxMapping* merged = new (std::nothrow) knx::KnxMapping[total];
		if (merged == nullptr) {
			return fail_applied(source, "knx", "no memory for the merged KNX mapping");
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

	// 7: schedules.
	ScheduleStatus schedule_status = scheduler_.load(plan.schedules, plan.schedule_count);
	if (schedule_status != ScheduleStatus::ok) {
		return fail_applied(source, "schedules", "the scheduler refused: %s", schedule_status_name(schedule_status));
	}

	// 7b: external values, after the model they bind to.
	if (external_ != nullptr) {
		if (plan.has_external) {
			if (!external_->configure_json(plan.external.data, plan.external.length, &error)) {
				return fail_applied(source, "external", "the external values refused: %s", error);
			}
		} else {
			external_->clear();
		}
	}
	return true;
}

bool ImageService::apply(Plan& plan, Source source) {
	report_.clear();
	// With providers the plan is kept for their reloads: the `external` text must not point into the
	// image text, which is freed after the load (the rules are not loaded again).
	if (provider_count_ > 0 && plan.has_external) {
		char* copy = plan.arena.copy(plan.external.data, plan.external.length);
		if (copy == nullptr) {
			report_.add(Report::Kind::no_memory, "external", "no memory to keep the external sources");
			return false;
		}
		plan.external.data = copy;
	}
	// The model: the image's datapoints, then the providers'.
	Composed composed;
	if (!compose(&plan, composed, report_)) {
		Log::warn(kTag, "image from %s refused: it does not compose with the providers' datapoints; the %s",
			source_name(source), loaded_ ? "loaded image stays" : "device keeps no image");
		for (size_t i = 0; i < report_.count; ++i) {
			Log::warn(kTag, "  %s: %s", report_.refusals[i].path[0] != '\0' ? report_.refusals[i].path : "(image)",
				report_.refusals[i].reason);
		}
		return false;
	}
	// Where each datapoint of the new model was in the old one (the rules follow it by that).
	uint16_t* previous = composed.arena.array<uint16_t>(composed.count);
	if (composed.count > 0 && previous == nullptr) {
		report_.add(Report::Kind::no_memory, "", "no memory for the swap table");
		return false;
	}
	for (size_t i = 0; i < composed.count; ++i) {
		const Datapoint* old = model_.find(composed.specs[i].name);
		previous[i] = old != nullptr ? old->id() : rules::kNone;
	}

	// 1, 2: the rules' queued events still name the old ids; then the model.
	engine_.flush();
	DatapointStatus status = model_.load(composed.specs, composed.count, true);
	if (status != DatapointStatus::ok) {
		report_.add(status == DatapointStatus::out_of_memory ? Report::Kind::no_memory : Report::Kind::apply,
			"datapoints", "the datapoint model refused the image: %s; the %s", datapoint_status_name(status),
			loaded_ ? "loaded image stays" : "device keeps no image");
		Log::error(kTag, "image not applied: the datapoint model refused it (%s)", datapoint_status_name(status));
		return false;
	}
	// From here on the previous image is gone from RAM.
	bool was_loaded = loaded_;
	loaded_ = false;
	bind_providers(&plan, composed);
	// The applied image's composition includes every provider change until now.
	if (reload_pending_) {
		controller_.loop().cancel(reload_timer_);
		reload_pending_ = false;
	}

	// 3: settings.
	apply_settings(plan);

	// 4: persistence, before anything reports (the scheduler's outputs).
	if (!bind_persistence(plan, source)) {
		return false;
	}

	// 5: rules.
	JsonSpanDocument document;
	JsonSpan rules_span = plan.rules;
	if (!plan.has_rules) {
		rules_span = JsonSpan{kNoRules, sizeof(kNoRules) - 1, JsonSpan::Kind::array};
	}
	PlanSchedules schedules(plan);
	rules::LoadResult result = engine_.load(document.value(rules_span), &schedules, previous);
	if (!result.ok()) {
		return fail_applied(source, result.path, "rule '%s': %s: %s", result.rule, rules::load_error_name(result.error),
			result.text);
	}

	// 6, 7, 7b: KNX and the time master, the schedules, the external values.
	if (!bind_technologies(plan, source)) {
		return false;
	}

	// 8: start.
	info_ = plan.info;
	source_ = source;
	loaded_ = true;
	has_knx_ = plan.mapping_count > 0 || plan.has_time_master;
	loaded_ms_ = controller_.clock().monotonic_ms();
	accept_dynamic();
	rules::StartReason reason = rules_started_ ? rules::StartReason::image : rules::StartReason::boot;
	rules_started_ = true;
	engine_.start(reason);
	if (was_loaded) {
		Log::info(kTag, "image swapped; rules started (%s)", rules::start_reason_name(reason));
	}
	// Kept for the providers' reloads; the rules' text is gone with the image text.
	drop_plan();
	if (provider_count_ > 0) {
		plan.has_rules = false;
		plan.rules = JsonSpan{};
		plan_ = &plan;
	}
	return true;
}

void ImageService::drop_plan() {
	delete plan_;
	plan_ = nullptr;
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
	const Plan* plan = loaded_ ? plan_ : nullptr;
	if (loaded_ && plan == nullptr) {
		refuse_dynamic("", "the loaded image's tables were not kept (no provider when it was applied): the providers' "
			"datapoints join with the next image load");
		Log::warn(kTag, "model not reloaded (%s): %s", why, dynamic_refusal()->reason);
		return false;
	}
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
	// The load dropped the rule engine's binding of the parameter datapoints (spec/rules.md 8.6).
	engine_.bind_parameters();
	accept_dynamic();
	++dynamic_reloads_;
	// The image's datapoints are at their positions: the rules' ids stay valid. Every component that
	// binds or indexes datapoints takes the new model, as in a swap (the bindings were dropped).
	if (plan != nullptr && (!bind_persistence(*plan, source_) || !bind_technologies(*plan, source_))) {
		return false; // torn down and restored (fail_applied)
	}
	Log::info(kTag, "model reloaded (%s): %u datapoint(s) of the image, %u of the providers", why,
		static_cast<unsigned>(composed.image_count), static_cast<unsigned>(dynamic_count_));
	if (mqtt_ != nullptr) {
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
	reload_timer_ = controller_.loop().schedule_at(at, Callback::member<ImageService, &ImageService::reload_due>(this));
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
	if (provider_count_ == 0) {
		drop_plan();
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
	Log::error(kTag, "image from %s failed while it was applied at %s: %s", source_name(source), where, reason);
	tear_down();
	if (!restoring_) {
		size_t bytes = 0;
		if (store_.stored(bytes)) {
			Log::warn(kTag, "loading the stored image again");
			restoring_ = true;
			load_stored(Source::restore);
			restoring_ = false;
		} else {
			Log::warn(kTag, "no stored image to return to: the device has no image");
		}
	}
	if (!restoring_ && !loaded_ && provider_count_ > 0) {
		reload_dynamic("no image after a failed apply");
	}
	report_.clear();
	report_.add(Report::Kind::apply, where, "%s", reason);
	if (changed_ && !restoring_) {
		changed_(); // the model was torn down (and maybe restored)
	}
	return false;
}

void ImageService::tear_down() {
	// Without deleting persisted state: the stored image is loaded again next.
	engine_.flush();
	model_.clear();
	for (size_t i = 0; i < provider_count_; ++i) {
		providers_[i]->model_cleared();
	}
	dynamic_count_ = 0;
	drop_plan();
	NoSchedules none;
	JsonSpanDocument document;
	engine_.load(document.value(JsonSpan{kNoRules, sizeof(kNoRules) - 1, JsonSpan::Kind::array}), &none, nullptr);
	adapter_.clear();
	time_master_.clear();
	scheduler_.clear();
	if (external_ != nullptr) {
		external_->clear();
	}
	if (mqtt_ != nullptr) {
		mqtt_->clear();
	}
	loaded_ = false;
	has_knx_ = false;
	info_ = ImageInfo();
}

bool ImageService::unload(const char** error) {
	engine_.flush();
	bool had = loaded_;
	loaded_ = false;
	drop_plan();
	// The providers' datapoints stay, with their values; everything else goes.
	if (provider_count_ == 0 || !reload_dynamic("the image is removed")) {
		model_.clear();
		for (size_t i = 0; i < provider_count_; ++i) {
			providers_[i]->model_cleared();
		}
		dynamic_count_ = 0;
	}
	NoSchedules none;
	JsonSpanDocument document;
	engine_.load(document.value(JsonSpan{kNoRules, sizeof(kNoRules) - 1, JsonSpan::Kind::array}), &none, nullptr);
	if (external_ != nullptr) {
		external_->clear();
	}
	if (mqtt_ != nullptr) {
		mqtt_->clear();
	}
	adapter_.clear();
	time_master_.clear();
	scheduler_.load(nullptr, 0);
	value_store_.load();
	history_.load(nullptr, 0, 0);
	has_knx_ = false;
	info_ = ImageInfo();
	report_.clear();
	stored_ok_ = false;
	store_error_[0] = '\0';
	view_.clear();
	if (changed_) {
		changed_();
	}
	if (!store_.remove(error)) {
		return false;
	}
	Log::info(kTag, "image %s: the device has no image (persisted values, rings and overrides removed)",
		had ? "unloaded" : "cleared");
	return true;
}

void ImageService::flush() {
	value_store_.flush();
	history_.flush();
}

bool ImageService::image_degraded() const {
	return loaded_ && has_knx_ && (tunnel_ == nullptr || !tunnel_->is_up());
}

void ImageService::schedule_fired(const char* id) {
	engine_.schedule_fired(id);
}

void ImageService::write_status(JsonWriter& json) const {
	json.member("loaded", loaded_);
	if (loaded_) {
		json.member("project", info_.project_id)
			.member("revision", info_.revision)
			.member("name", info_.name)
			.member("source", source_name(source_))
			.member("datapoints", static_cast<uint32_t>(info_.datapoints))
			.member("rules", static_cast<uint32_t>(info_.rules))
			.member("schedules", static_cast<uint32_t>(info_.schedules))
			.member("degraded", image_degraded());
	} else if (!report_.ok()) {
		json.member("refused", report_code(report_.kind));
	}
}

void ImageService::write_info(JsonWriter& json) const {
	json.member("ok", true);
	json.key("image").begin_object()
		.member("schemaVersion", info_.schema_version)
		.member("projectId", info_.project_id)
		.member("revision", info_.revision)
		.member("generatedAt", info_.generated_at)
		.member("generator", info_.generator)
		.member("name", info_.name)
		.member("bytes", static_cast<uint32_t>(info_.bytes))
		.end_object();
	json.key("counts").begin_object()
		.member("datapoints", static_cast<uint32_t>(info_.datapoints))
		.member("knx", static_cast<uint32_t>(info_.knx_datapoints))
		.member("rules", static_cast<uint32_t>(info_.rules))
		.member("schedules", static_cast<uint32_t>(info_.schedules))
		.member("locations", static_cast<uint32_t>(info_.locations))
		.member("functions", static_cast<uint32_t>(info_.functions))
		.member("historyRings", static_cast<uint32_t>(info_.history_rings))
		.member("timeMaster", info_.time_master)
		.end_object();
	json.member("source", source_name(source_));
	json.member("stored", stored_ok_);
	if (!stored_ok_ && store_error_[0] != '\0') {
		json.member("storeError", store_error_);
	}
}

void ImageService::write_report(JsonWriter& json) const {
	json.member("error", report_text(report_.kind)).member("code", report_code(report_.kind));
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
