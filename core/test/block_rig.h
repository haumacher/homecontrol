/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// A controller on the device's block runtime for the host tests of the built-in block types: the
// engine (no rules), the block runtime, a model with the test's datapoints and the blocks' mirrors,
// a settable clock and wall clock. Its image is compiled from the blocks JSON and the datapoints by
// the test compiler and loaded as the device loads it (validator, Loader, BlockSet::bind).
#ifndef HC_TEST_BLOCK_RIG_H
#define HC_TEST_BLOCK_RIG_H

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "conformance_block.h"
#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/blocks/block_runtime.h"
#include "hc/event_loop.h"
#include "hc/image/loader.h"
#include "hc/image/validator.h"
#include "hc/rules/engine.h"
#include "image_compiler.h"
#include "image_support.h"
#include "rules_test_support.h"
#include "test_support.h"

namespace hc_test::blocks {

using hc::blocks::BlockRuntime;

constexpr uint64_t kBase = 1000000;
constexpr uint64_t kMinute = 60000;
constexpr uint64_t kHour = 60 * kMinute;

/// The wall clock of the rules and blocks: settable, UTC.
class Wall : public hc::rules::TimeSource {
public:
	bool local_now(hc::DateTime& out) const override {
		if (!synchronized) {
			return false;
		}
		return utc_to_local(start_utc + static_cast<int64_t>((clock->now_ms - kBase) / 1000), out);
	}
	bool sun_time(hc::rules::SunEvent, hc::TimeOfDay&) const override { return false; }
	bool sun_position(int64_t utc_seconds, hc::SunPosition& out) const override {
		if (!has_location) {
			return false;
		}
		out = hc::sun_position(utc_seconds, location);
		return true;
	}
	/// The UTC instant of the wall clock now.
	int64_t utc() const { return start_utc + static_cast<int64_t>((clock->now_ms - kBase) / 1000); }
	const hc_test::FakeClock* clock = nullptr;
	bool synchronized = false;
	int64_t start_utc = 0;
	bool has_location = false;
	hc::GeoLocation location;
};

/// A controller on the device's runtime: the engine (no rules), the block runtime, a model with the
/// room's datapoints and the block's mirrors.
class Rig {
public:
	/// The room controller's datapoints (temp, temp2, valve, sw, win, mode, on).
	static std::vector<std::pair<std::string, hc::ValueType>> room_datapoints() {
		return {{"temp", hc::ValueType::real}, {"temp2", hc::ValueType::real}, {"valve", hc::ValueType::real},
			{"sw", hc::ValueType::boolean}, {"win", hc::ValueType::boolean}, {"mode", hc::ValueType::enumeration},
			{"on", hc::ValueType::boolean}};
	}

	/// The source of the rig's image (spec/runtime-image.md 3): `datapoints` (writable, transmitting,
	/// without technology: the rig's test technology owns them) and the `blocks` JSON array.
	static std::string source(const std::string& blocks, const std::vector<std::pair<std::string, hc::ValueType>>& datapoints) {
		std::string out = R"({"image": {"schemaVersion": "1.7", "projectId": "rig", "revision": 1}, "datapoints": [)";
		for (size_t i = 0; i < datapoints.size(); ++i) {
			out += std::string(i > 0 ? ", " : "") + R"({"id": ")" + datapoints[i].first + R"(", "type": ")" +
				hc::image::value_type_label(datapoints[i].second) + R"(", "writable": true, "transmit": true})";
		}
		return out + R"(], "blocks": )" + blocks + "}";
	}

	/// The image of `blocks` and `datapoints` compiled (with the corpus's block types) and put
	/// through the device's checks (validator, Loader::build); false with the refusals in `report`.
	static bool build(const std::string& blocks, const std::vector<std::pair<std::string, hc::ValueType>>& datapoints,
		const hc::Clock& clock, hc_test::CompiledImage& image, hc::image::Plan& plan, hc::image::Report& report) {
		hc::test::CompileOptions options;
		options.registry = &hc_test::corpus_registry();
		image.bytes = hc_test::compiled(source(blocks, datapoints), options);
		return check(clock, image, plan, report);
	}

	explicit Rig(const std::string& blocks, hc_test::FakeBlobStore* shared = nullptr,
		const std::vector<std::pair<std::string, hc::ValueType>>& datapoints = room_datapoints()) {
		clock_.now_ms = kBase;
		wall_.clock = &clock_;
		blobs_ = shared != nullptr ? shared : &own_blobs_;
		own_ = datapoints;
		plan_ = std::make_unique<hc::image::Plan>();
		hc::image::Report report;
		bool ok = build(blocks, datapoints, clock_, image_, *plan_, report);
		REQUIRE_MESSAGE(ok, "the rig's image is refused: " << hc_test::describe(report));
		load_model();
		boot();
	}

	/// An orderly reboot of the controller (the blobs and settings stay, the values go): the slot's
	/// image is loaded again.
	void reboot() {
		blocks_.reset();
		engine_.reset();
		plan_ = std::make_unique<hc::image::Plan>();
		hc::image::Report report;
		bool ok = check(clock_, image_, *plan_, report);
		REQUIRE_MESSAGE(ok, "the rig's image is refused at the reboot: " << hc_test::describe(report));
		load_model();
		boot();
	}

	/// Runs the loop up to `ms` after the start, every timer at its time.
	void run(uint64_t ms) {
		uint64_t target = kBase + ms;
		for (;;) {
			uint64_t next = loop_.next_deadline();
			if (next > target) {
				break;
			}
			if (next > clock_.now_ms) {
				clock_.now_ms = next;
			}
			loop_.run_once(clock_.now_ms);
		}
		clock_.now_ms = target;
	}

	uint64_t now() const { return clock_.now_ms - kBase; }

	void report(const char* name, const hc::Value& value) {
		const hc::Datapoint* dp = model_.find(name);
		REQUIRE(dp != nullptr);
		REQUIRE(model_.report(technology_, dp->id(), value) == hc::DatapointStatus::ok);
		run(now());
	}
	void report(const char* name, double value) { report(name, hc::Value::of_real(value)); }
	void stale(const char* name) {
		REQUIRE(model_.report_stale(technology_, model_.find(name)->id()) == hc::DatapointStatus::ok);
		run(now());
	}

	double number(const char* name) const {
		const hc::Datapoint* dp = model_.find(name);
		REQUIRE(dp != nullptr);
		const hc::Value& v = dp->value();
		switch (v.type()) {
			case hc::ValueType::boolean: return v.as_bool() ? 1 : 0;
			case hc::ValueType::enumeration: return v.as_enum();
			case hc::ValueType::signed_int: return v.as_signed();
			case hc::ValueType::unsigned_int: return v.as_unsigned();
			default: return v.as_real();
		}
	}
	/// A state field of the first block.
	double field(size_t f) const { return blocks_->state(0)[f]; }
	/// Sets a parameter of the first block through the API.
	void set(const char* parameter, double value) {
		const char* reason = nullptr;
		REQUIRE(blocks_->set_parameter(blocks_->set().at(0).id, parameter, value, &reason) == hc::blocks::ChangeStatus::ok);
		run(now());
	}

	BlockRuntime& blocks() { return *blocks_; }
	hc::DatapointModel& model() { return model_; }
	hc_test::TestTechnology& technology() { return technology_; }
	Wall& wall() { return wall_; }
	hc_test::FakeBlobStore& blobs() { return *blobs_; }

