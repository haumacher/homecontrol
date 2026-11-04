/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// A controller on the device's block runtime for the host tests of the built-in block types: the
// engine (no rules), the block runtime, a model with the test's datapoints and the blocks' mirrors,
// a settable clock and wall clock.
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
#include "hc/json.h"
#include "hc/rules/engine.h"
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
	const hc_test::FakeClock* clock = nullptr;
	bool synchronized = false;
	int64_t start_utc = 0;
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

	explicit Rig(const std::string& blocks, hc_test::FakeBlobStore* shared = nullptr,
		const std::vector<std::pair<std::string, hc::ValueType>>& datapoints = room_datapoints())
		: blocks_text_(blocks) {
		clock_.now_ms = kBase;
		wall_.clock = &clock_;
		blobs_ = shared != nullptr ? shared : &own_blobs_;
		hc::JsonSpan span;
		REQUIRE(hc::json_parse(blocks_text_.data(), blocks_text_.size(), span, nullptr, 48));
		own_ = static_cast<uint16_t>(datapoints.size());
		for (const auto& dp : datapoints) {
			names_.push_back(dp.first);
			specs_types_.push_back(dp.second);
			flags_.push_back(hc::DatapointFlags::kWritable | hc::DatapointFlags::kTransmit);
		}
		std::vector<hc::blocks::ImpliedDatapoint> mirrors(hc::blocks::implied_datapoints(span, hc_test::corpus_registry(), nullptr, 0));
		hc::blocks::implied_datapoints(span, hc_test::corpus_registry(), mirrors.data(), mirrors.size());
		for (const auto& m : mirrors) {
			names_.push_back(m.name);
			specs_types_.push_back(m.type);
			flags_.push_back(m.flags);
		}
		for (size_t i = 0; i < names_.size(); ++i) {
			specs_.push_back(hc::DatapointSpec{names_[i].c_str(), specs_types_[i], flags_[i]});
		}
		REQUIRE(model_.load(specs_.data(), specs_.size()) == hc::DatapointStatus::ok);
		for (uint16_t i = 0; i < own_; ++i) {
			REQUIRE(model_.bind(i, technology_) == hc::DatapointStatus::ok);
		}
		boot(span);
	}

	void boot(const hc::JsonSpan& span) {
		engine_ = std::make_unique<hc::rules::Engine>(loop_, model_, clock_, settings_, wall_);
		hc_test::JsonTree tree;
		hc_test::Json none;
		none.kind = hc_test::Json::Kind::array;
		REQUIRE(engine_->load(tree.value(none), nullptr).ok());
		blocks_ = std::make_unique<BlockRuntime>(loop_, model_, clock_, settings_, *blobs_, *engine_);
		hc::blocks::BlockSet set;
		struct NoOwners : hc::blocks::BlockDirectory {
			const char* owner(const char*) const override { return nullptr; }
		} owners;
		hc::blocks::BlockLoadResult result = set.load(span, model_, owners, hc_test::corpus_registry());
		REQUIRE_MESSAGE(result.ok(), result.path << ": " << result.text);
		REQUIRE(blocks_->adopt(std::move(set)));
		engine_->start(hc::rules::StartReason::boot);
		run(now()); // a reboot keeps the clock: the loop and its timers count from any origin

	}

	/// An orderly reboot of the controller (the blobs and settings stay, the values go).
	void reboot() {
		blocks_.reset();
		engine_.reset();
		REQUIRE(model_.load(specs_.data(), specs_.size()) == hc::DatapointStatus::ok);
		for (uint16_t i = 0; i < own_; ++i) {
			REQUIRE(model_.bind(i, technology_) == hc::DatapointStatus::ok);
		}
		hc::JsonSpan span;
		REQUIRE(hc::json_parse(blocks_text_.data(), blocks_text_.size(), span, nullptr, 48));
		boot(span);
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
	std::string blocks_text_;
	uint16_t own_ = 0; ///< The rig's own datapoints, before the mirrors.
	hc_test::FakeClock clock_;
	Wall wall_;
	hc_test::FakeSettings settings_;
	hc_test::FakeBlobStore own_blobs_;
	hc_test::FakeBlobStore* blobs_ = nullptr;
	hc::EventLoop loop_;
	hc::DatapointModel model_{clock_};
	hc_test::TestTechnology technology_;
	std::vector<std::string> names_;
	std::vector<hc::ValueType> specs_types_;
	std::vector<uint8_t> flags_;
	std::vector<hc::DatapointSpec> specs_;
	std::unique_ptr<hc::rules::Engine> engine_;
	std::unique_ptr<BlockRuntime> blocks_; ///< Destroyed before the engine.
};

} // namespace hc_test::blocks

#endif // HC_TEST_BLOCK_RIG_H