private:
	/// The device's checks of `image.bytes` and the Plan of its view.
	static bool check(const hc::Clock& clock, hc_test::CompiledImage& image, hc::image::Plan& plan, hc::image::Report& report) {
		const uint8_t* b = image.bytes.data();
		size_t n = image.bytes.size();
		if (!hc::image::check_header(b, n, n, report) || !hc::image::check_crc(b, report) ||
			!hc::image::check_structure(b, report)) {
			return false;
		}
		image.view.open(b);
		hc::image::Loader loader(clock, hc::HistoryLimits().cap_bytes);
		loader.set_block_registry(hc_test::corpus_registry());
		return loader.build(image.view, plan, report);
	}

	/// The model of the image's datapoints in image order; the rig's own (the image's first) bound to
	/// the test technology.
	void load_model() {
		REQUIRE(model_.load(plan_->datapoints, plan_->datapoint_count) == hc::DatapointStatus::ok);
		for (size_t i = 0; i < own_.size(); ++i) {
			const hc::Datapoint* dp = model_.find(own_[i].first.c_str());
			REQUIRE(dp != nullptr);
			REQUIRE(model_.bind(dp->id(), technology_) == hc::DatapointStatus::ok);
		}
	}

	/// The engine with the image's (empty) rules and the block runtime with its blocks, started.
	void boot() {
		engine_ = std::make_unique<hc::rules::Engine>(loop_, model_, clock_, settings_, wall_);
		REQUIRE(engine_->adopt(std::move(plan_->rule_set), nullptr).ok());
		blocks_ = std::make_unique<BlockRuntime>(loop_, model_, clock_, settings_, *blobs_, *engine_);
		REQUIRE(blocks_->adopt(std::move(plan_->block_set)));
		engine_->start(hc::rules::StartReason::boot);
		run(now()); // a reboot keeps the clock: the loop and its timers count from any origin
	}

	std::vector<std::pair<std::string, hc::ValueType>> own_; ///< The rig's own datapoints, before the mirrors.
	hc_test::FakeClock clock_;
	Wall wall_;
	hc_test::FakeSettings settings_;
	hc_test::FakeBlobStore own_blobs_;
	hc_test::FakeBlobStore* blobs_ = nullptr;
	hc::EventLoop loop_;
	hc::DatapointModel model_{clock_};
	hc_test::TestTechnology technology_;
	hc_test::CompiledImage image_;            ///< The slot's image: outlives the components that read it.
	std::unique_ptr<hc::image::Plan> plan_;
	std::unique_ptr<hc::rules::Engine> engine_;
	std::unique_ptr<BlockRuntime> blocks_; ///< Destroyed before the engine.
};

} // namespace hc_test::blocks

#endif // HC_TEST_BLOCK_RIG_H
