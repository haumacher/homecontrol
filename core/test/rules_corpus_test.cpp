/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Runs the rule conformance corpus spec/fixtures/rules/*.json (format: its README.md), which
// E-Home's reference interpreter passes too.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "conformance_block.h"
#include "doctest.h"
#include "fake_blob_store.h"
#include "hc/blocks/block_runtime.h"
#include "hc/blocks/block_set.h"
#include "hc/datapoint.h"
#include "hc/event_loop.h"
#include "hc/image/loader.h"
#include "hc/image/rules_clock.h"
#include "hc/json.h"
#include "hc/network_settings.h"
#include "hc/rules/engine.h"
#include "hc/time_service.h"
#include "hc/rules/parameter_store.h"
#include "hc/scheduler.h"
#include "hc/value_store.h"
#include "rules_test_support.h"
#include "test_support.h"

using hc::DatapointModel;
using hc::DatapointSpec;
using hc::EventLoop;
using hc::JsonValue;
using hc::rules::Engine;
using hc::rules::LoadResult;
using hc::rules::LogCode;
using hc::rules::StartReason;
using hc::rules::Type;
using hc::rules::Val;
using hc_test::Json;
using hc_test::JsonTree;

namespace hc_test {

std::vector<std::string> corpus_files(const char* directory) {
	std::vector<std::string> files;
	std::error_code error;
	for (const auto& entry : std::filesystem::directory_iterator(std::string(HC_SPEC_DIR "/fixtures/") + directory, error)) {
		std::string name = entry.path().filename().string();
		if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0) {
			files.push_back(entry.path().string());
		}
	}
	std::sort(files.begin(), files.end());
	return files;
}

std::vector<std::string> rule_corpus_files() { return corpus_files("rules"); }

} // namespace hc_test

namespace {

constexpr uint64_t kBase = 1000000; ///< Monotonic time of the corpus's time 0.

/// The `blocks` of an image without blocks.
constexpr char kEmptyArray[] = "[]";
const hc::JsonSpan kNoBlocks{kEmptyArray, 2, hc::JsonSpan::Kind::array};

/// The case's wall clock as the core's time service interface: UTC from the case's start and the
/// monotonic clock, local time through the case's time zone (README "clock": `zone`, UTC without).
class CaseClock : public hc::TimeSource {
public:
	uint64_t monotonic_ms() const override { return clock->now_ms; }

	hc::WallTime now_utc() const override {
		return hc::WallTime{start_utc + static_cast<int64_t>((clock->now_ms - kBase) / 1000), synchronized};
	}

	bool to_local(int64_t utc_seconds, hc::LocalTime& out) const override { return zone.to_local(utc_seconds, out); }

	bool sun_times(const hc::Date&, hc::SunTimes&) const override { return false; }
	bool sun_window(const hc::Date&, hc::SunWindow&) const override { return false; }
	/// The case's `location` (the blocks' sun(), spec/fixtures/blocks/README.md).
	bool location(hc::GeoLocation& out) const override {
		out = where;
		return has_location;
	}

	const hc_test::FakeClock* clock = nullptr;
	bool has_location = false;
	hc::GeoLocation where;
	bool synchronized = false;
	int64_t start_utc = 0;
	hc_test::PosixRuleZone zone;
};

/// The wall clock and the sun of a case: the device's adapter (image::RulesClock) over the case's
/// clock and zone, the sun times from the case.
class CaseTime : public hc::rules::TimeSource {
public:
	bool local_now(hc::DateTime& out) const override { return rules_clock.local_now(out); }
	int64_t local_to_utc(const hc::DateTime& local) const override { return rules_clock.local_to_utc(local); }
	bool utc_to_local(int64_t utc, hc::DateTime& out) const override { return rules_clock.utc_to_local(utc, out); }
	bool sun_position(int64_t utc, hc::SunPosition& out) const override { return rules_clock.sun_position(utc, out); }

	bool sun_time(hc::rules::SunEvent event, hc::TimeOfDay& out) const override {
		const Val& v = sun[static_cast<size_t>(event)];
		if (!v.has()) {
			return false;
		}
		out = v.u.tod;
		return true;
	}

	CaseClock wall;
	hc::image::RulesClock rules_clock{wall};
	Val sun[4];
};

struct LogEntry {
	uint64_t at;
	std::string rule; ///< The rule's id, or "block <id>" for an entry of the block runtime.
	std::string code;
	std::string level;
	std::string message;
	std::string culprit; ///< loopGuard: the rule that fired most, and its derived firings.
	uint32_t firings = 0;
};

struct CommandEntry {
	uint64_t at;
	std::string rule;
	std::string datapoint;
	Val value;
};

struct ReadEntry {
	uint64_t at;
	std::string rule;
	std::string datapoint;
};

class Recorder : public hc::rules::EngineObserver, public hc::blocks::BlockObserver {
public:
	explicit Recorder(const hc_test::FakeClock& clock) : clock_(clock) {}

	void rule_logged(const char* rule, LogCode code, hc::LogLevel level, const char* text) override {
		const char* level_name = level == hc::LogLevel::error ? "error" : (level == hc::LogLevel::warn ? "warning" : "info");
		logs.push_back(LogEntry{clock_.now_ms - kBase, rule, hc::rules::log_code_name(code), level_name, text, std::string(), 0});
	}

	void loop_guard(const char* rule, const char* culprit, uint32_t firings) override {
		REQUIRE(!logs.empty());
		CHECK(logs.back().code == "loopGuard");
		CHECK((logs.back().rule == rule || logs.back().rule == std::string("block ") + rule)); // a rule or a block
		logs.back().culprit = culprit;
		logs.back().firings = firings;
	}

	void rule_commanded(const char* rule, const hc::Datapoint& datapoint, const hc::Value& value) override {
		commands.push_back(CommandEntry{clock_.now_ms - kBase, rule, datapoint.name(), hc::rules::val_from_datapoint(value)});
	}

	void rule_read(const char* rule, const hc::Datapoint& datapoint) override {
		reads.push_back(ReadEntry{clock_.now_ms - kBase, rule, datapoint.name()});
	}

	// The block runtime (spec/fixtures/blocks/README.md): entries named "block <id>".
	void block_logged(const char* block, hc::blocks::BlockLogCode code, hc::LogLevel level, const char* text) override {
		const char* level_name = level == hc::LogLevel::error ? "error" : (level == hc::LogLevel::warn ? "warning" : "info");
		logs.push_back(LogEntry{clock_.now_ms - kBase, std::string("block ") + block, hc::blocks::block_log_code_name(code),
			level_name, text, std::string(), 0});
	}

	void block_commanded(const char* block, const hc::Datapoint& datapoint, const hc::Value& value) override {
		commands.push_back(CommandEntry{clock_.now_ms - kBase, std::string("block ") + block, datapoint.name(),
			hc::rules::val_from_datapoint(value)});
	}


	std::vector<LogEntry> logs;
	std::vector<CommandEntry> commands;
	std::vector<ReadEntry> reads;

private:
	const hc_test::FakeClock& clock_;
};

std::string text_of(const Val& v) {
	char buffer[256];
	hc::rules::literal_text(v, buffer, sizeof(buffer));
	if (!v.has()) {
		return "null";
	}
	std::string text = std::string(hc::rules::type_name(v.type)) + " " + buffer;
	if (v.type == Type::date_time && (v.u.dt.flags & hc::DateTime::kSummerTime) != 0) {
		text += " (summer time)";
	}
	return text;
}

/// Exact comparison, reals with a relative tolerance of 1e-9.
bool same(const Val& actual, const Val& expected) {
	if (!actual.has() || !expected.has()) {
		return actual.has() == expected.has();
	}
	if (actual.type == Type::real && expected.type == Type::real) {
		double a = actual.u.r, e = expected.u.r;
		return a == e || std::fabs(a - e) <= 1e-9 * std::max(std::fabs(a), std::fabs(e));
	}
	if (actual.type == Type::real && expected.type == Type::integer) {
		return actual.u.r == static_cast<double>(expected.u.i);
	}
	return hc::rules::val_identical(actual, expected);
}

/// The schedule entries of a case in the image's full form (README "schedules"), parsed by the
/// device's image loader from a synthetic image that holds only them: the specs the scheduler loads
/// and the time datapoints they imply (spec/schedules.md 2.2).
struct SchedulePlan {
	std::string text;
	hc::image::Plan plan;
	std::vector<std::string> time_datapoints;

	/// From the `schedules` array of the case or of an image step; false when it holds no full entry.
	bool build(const hc::JsonSpan& list, const hc_test::FakeClock& clock) {
		std::string entries;
		size_t position = 0;
		hc::JsonSpan item;
		hc::JsonSpan kind;
		while (hc::json_next_item(list, position, item)) {
			if (item.kind == hc::JsonSpan::Kind::object && hc::json_member(item, "kind", kind)) {
				entries += (entries.empty() ? "" : ",") + std::string(item.data, item.length);
			}
		}
		if (entries.empty()) {
			return false;
		}
		text = R"({"image": {"schemaVersion": "1.2", "projectId": "corpus", "revision": 1}, "schedules": [)" + entries + "]}";
		hc::image::Loader loader(clock, hc::HistoryLimits().cap_bytes);
		hc::image::Report report;
		bool ok = loader.parse(text.data(), text.size(), plan, report);
		std::string reasons;
		for (size_t i = 0; i < report.count; ++i) {
			reasons += std::string(report.refusals[i].path) + ": " + report.refusals[i].reason + "\n";
		}
		REQUIRE_MESSAGE(ok, "the case's schedules are refused:\n" << reasons);
		for (size_t i = 0; i < plan.datapoint_count; ++i) {
			if (plan.bindings[i] == hc::image::Binding::schedule) {
				time_datapoints.push_back(plan.datapoints[i].name);
			}
		}
		return true;
	}
};

class CaseRunner {
public:
	CaseRunner(const Json& json, const std::string& text) : case_(json), text_(text) {
		// Index: 1 refuses commands, 2 refuses reads.
		technologies_[1].refuse = technologies_[3].refuse = true;
		technologies_[2].refuse_reads = technologies_[3].refuse_reads = true;
	}

	void run() {
		time_.wall.clock = &clock_;
		clock_.now_ms = kBase;
		REQUIRE(time_.wall.zone.parse("UTC0"));
		if (case_.has("clock")) {
			const Json& c = case_["clock"];
			if (c.has("zone")) {
				REQUIRE_MESSAGE(time_.wall.zone.parse(c["zone"].text.c_str()), "zone " << c["zone"].text);
			}
			int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
			REQUIRE(std::sscanf(c["start"].text.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6);
			hc::Date date;
			date.year = static_cast<uint16_t>(y);
			date.month = static_cast<uint8_t>(mo);
			date.day = static_cast<uint8_t>(d);
			// The local start time; a time that occurs twice is its first occurrence.
			time_.wall.start_utc = time_.wall.local_to_utc(date, static_cast<unsigned>(h), static_cast<unsigned>(mi),
				static_cast<unsigned>(s));
			time_.wall.synchronized = !c.has("synchronized") || c["synchronized"].boolean;
		}
		if (case_.has("location")) {
			time_.wall.where.latitude = case_["location"]["lat"].number;
			time_.wall.where.longitude = case_["location"]["lon"].number;
			time_.wall.has_location = true;
		}
		if (case_.has("ui") && case_["ui"].has("lang")) {
			REQUIRE(settings_.set_string(hc::setting::kUiLang, case_["ui"]["lang"].text.c_str()) == hc::SettingsStatus::ok);
		}
		static const char* const kSun[] = {"sunrise", "sunset", "dawn", "dusk"};
		for (size_t i = 0; i < 4; ++i) {
			if (case_["astro"].has(kSun[i])) {
				REQUIRE(hc::rules::val_from_json(Type::time_of_day, tree_.value(case_["astro"][kSun[i]]), time_.sun[i]) ==
					hc::rules::LiteralStatus::ok);
			}
		}
		for (const Json& s : case_["schedules"].items) {
			if (s.kind == Json::Kind::object) {
				schedules_.ids.push_back(s["id"].text);
				if (s.has("output")) {
					schedules_.outputs.push_back(s["output"].text);
				}
			} else {
				schedules_.ids.push_back(s.text);
			}
		}
		hc::JsonSpan root;
		hc::JsonSpan list;
		REQUIRE(hc::json_parse(text_.data(), text_.size(), root, nullptr, 100));
		if (hc::json_member(root, "schedules", list)) {
			auto plan = std::make_unique<SchedulePlan>();
			if (plan->build(list, clock_)) {
				schedule_plan_ = std::move(plan);
			}
		}
		for (const Json& dp : case_["datapoints"].items) {
			if (dp.has("readable") && !dp["readable"].boolean) {
				schedules_.unreadable.push_back(dp["name"].text);
			}
		}
		empty_rules_.kind = Json::Kind::array;
		if (!hc::json_member(root, "blocks", blocks_span_)) {
			blocks_span_ = kNoBlocks;
		}
		load_model(rules_of(case_), blocks_span_);
		for (const auto& entry : case_["persisted"].members) {
			if (entry.first.compare(0, 6, "block/") == 0) {
				persist_record(entry.first.substr(6), entry.second);
				continue;
			}
			size_t slash = entry.first.find('/');
			REQUIRE(slash != std::string::npos);
			Val v;
			if (entry.second["type"].text == "state") {
				// A persisted rule state (spec 8.5): `<ruleId>/.state` with the state id.
				REQUIRE(entry.first.substr(slash + 1) == hc::rules::ParameterStore::kStateKey);
				const std::string& id = entry.second["value"].text;
				v = Val::of_string(id.c_str(), id.size());
				REQUIRE(v.has());
			} else {
				Type type = hc::rules::type_from_name(entry.second["type"].text.c_str());
				REQUIRE(hc::rules::val_from_json(type, tree_.value(entry.second["value"]), v) == hc::rules::LiteralStatus::ok);
			}
			hc::rules::ParameterStore store(settings_);
			REQUIRE(store.write(entry.first.substr(0, slash).c_str(), entry.first.substr(slash + 1).c_str(), v));
		}
		for (const auto& entry : case_["initial"].members) {
			report(entry.first, entry.second);
		}
		image_ = &rules_of(case_);
		engine_ = std::make_unique<Engine>(loop_, model_, clock_, settings_, time_);
		engine_->set_observer(&recorder_);
		LoadResult result = engine_->load(tree_.value(*image_), &schedules_);
		if (case_["expected"].has("loadRefused") && (!result.ok() || !case_.has("blocks"))) {
			CHECK_MESSAGE(!result.ok(), "the load must be refused");
			CHECK(std::string(hc::rules::load_error_name(result.error)) == case_["expected"]["loadRefused"].text);
			MESSAGE("refusal: " << std::string(result.rule) << " at " << std::string(result.path) << ": " << std::string(result.text));
			return;
		}
		REQUIRE_MESSAGE(result.ok(), "load refused: " << std::string(hc::rules::load_error_name(result.error)) << " rule "
			<< std::string(result.rule) << " at " << std::string(result.path) << ": " << std::string(result.text));
		// The blocks after the rules (spec/fixtures/blocks/README.md).
		hc::blocks::BlockSet set;
		hc::blocks::BlockLoadResult block_result = set.load(blocks_span_, model_, owners_, hc_test::corpus_registry());
		if (case_["expected"].has("loadRefused")) {
			CHECK_MESSAGE(!block_result.ok(), "the load must be refused");
			CHECK(std::string(hc::blocks::block_error_name(block_result.error)) == case_["expected"]["loadRefused"].text);
			if (case_["expected"].has("path")) {
				CHECK(std::string(block_result.path) == case_["expected"]["path"].text);
			}
			MESSAGE("refusal: " << std::string(block_result.block) << " at " << std::string(block_result.path) << ": "
				<< std::string(block_result.text));
			return;
		}
		REQUIRE_MESSAGE(block_result.ok(), "blocks refused at " << std::string(block_result.path) << ": "
			<< std::string(hc::blocks::block_error_name(block_result.error)) << ": " << std::string(block_result.text));
		make_blocks(std::move(set));
		load_schedules(true);
		engine_->start(StartReason::boot);
		advance(0);
		size_t step_index = 0;
		for (const Json& step : case_["timeline"].items) {
			current_step_ = step_index++;
			uint64_t at = static_cast<uint64_t>(step["at"].number);
			advance(at);
			apply(step, at);
			advance(at);
		}
		advance(static_cast<uint64_t>(case_["end"].number));
		compare_outputs();
		if (case_["expected"].has("final")) {
			compare_state(case_["expected"]["final"], "final");
		}
	}

private:
	struct Schedules : hc::rules::ScheduleDirectory {
		bool has_schedule(const char* id) const override {
			return std::find(ids.begin(), ids.end(), std::string(id)) != ids.end();
		}
		bool owns_output(const char* datapoint) const override {
			return std::find(outputs.begin(), outputs.end(), std::string(datapoint)) != outputs.end();
		}
		bool technology_reads(const char* datapoint) const override {
			return std::find(unreadable.begin(), unreadable.end(), std::string(datapoint)) == unreadable.end();
		}
		std::vector<std::string> ids;
		std::vector<std::string> outputs;
		std::vector<std::string> unreadable; ///< "readable": false
	};

	/// Forwards the scheduler's firings to the current engine.
	struct Firings : hc::ScheduleListener {
		explicit Firings(CaseRunner& r) : runner(r) {}
		void schedule_fired(const char* id) override { runner.engine_->schedule_fired(id); }
		CaseRunner& runner;
	};

	/// The scheduler with the case's full entries (README "schedules"), after the rules' load as in
	/// the image service (spec/runtime-image.md 6); the time datapoints take their values quietly.
	/// `fresh`: a new scheduler (the start, a reboot).
	void load_schedules(bool fresh) {
		if (schedule_plan_ == nullptr) {
			return;
		}
		if (fresh || scheduler_ == nullptr) {
			scheduler_.reset();
			scheduler_ = std::make_unique<hc::Scheduler>(loop_, time_.wall, settings_, &model_);
			scheduler_->defer_time_reports(true);
			scheduler_->add_listener(firings_);
		}
		const hc::image::Plan& plan = schedule_plan_->plan;
		REQUIRE(scheduler_->load(plan.schedules, plan.schedule_count) == hc::ScheduleStatus::ok);
		engine_->quietly([this] { scheduler_->publish_times(); });
	}

	/// The case's datapoints, owned by the test technologies, then the parameter datapoints of
	/// `rules` (spec/rules.md 8.6) as the image loader derives them, unbound: the engine binds them;
	/// then the time datapoints of the full schedule entries, which the scheduler binds.
	void load_model(const Json& rules, const hc::JsonSpan& blocks, bool keep_values = false) {
		names_.clear();
		specs_.clear();
		std::vector<hc::ValueType> types;
		std::vector<uint8_t> flags;
		for (const Json& dp : case_["datapoints"].items) {
			names_.push_back(dp["name"].text);
			hc::ValueType type = hc::rules::datapoint_type_from_name(dp["type"].text.c_str());
			REQUIRE_MESSAGE(type != hc::ValueType::none, "datapoint type " << dp["type"].text);
			bool writable = !dp.has("writable") || dp["writable"].boolean;
			types.push_back(type);
			flags.push_back(static_cast<uint8_t>(hc::DatapointFlags::kReadable |
				(writable ? hc::DatapointFlags::kWritable | hc::DatapointFlags::kTransmit : 0)));
		}
		size_t own = names_.size();
		std::vector<hc::rules::ParameterDatapoint> derived(256);
		size_t count = hc::rules::parameter_datapoints(tree_.value(rules), derived.data(), derived.size());
		REQUIRE(count <= derived.size());
		for (size_t i = 0; i < count; ++i) {
			names_.push_back(derived[i].name);
			types.push_back(derived[i].type);
			flags.push_back(derived[i].flags);
		}
		if (schedule_plan_ != nullptr) {
			for (const std::string& name : schedule_plan_->time_datapoints) {
				names_.push_back(name);
				types.push_back(hc::ValueType::time_of_day);
				flags.push_back(static_cast<uint8_t>(hc::DatapointFlags::kWritable | hc::DatapointFlags::kTransmit));
			}
		}
		// The blocks' mirrors (spec/blocks.md 6), as the image loader derives them.
		std::vector<hc::blocks::ImpliedDatapoint> mirrors(
			hc::blocks::implied_datapoints(blocks, hc_test::corpus_registry(), nullptr, 0));
		hc::blocks::implied_datapoints(blocks, hc_test::corpus_registry(), mirrors.data(), mirrors.size());
		for (const hc::blocks::ImpliedDatapoint& mirror : mirrors) {
			names_.push_back(mirror.name);
			types.push_back(mirror.type);
			flags.push_back(mirror.flags);
		}
		for (size_t i = 0; i < names_.size(); ++i) {
			specs_.push_back(DatapointSpec{names_[i].c_str(), types[i], flags[i]});
		}
		REQUIRE(model_.load(specs_.data(), specs_.size(), keep_values) == hc::DatapointStatus::ok);
		for (size_t i = 0; i < own; ++i) {
			const Json& dp = case_["datapoints"].items[i];
			bool refuse = dp.has("refuseCommands") && dp["refuseCommands"].boolean;
			bool refuse_reads = dp.has("refuseReads") && dp["refuseReads"].boolean;
			REQUIRE(model_.bind(static_cast<uint16_t>(i), technologies_[(refuse ? 1 : 0) + (refuse_reads ? 2 : 0)]) ==
				hc::DatapointStatus::ok);
		}
	}

	const hc::Datapoint& datapoint(const std::string& name) {
		const hc::Datapoint* dp = model_.find(name.c_str());
		REQUIRE_MESSAGE(dp != nullptr, "no datapoint " << name);
		return *dp;
	}

	Val expected_value(Type type, const Json& json) {
		if (json.kind == Json::Kind::null) {
			return Val::none(type);
		}
		Val v;
		const char* reason = "";
		if (type == Type::date_time && (json.has("dayOfWeek") || json.has("summerTime"))) {
			// The corpus notation adds the day of the week and the summer-time flag to a date-time
			// (README "Values"); the image's literal (spec 3.1) has neither: the day comes from the
			// date, the flag from the zone.
			Json copy = json;
			copy.members.erase("dayOfWeek");
			copy.members.erase("summerTime");
			REQUIRE(hc::rules::val_from_json(type, tree_.value(copy), v, &reason) == hc::rules::LiteralStatus::ok);
			if (json.has("dayOfWeek")) {
				CHECK_MESSAGE(v.u.dt.day_of_week == static_cast<uint8_t>(json["dayOfWeek"].number),
					"day of week of an expected date-time");
			}
			if (json.has("summerTime") && json["summerTime"].boolean) {
				v.u.dt.flags = hc::DateTime::kSummerTime;
			}
			return v;
		}
		REQUIRE_MESSAGE(hc::rules::val_from_json(type, tree_.value(json), v, &reason) == hc::rules::LiteralStatus::ok,
			"expected value of type " << std::string(hc::rules::type_name(type)) << ": " << std::string(reason));
		return v;
	}

	hc::Value datapoint_value(const hc::Datapoint& dp, const Json& json) {
		Val v = expected_value(hc::rules::type_of_datapoint(dp.type()), json);
		std::optional<hc::Value> value = hc::rules::convert_for_datapoint(v, dp.type());
		REQUIRE_MESSAGE(value.has_value(), "value for " << dp.name());
		return *value;
	}

	void report(const std::string& name, const Json& json) {
		const hc::Datapoint& dp = datapoint(name);
		hc::Technology& owner = *dp.technology();
		REQUIRE(model_.report(owner, dp.id(), datapoint_value(dp, json)) == hc::DatapointStatus::ok);
	}

	/// Runs the loop up to time `at`: every timer (and posted event) at its own time.
	void advance(uint64_t at) {
		uint64_t target = kBase + at;
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

	void apply(const Json& step, uint64_t at) {
		if (step.has("report")) {
			report(step["report"]["datapoint"].text, step["report"]["value"]);
		} else if (step.has("stale")) {
			const hc::Datapoint& dp = datapoint(step["stale"].text);
			REQUIRE(model_.report_stale(*dp.technology(), dp.id()) == hc::DatapointStatus::ok);
		} else if (step.has("command")) {
			const hc::Datapoint& dp = datapoint(step["command"]["datapoint"].text);
			hc::DatapointStatus status = model_.command(dp.id(), datapoint_value(dp, step["command"]["value"]));
			if (step["command"].has("refused") && step["command"]["refused"].boolean) {
				CHECK_MESSAGE(status != hc::DatapointStatus::ok, "the command at " << at << " must be refused");
			} else {
				REQUIRE_MESSAGE(status == hc::DatapointStatus::ok, "the command at " << at << " was refused: "
					<< hc::datapoint_status_name(status));
			}
		} else if (step.has("schedule")) {
			engine_->schedule_fired(step["schedule"].text.c_str());
		} else if (step.has("setParameter")) {
			const Json& p = step["setParameter"];
			engine_->set_parameter(p["rule"].text.c_str(), p["parameter"].text.c_str(), tree_.value(p["value"]));
		} else if (step.has("clockSync")) {
			time_.wall.synchronized = step["clockSync"].boolean;
			if (scheduler_ != nullptr) {
				scheduler_->time_changed(time_.wall.synchronized ? hc::TimeEvents::kSynchronized
					: hc::TimeEvents::kSynchronizationLost);
			}
		} else if (step.has("setSchedule")) {
			set_schedule(step["setSchedule"], at);
		} else if (step.has("image")) {
			// As the image service swaps (doc/image.md): the rules' queued events go, the model is
			// loaded again with the values kept (the parameter datapoints follow the new rules), the
			// rules follow the datapoints by their previous ids.
			engine_->flush();
			std::vector<std::string> old_names;
			for (size_t i = 0; i < model_.size(); ++i) {
				old_names.push_back(model_.get(static_cast<uint16_t>(i))->name());
			}
			std::unique_ptr<SchedulePlan> previous_plan;
			if (step["image"].has("schedules")) {
				// The step's entries replace the case's (README "image").
				hc::JsonSpan list = timeline_member(current_step_, "image", "schedules");
				auto plan = std::make_unique<SchedulePlan>();
				REQUIRE_MESSAGE(plan->build(list, clock_), "the image step's schedules need full entries");
				previous_plan = std::move(schedule_plan_);
				schedule_plan_ = std::move(plan);
				for (const Json& s : step["image"]["schedules"].items) {
					if (std::find(schedules_.ids.begin(), schedules_.ids.end(), s["id"].text) == schedules_.ids.end()) {
						schedules_.ids.push_back(s["id"].text);
					}
				}
			}
			const Json& next_rules = rules_of(step["image"]);
			hc::JsonSpan next_blocks = step["image"].has("blocks") ? timeline_member(current_step_, "image", "blocks") : kNoBlocks;
			load_model(next_rules, next_blocks, true);
			std::vector<uint16_t> previous;
			for (size_t i = 0; i < model_.size(); ++i) {
				auto it = std::find(old_names.begin(), old_names.end(), std::string(model_.get(static_cast<uint16_t>(i))->name()));
				previous.push_back(it == old_names.end() ? hc::rules::kNone : static_cast<uint16_t>(it - old_names.begin()));
			}
			LoadResult result = engine_->load(tree_.value(next_rules), &schedules_, previous.data());
			REQUIRE_MESSAGE(result.ok(), "image at " << at << " refused: " << std::string(hc::rules::load_error_name(result.error))
				<< " rule " << std::string(result.rule) << " at " << std::string(result.path) << ": " << std::string(result.text));
			image_ = &next_rules;
			blocks_span_ = next_blocks;
			hc::blocks::BlockSet set;
			hc::blocks::BlockLoadResult block_result = set.load(blocks_span_, model_, owners_, hc_test::corpus_registry());
			REQUIRE_MESSAGE(block_result.ok(), "the blocks of the image at " << at << " are refused at "
				<< std::string(block_result.path) << ": " << std::string(block_result.text));
			REQUIRE(blocks_->adopt(std::move(set)));
			load_schedules(false);
			engine_->start(StartReason::image);
		} else if (step.has("reboot")) {
			scheduler_.reset();
			blocks_.reset(); // an orderly reboot: its due records are written
			engine_.reset();
			load_model(*image_, blocks_span_);
			engine_ = std::make_unique<Engine>(loop_, model_, clock_, settings_, time_);
			engine_->set_observer(&recorder_);
			LoadResult result = engine_->load(tree_.value(*image_), &schedules_);
			REQUIRE(result.ok());
			hc::blocks::BlockSet set;
			REQUIRE(set.load(blocks_span_, model_, owners_, hc_test::corpus_registry()).ok());
			make_blocks(std::move(set));
			load_schedules(true);
			engine_->start(StartReason::boot);
		} else if (step.has("setBlock")) {
			set_block(step["setBlock"], at);
		} else if (step.has("check")) {
			compare_state(step["check"], "check at " + std::to_string(at));
		} else {
			FAIL("unknown timeline step at " << at);
		}
	}

	/// The span of `timeline[index].<step>.<member>` in the case's text.
	hc::JsonSpan timeline_member(size_t index, const char* step, const char* member) {
		hc::JsonSpan root, timeline, item, inner, out;
		REQUIRE(hc::json_parse(text_.data(), text_.size(), root, nullptr, 100));
		REQUIRE(hc::json_member(root, "timeline", timeline));
		size_t position = 0;
		for (size_t i = 0; i <= index; ++i) {
			REQUIRE(hc::json_next_item(timeline, position, item));
		}
		REQUIRE(hc::json_member(item, step, inner));
		REQUIRE(hc::json_member(inner, member, out));
		return out;
	}

	/// A member's value in the JSON form of the image and the API (spec/schedules.md 3) into `values`.
	static void read_member(const Json& json, uint16_t member, hc::ScheduleSpec& values) {
		if (hc::ScheduleMember::is_time(member) || member == hc::ScheduleMember::kAt) {
			int64_t seconds = static_cast<int64_t>(json["hour"].number) * 3600 + static_cast<int64_t>(json["minute"].number) * 60 +
				static_cast<int64_t>(json["second"].number);
			if (member == hc::ScheduleMember::kAt) {
				seconds += hc::days_from_civil(static_cast<int32_t>(json["year"].number), static_cast<unsigned>(json["month"].number),
					static_cast<unsigned>(json["day"].number)) * 86400;
			}
			hc::set_schedule_member_value(values, member, seconds);
		} else if (member == hc::ScheduleMember::kDays) {
			uint8_t days = 0;
			for (const Json& day : json.items) {
				days = static_cast<uint8_t>(days | hc::Weekdays::bit(static_cast<unsigned>(day.number)));
			}
			values.days = days;
		} else {
			hc::set_schedule_member_value(values, member, static_cast<int64_t>(json.number) / 1000);
		}
	}

	/// README "setSchedule": the change of `PUT /api/schedules/<id>`.
	void set_schedule(const Json& change, uint64_t at) {
		REQUIRE_MESSAGE(scheduler_ != nullptr, "setSchedule needs full schedule entries");
		const std::string& id = change["id"].text;
		std::string refused = change.has("refused") ? change["refused"].text : "";
		if (change.has("reset")) {
			CHECK_MESSAGE(scheduler_->reset_overrides(id.c_str()), "the reset at " << at << " was refused");
			return;
		}
		std::optional<size_t> index = scheduler_->find(id.c_str());
		REQUIRE_MESSAGE(index.has_value(), "no schedule " << id);
		hc::ScheduleSpec values = scheduler_->spec(*index);
		uint16_t members = 0;
		for (const auto& entry : change.members) {
			uint16_t member = hc::ScheduleMember::of_name(entry.first.c_str(), entry.first.size());
			if (member == 0) {
				REQUIRE_MESSAGE((entry.first == "id" || entry.first == "refused" || entry.first == "enabled"),
					"unknown setSchedule member " << entry.first);
				continue;
			}
			read_member(entry.second, member, values);
			members = static_cast<uint16_t>(members | member);
		}
		hc::ScheduleChange result = hc::ScheduleChange::ok;
		if (members != 0) {
			result = scheduler_->set_members(id.c_str(), members, values);
		}
		if (result == hc::ScheduleChange::ok && change.has("enabled")) {
			result = scheduler_->set_enabled(id.c_str(), change["enabled"].boolean) ? hc::ScheduleChange::ok
				: hc::ScheduleChange::not_adjustable;
		}
		std::string got = result == hc::ScheduleChange::ok ? "" : hc::schedule_change_name(result);
		CHECK_MESSAGE(got == refused, "setSchedule at " << at << ": " << (got.empty() ? "accepted" : got) << ", expected "
			<< (refused.empty() ? "accepted" : refused));
	}

	void compare_schedules(const Json& expected, const std::string& where) {
		REQUIRE_MESSAGE(scheduler_ != nullptr, where << ": `schedules` needs full schedule entries");
		for (const auto& entry : expected.members) {
			std::optional<size_t> index = scheduler_->find(entry.first.c_str());
			REQUIRE_MESSAGE(index.has_value(), where << ": no schedule " << entry.first);
			const hc::ScheduleSpec& spec = scheduler_->spec(*index);
			for (const auto& value : entry.second["values"].members) {
				uint16_t member = hc::ScheduleMember::of_name(value.first.c_str(), value.first.size());
				REQUIRE_MESSAGE(member != 0, "unknown member " << value.first);
				hc::ScheduleSpec want = spec;
				read_member(value.second, member, want);
				CHECK_MESSAGE(hc::schedule_member_value(spec, member) == hc::schedule_member_value(want, member),
					where << ": " << entry.first << "." << value.first << " is " << hc::schedule_member_value(spec, member)
					<< ", expected " << hc::schedule_member_value(want, member));
			}
			if (entry.second.has("overridden")) {
				uint16_t want = 0;
				for (const Json& name : entry.second["overridden"].items) {
					want = static_cast<uint16_t>(want | hc::ScheduleMember::of_name(name.text.c_str(), name.text.size()));
				}
				CHECK_MESSAGE(scheduler_->overridden(*index) == want, where << ": overridden members of " << entry.first
					<< " are " << scheduler_->overridden(*index) << ", expected " << want);
			}
			if (entry.second.has("enabled")) {
				CHECK_MESSAGE(scheduler_->enabled(*index) == entry.second["enabled"].boolean, where << ": " << entry.first
					<< " enabled");
			}
		}
	}


	/// The `rules` of the case or of an image step; none: an empty array (block cases).
	const Json& rules_of(const Json& image) const { return image.has("rules") ? image["rules"] : empty_rules_; }

	/// Which datapoints another component owns (spec/blocks.md B14): the schedule outputs of the case
	/// and the implied datapoints.
	struct Owners : hc::blocks::BlockDirectory {
		explicit Owners(const Schedules& s) : schedules(s) {}
		const char* owner(const char* datapoint) const override {
			if (schedules.owns_output(datapoint)) {
				return "a schedule entry";
			}
			if (std::strncmp(datapoint, "rule/", 5) == 0) {
				return "the rule engine";
			}
			if (std::strncmp(datapoint, "schedule/", 9) == 0) {
				return "a schedule entry";
			}
			if (std::strncmp(datapoint, "block/", 6) == 0) {
				return "the block runtime";
			}
			return nullptr;
		}
		const Schedules& schedules;
	};

	/// A new block runtime on the current engine with `set` (the boot or the reboot).
	void make_blocks(hc::blocks::BlockSet&& set) {
		blocks_ = std::make_unique<hc::blocks::BlockRuntime>(loop_, model_, clock_, settings_, blobs_, *engine_);
		blocks_->set_observer(&recorder_);
		REQUIRE(blocks_->adopt(std::move(set)));
	}

	/// README "persisted": a state record `block/<id>` written as the device writes it.
	void persist_record(const std::string& id, const Json& record) {
		uint32_t major = 0, minor = 0;
		REQUIRE(std::sscanf(record["version"].text.c_str(), "%u.%u", &major, &minor) == 2);
		const hc::blocks::BlockType* type = hc_test::corpus_registry().find(record["type"].text.c_str(),
			static_cast<uint16_t>(major));
		REQUIRE_MESSAGE(type != nullptr, "no block type " << record["type"].text);
		const hc::blocks::Manifest& m = type->manifest();
		std::vector<double> values(m.state_count);
		for (size_t f = 0; f < m.state_count; ++f) {
			values[f] = m.state[f].initial;
		}
		for (const auto& field : record["fields"].members) {
			int f = m.field(field.first.c_str());
			REQUIRE_MESSAGE(f >= 0, "no state field " << field.first);
			values[static_cast<size_t>(f)] = field.second.number;
		}
		uint8_t data[hc::blocks::BlockLimits::kRecordBytes];
		size_t length = 0;
		REQUIRE(hc::blocks::BlockRuntime::encode_record(m, id.c_str(), values.data(), data, sizeof(data), length));
		char name[hc::persist::kBlobNameSize];
		hc::persist::blob_name(hc::blocks::BlockRuntime::kBlobPrefix, id.c_str(), name);
		hc::BlobChunk chunk{data, length};
		REQUIRE(blobs_.write(name, &chunk, 1) == hc::BlobStatus::ok);
	}

	/// README "setBlock": the change of PUT /api/blocks/<id>, checked whole, then applied.
	void set_block(const Json& change, uint64_t at) {
		const std::string& id = change["block"].text;
		std::string refused = change.has("refused") ? change["refused"].text : "";
		hc::JsonSpan parameters;
		bool has_parameters = change.has("parameters");
		if (has_parameters) {
			parameters = timeline_member(current_step_, "setBlock", "parameters");
		}
		hc::blocks::ChangeStatus status = hc::blocks::ChangeStatus::ok;
		const char* reason = "";
		std::string member;
		std::vector<std::pair<std::string, double>> values;
		size_t position = 0;
		hc::JsonSpan key, value;
		while (has_parameters && status == hc::blocks::ChangeStatus::ok && hc::json_next_member(parameters, position, key, value)) {
			char name[40];
			REQUIRE(hc::json_string(key, name, sizeof(name)));
			double v = 0;
			status = blocks_->check_parameter(id.c_str(), name, &value, &v, &reason);
			member = name;
			values.emplace_back(name, v);
		}
		for (const Json& name : change["reset"].items) {
			if (status == hc::blocks::ChangeStatus::ok) {
				status = blocks_->check_parameter(id.c_str(), name.text.c_str(), nullptr, nullptr, &reason);
				member = name.text;
			}
		}
		if (status == hc::blocks::ChangeStatus::ok && change.has("enabled")) {
			status = blocks_->check_enabled(id.c_str(), &reason);
			member = "enabled";
		}
		if (status == hc::blocks::ChangeStatus::ok && change.has("restart")) {
			status = blocks_->check_restart(id.c_str(), &reason);
			member = "restart";
		}
		if (status != hc::blocks::ChangeStatus::ok && status != hc::blocks::ChangeStatus::not_faulted) {
			blocks_->refused(id.c_str(), member.c_str(), reason);
		}
		std::string got = status == hc::blocks::ChangeStatus::ok ? "" : hc::blocks::change_status_name(status);
		CHECK_MESSAGE(got == refused, "setBlock at " << at << ": " << (got.empty() ? "accepted" : got) << " (" << reason
			<< "), expected " << (refused.empty() ? "accepted" : refused));
		if (status != hc::blocks::ChangeStatus::ok) {
			return;
		}
		for (const auto& v : values) {
			REQUIRE(blocks_->set_parameter(id.c_str(), v.first.c_str(), v.second, &reason) == hc::blocks::ChangeStatus::ok);
		}
		for (const Json& name : change["reset"].items) {
			REQUIRE(blocks_->reset_parameter(id.c_str(), name.text.c_str(), &reason) == hc::blocks::ChangeStatus::ok);
		}
		if (change.has("enabled")) {
			REQUIRE(blocks_->set_enabled(id.c_str(), change["enabled"].boolean, &reason) == hc::blocks::ChangeStatus::ok);
		}
		if (change.has("restart")) {
			REQUIRE(blocks_->restart(id.c_str(), &reason) == hc::blocks::ChangeStatus::ok);
		}
	}

	int block_index(const std::string& id) {
		int b = blocks_->find(id.c_str());
		REQUIRE_MESSAGE(b >= 0, "no block " << id);
		return b;
	}

	static bool same_number(double have, double want) {
		return have == want || std::fabs(have - want) <= 1e-9 * std::max(1.0, std::max(std::fabs(have), std::fabs(want)));
	}

	static double number_of(const Json& json) {
		return json.kind == Json::Kind::boolean ? (json.boolean ? 1 : 0) : json.number;
	}

	void compare_blocks(const Json& expected, const std::string& where) {
		for (const auto& entry : expected.members) {
			size_t b = static_cast<size_t>(block_index(entry.first));
			const Json& want = entry.second;
			hc::blocks::BlockDiagnostics d;
			REQUIRE(blocks_->diagnostics(b, d));
			const hc::blocks::Manifest& m = blocks_->set().at(b).type->manifest();
			if (want.has("status")) {
				CHECK_MESSAGE(std::string(hc::blocks::block_status_name(d.status)) == want["status"].text, where << ": block "
					<< entry.first << " is " << hc::blocks::block_status_name(d.status) << ", expected " << want["status"].text);
			}
			if (want.has("calls")) {
				CHECK_MESSAGE(d.calls == static_cast<uint32_t>(want["calls"].number), where << ": block " << entry.first
					<< " made " << d.calls << " calls, expected " << want["calls"].text);
			}
			for (const auto& field : want["state"].members) {
				int f = m.field(field.first.c_str());
				REQUIRE_MESSAGE(f >= 0, "no state field " << field.first);
				double have = blocks_->state(b)[f];
				CHECK_MESSAGE(same_number(have, number_of(field.second)), where << ": " << entry.first << "." << field.first
					<< " is " << have << ", expected " << field.second.text);
			}
			for (const auto& param : want["parameters"].members) {
				int p = m.param(param.first.c_str());
				REQUIRE_MESSAGE(p >= 0, "no parameter " << param.first);
				double have = blocks_->param(b, static_cast<size_t>(p));
				CHECK_MESSAGE(same_number(have, number_of(param.second)), where << ": parameter " << entry.first << "/"
					<< param.first << " is " << have << ", expected " << param.second.text);
			}
		}
	}

	/// The complete set of persisted records (README "records"), after an orderly shutdown's writes.
	void compare_records(const Json& expected, const std::string& where) {
		blocks_->flush();
		std::vector<std::string> expected_names;
		for (const auto& entry : expected.members) {
			char name[hc::persist::kBlobNameSize];
			hc::persist::blob_name(hc::blocks::BlockRuntime::kBlobPrefix, entry.first.c_str(), name);
			auto it = blobs_.blobs.find(name);
			if (entry.second.kind == Json::Kind::null) {
				CHECK_MESSAGE(it == blobs_.blobs.end(), where << ": a record of " << entry.first << " is stored");
				continue;
			}
			expected_names.push_back(name);
			if (it == blobs_.blobs.end()) {
				CHECK_MESSAGE(false, where << ": no record of " << entry.first);
				continue;
			}
			size_t b = static_cast<size_t>(block_index(entry.first));
			const hc::blocks::Manifest& m = blocks_->set().at(b).type->manifest();
			std::vector<double> values(m.state_count, std::nan(""));
			const char* reason = "";
			CHECK_MESSAGE(hc::blocks::BlockRuntime::decode_record(m, entry.first.c_str(), it->second.data(), it->second.size(),
				values.data(), &reason), where << ": the record of " << entry.first << " is " << reason);
			for (size_t f = 0; f < m.state_count; ++f) {
				if (!m.state[f].persist) {
					continue;
				}
				const Json& field = entry.second[m.state[f].id];
				CHECK_MESSAGE(entry.second.has(m.state[f].id), where << ": the record of " << entry.first << " holds "
					<< m.state[f].id << " = " << values[f] << ", not expected");
				if (entry.second.has(m.state[f].id)) {
					CHECK_MESSAGE(same_number(values[f], number_of(field)), where << ": the record of " << entry.first << ": "
						<< m.state[f].id << " is " << values[f] << ", expected " << field.text);
				}
			}
		}
		for (const auto& blob : blobs_.blobs) {
			if (blob.first.compare(0, 2, hc::blocks::BlockRuntime::kBlobPrefix) == 0) {
				CHECK_MESSAGE(std::find(expected_names.begin(), expected_names.end(), blob.first) != expected_names.end(),
					where << ": an unexpected record " << blob.first << " is stored");
			}
		}
	}

	int rule_index(const std::string& id) {
		int r = engine_->rules().find_rule(id.c_str());
		REQUIRE_MESSAGE(r >= 0, "no rule " << id);
		return r;
	}

	void compare_state(const Json& expected, const std::string& where) {
		for (const auto& entry : expected["datapoints"].members) {
			const hc::Datapoint& dp = datapoint(entry.first);
			Val want = expected_value(hc::rules::type_of_datapoint(dp.type()), entry.second);
			Val have = dp.quality() == hc::Quality::unknown ? Val::none() : hc::rules::val_from_datapoint(dp.value());
			CHECK_MESSAGE(same(have, want), where << ": datapoint " << entry.first << " is " << text_of(have)
				<< ", expected " << text_of(want));
		}
		for (const auto& entry : expected["states"].members) {
			const char* state = engine_->current_state(static_cast<size_t>(rule_index(entry.first)));
			std::string have = state ? state : "(none)";
			CHECK_MESSAGE(have == entry.second.text, where << ": state of " << entry.first << " is " << have
				<< ", expected " << entry.second.text);
		}
		for (const auto& entry : expected["parameters"].members) {
			size_t slash = entry.first.find('/');
			int r = rule_index(entry.first.substr(0, slash));
			int p = engine_->rules().find_parameter(static_cast<size_t>(r), entry.first.substr(slash + 1).c_str());
			REQUIRE_MESSAGE(p >= 0, "no parameter " << entry.first);
			const Val& have = engine_->parameter_value(static_cast<size_t>(r), static_cast<size_t>(p));
			Val want = expected_value(have.type, entry.second);
			CHECK_MESSAGE(same(have, want), where << ": parameter " << entry.first << " is " << text_of(have)
				<< ", expected " << text_of(want));
		}
		for (const auto& entry : expected["timers"].members) {
			size_t slash = entry.first.find('/');
			int r = rule_index(entry.first.substr(0, slash));
			int t = engine_->rules().find_timer(static_cast<size_t>(r), entry.first.substr(slash + 1).c_str());
			REQUIRE_MESSAGE(t >= 0, "no timer " << entry.first);
			int64_t ms = 0;
			bool running = engine_->timer_remaining(static_cast<size_t>(r), static_cast<size_t>(t), ms);
			std::string have = running ? std::to_string(ms) : "null";
			std::string want = entry.second.kind == Json::Kind::null ? "null" : entry.second.text;
			CHECK_MESSAGE(have == want, where << ": timer " << entry.first << " is " << have << ", expected " << want);
		}
		for (const auto& entry : expected["names"].members) {
			hc::rules::RuleDiagnostics d;
			REQUIRE(engine_->diagnostics(static_cast<size_t>(rule_index(entry.first)), d));
			CHECK_MESSAGE(std::string(d.name) == entry.second.text, where << ": name of " << entry.first << " is "
				<< std::string(d.name) << ", expected " << entry.second.text);
		}
		for (const auto& entry : expected["labels"].members) {
			size_t slash = entry.first.find('/');
			int r = rule_index(entry.first.substr(0, slash));
			int p = engine_->rules().find_parameter(static_cast<size_t>(r), entry.first.substr(slash + 1).c_str());
			REQUIRE_MESSAGE(p >= 0, "no parameter " << entry.first);
			const hc::rules::RuleSet& set = engine_->rules();
			std::string have = set.str(set.parameter(set.rule(static_cast<size_t>(r)).first_parameter + p).label);
			CHECK_MESSAGE(have == entry.second.text, where << ": label of " << entry.first << " is " << have
				<< ", expected " << entry.second.text);
		}
		for (const auto& entry : expected["firings"].members) {
			hc::rules::RuleDiagnostics d;
			REQUIRE(engine_->diagnostics(static_cast<size_t>(rule_index(entry.first)), d));
			CHECK_MESSAGE(d.firings == static_cast<uint32_t>(entry.second.number), where << ": firings of "
				<< entry.first << " are " << d.firings << ", expected " << entry.second.text);
		}
		if (expected.has("schedules")) {
			compare_schedules(expected["schedules"], where);
		}
		if (expected.has("blocks")) {
			compare_blocks(expected["blocks"], where);
		}
		if (expected.has("records")) {
			compare_records(expected["records"], where);
		}
		if (expected.has("persisted")) {
			engine_->persist_now(); // the delayed state writes (README: the store as an orderly shutdown leaves it)
			compare_persisted(expected["persisted"], where);
		}
	}

	void compare_persisted(const Json& expected, const std::string& where) {
		std::map<std::string, Val> stored;
		for (const auto& entry : settings_.entries) {
			if (entry.first.compare(0, 3, "rp.") != 0) {
				continue;
			}
			char logical[64];
			Val v;
			bool ok = hc::rules::ParameterStore::decode(entry.second.text.c_str(), logical, sizeof(logical), v);
			CHECK_MESSAGE(ok, where << ": unreadable stored override " << entry.first << " = " << entry.second.text);
			char key[16];
			std::string name = logical;
			size_t slash = name.find('/');
			hc::rules::ParameterStore::key_for(name.substr(0, slash).c_str(), name.substr(slash + 1).c_str(), key);
			CHECK_MESSAGE(entry.first == key, where << ": override " << name << " under the key " << entry.first);
			stored[name] = v;
		}
		for (const auto& entry : expected.members) {
			auto it = stored.find(entry.first);
			if (it == stored.end()) {
				CHECK_MESSAGE(false, where << ": override " << entry.first << " not stored");
				continue;
			}
			Val want = expected_value(it->second.type, entry.second);
			CHECK_MESSAGE(same(it->second, want), where << ": stored override " << entry.first << " is "
				<< text_of(it->second) << ", expected " << text_of(want));
		}
		for (const auto& entry : stored) {
			CHECK_MESSAGE(expected.has(entry.first), where << ": unexpected stored override " << entry.first << " = "
				<< text_of(entry.second));
		}
	}

	void compare_outputs() {
		const Json& expected = case_["expected"];
		std::string have_commands, want_commands;
		for (const CommandEntry& c : recorder_.commands) {
			have_commands += std::to_string(c.at) + " " + c.rule + " " + c.datapoint + " = " + text_of(c.value) + "\n";
		}
		bool commands_ok = recorder_.commands.size() == expected["commands"].items.size();
		for (size_t i = 0; i < expected["commands"].items.size(); ++i) {
			const Json& c = expected["commands"].items[i];
			const hc::Datapoint& dp = datapoint(c["datapoint"].text);
			Val want = expected_value(hc::rules::type_of_datapoint(dp.type()), c["value"]);
			std::string who = c.has("block") ? "block " + c["block"].text : c["rule"].text;
			want_commands += c["at"].text + " " + who + " " + c["datapoint"].text + " = " + text_of(want) + "\n";
			if (commands_ok) {
				const CommandEntry& h = recorder_.commands[i];
				commands_ok = h.at == static_cast<uint64_t>(c["at"].number) && h.rule == who &&
					h.datapoint == c["datapoint"].text && same(h.value, want);
			}
		}
		CHECK_MESSAGE(commands_ok, "commands differ\nexpected:\n" << want_commands << "actual:\n" << have_commands);

		std::string have_reads, want_reads;
		for (const ReadEntry& r : recorder_.reads) {
			have_reads += std::to_string(r.at) + " " + r.rule + " " + r.datapoint + "\n";
		}
		bool reads_ok = recorder_.reads.size() == expected["reads"].items.size();
		for (size_t i = 0; i < expected["reads"].items.size(); ++i) {
			const Json& r = expected["reads"].items[i];
			want_reads += r["at"].text + " " + r["rule"].text + " " + r["datapoint"].text + "\n";
			if (reads_ok) {
				const ReadEntry& h = recorder_.reads[i];
				reads_ok = h.at == static_cast<uint64_t>(r["at"].number) && h.rule == r["rule"].text &&
					h.datapoint == r["datapoint"].text;
			}
		}
		CHECK_MESSAGE(reads_ok, "reads differ\nexpected:\n" << want_reads << "actual:\n" << have_reads);

		std::string have_logs, want_logs;
		for (const LogEntry& l : recorder_.logs) {
			have_logs += std::to_string(l.at) + " " + l.rule + " " + l.code;
			have_logs += l.code == "log" ? " " + l.level + " \"" + l.message + "\"\n" : " (" + l.message + ")\n";
			if (!l.culprit.empty()) {
				have_logs.back() = ' ';
				have_logs += "culprit " + l.culprit + " " + std::to_string(l.firings) + "\n";
			}
		}
		bool logs_ok = recorder_.logs.size() == expected["logs"].items.size();
		for (size_t i = 0; i < expected["logs"].items.size(); ++i) {
			const Json& l = expected["logs"].items[i];
			std::string who = l.has("block") ? "block " + l["block"].text : l["rule"].text;
			std::string level = l.has("level") ? l["level"].text : "info";
			want_logs += l["at"].text + " " + who + " " + l["code"].text;
			want_logs += l["code"].text == "log" ? " " + level + " \"" + l["message"].text + "\"\n" : "\n";
			if (l.has("culprit")) {
				want_logs.back() = ' ';
				want_logs += "culprit " + l["culprit"].text + " " + std::to_string(static_cast<uint32_t>(l["firings"].number)) + "\n";
			}
			if (logs_ok) {
				const LogEntry& h = recorder_.logs[i];
				logs_ok = h.at == static_cast<uint64_t>(l["at"].number) && h.rule == who && h.code == l["code"].text;
				if (logs_ok && h.code == "log") {
					logs_ok = h.level == level && h.message == l["message"].text;
				}
				if (logs_ok && l.has("culprit")) {
					logs_ok = h.culprit == l["culprit"].text && h.firings == static_cast<uint32_t>(l["firings"].number);
				}
			}
		}
		CHECK_MESSAGE(logs_ok, "logs differ\nexpected:\n" << want_logs << "actual:\n" << have_logs);
	}

	const Json& case_;
	const std::string& text_;
	JsonTree tree_;
	hc_test::FakeClock clock_;
	hc_test::FakeSettings settings_;
	CaseTime time_;
	EventLoop loop_;
	DatapointModel model_{clock_};
	hc_test::TestTechnology technologies_[4];
	Schedules schedules_;
	Recorder recorder_{clock_};
	std::vector<std::string> names_;
	std::vector<DatapointSpec> specs_;
	const Json* image_ = nullptr;
	Json empty_rules_;
	hc::JsonSpan blocks_span_ = kNoBlocks;
	hc_test::FakeBlobStore blobs_;
	Owners owners_{schedules_};
	std::unique_ptr<Engine> engine_;
	std::unique_ptr<hc::blocks::BlockRuntime> blocks_; ///< Declared after the engine: destroyed first.
	std::unique_ptr<SchedulePlan> schedule_plan_;
	Firings firings_{*this};
	std::unique_ptr<hc::Scheduler> scheduler_; ///< Declared after the engine: destroyed first.
	size_t current_step_ = 0;
};

void run_case(const char* prefix, const char* directory = "rules") {
	std::string path;
	for (const std::string& file : hc_test::corpus_files(directory)) {
		if (std::filesystem::path(file).filename().string().compare(0, std::strlen(prefix), prefix) == 0) {
			path = file;
		}
	}
	REQUIRE_MESSAGE(!path.empty(), "no corpus case " << prefix);
	std::string text, error;
	REQUIRE(hc_test::read_text(path, text));
	Json json;
	REQUIRE_MESSAGE(hc_test::parse_json(text, json, error), path << ": " << error);
	hc_test::ScopedCapture capture(hc::LogLevel::error); // keep the test output short
	CaseRunner runner(json, text);
	runner.run();
}

} // namespace

#define HC_RULE_CASE(number, name) \
	TEST_CASE("RulesCorpus " number " " name) { run_case(number "-" name ".json"); }

HC_RULE_CASE("01", "trigger-datapoint-change")
HC_RULE_CASE("02", "trigger-datapoint-origin")
HC_RULE_CASE("03", "trigger-match-edge")
HC_RULE_CASE("04", "trigger-schedule")
HC_RULE_CASE("05", "timer-presence-light")
HC_RULE_CASE("06", "trigger-start")
HC_RULE_CASE("07", "trigger-parameter")
HC_RULE_CASE("08", "operators-arithmetic")
HC_RULE_CASE("09", "operators-comparison-logic")
HC_RULE_CASE("10", "operators-duration-time")
HC_RULE_CASE("11", "functions-numeric")
HC_RULE_CASE("12", "functions-string-color-controlled")
HC_RULE_CASE("13", "null-condition")
HC_RULE_CASE("14", "null-stale-known-lastknown")
HC_RULE_CASE("15", "eval-error-once")
HC_RULE_CASE("16", "write-refused-onlyonchange")
HC_RULE_CASE("17", "time-functions")
HC_RULE_CASE("18", "history-since-change")
HC_RULE_CASE("19", "function-hysteresis")
HC_RULE_CASE("20", "timers-start-restart-cancel")
HC_RULE_CASE("21", "state-machine-staircase")
HC_RULE_CASE("22", "state-machine-order")
HC_RULE_CASE("23", "queue-order")
HC_RULE_CASE("24", "loop-guard")
HC_RULE_CASE("25", "image-swap-state-timers")
HC_RULE_CASE("26", "image-swap-parameters")
HC_RULE_CASE("27", "parameter-persistence-reboot")
HC_RULE_CASE("28", "enable-switch")
HC_RULE_CASE("29", "set-parameter-action")
HC_RULE_CASE("30", "log-format")
HC_RULE_CASE("31", "load-refused-type-error")
HC_RULE_CASE("32", "load-refused-match-time")
HC_RULE_CASE("33", "load-refused-unknown-timer")
HC_RULE_CASE("34", "example-heating-setback")
HC_RULE_CASE("35", "example-blind-sun-wind")
HC_RULE_CASE("36", "parameter-reset-to-default")
HC_RULE_CASE("37", "state-persistence-reboot")
HC_RULE_CASE("38", "date-time-across-dst")
HC_RULE_CASE("39", "load-refused-schedule-output")
HC_RULE_CASE("40", "labels-language")
HC_RULE_CASE("41", "canonical-normalization")
HC_RULE_CASE("42", "state-override-fold-labels")
HC_RULE_CASE("43", "action-read")
HC_RULE_CASE("44", "action-read-refused")
HC_RULE_CASE("45", "load-refused-not-readable")
HC_RULE_CASE("46", "trigger-silence")
HC_RULE_CASE("47", "silence-enable-parameter")
HC_RULE_CASE("48", "silence-image-swap")
HC_RULE_CASE("49", "silence-reboot")
HC_RULE_CASE("50", "parameter-datapoint-change")
HC_RULE_CASE("51", "parameter-datapoint-command")
HC_RULE_CASE("52", "parameter-datapoint-other-rule")
HC_RULE_CASE("53", "parameter-datapoint-enabled")
HC_RULE_CASE("54", "silence-read-parameter-datapoint")
HC_RULE_CASE("55", "schedule-adjust-time")
HC_RULE_CASE("56", "schedule-change-semantics")
HC_RULE_CASE("57", "schedule-time-datapoint")
HC_RULE_CASE("58", "schedule-image-swap-overrides")

TEST_CASE("RulesCorpus every case has a test") {
	std::vector<std::string> files = hc_test::rule_corpus_files();
	CHECK(files.size() == 58);
}

// The block conformance corpus (spec/fixtures/blocks/README.md), run by the same runner.
#define HC_BLOCK_CASE(number, name) \
	TEST_CASE("BlocksCorpus " number " " name) { run_case(number "-" name ".json", "blocks"); }

HC_BLOCK_CASE("01", "load-refused-unknown-type")
HC_BLOCK_CASE("02", "load-refused-missing-input")
HC_BLOCK_CASE("03", "load-refused-type-mismatch")
HC_BLOCK_CASE("04", "load-refused-output-owned")
HC_BLOCK_CASE("05", "load-refused-parameter-bounds")
HC_BLOCK_CASE("06", "load-refused-aggregation")
HC_BLOCK_CASE("07", "load-refused-unknown-member")
HC_BLOCK_CASE("08", "calls-inputs-outputs")
HC_BLOCK_CASE("09", "fault-failsafe-restart")
HC_BLOCK_CASE("10", "disable-enable-failsafe")
HC_BLOCK_CASE("11", "image-swap-keeps-state")
HC_BLOCK_CASE("12", "image-swap-drops-state")
HC_BLOCK_CASE("13", "quiet-load")
HC_BLOCK_CASE("14", "loop-guard")
HC_BLOCK_CASE("15", "persistence-reboot")
HC_BLOCK_CASE("16", "parameter-api")
HC_BLOCK_CASE("17", "room-pi-step")
HC_BLOCK_CASE("18", "room-window-no-windup")
HC_BLOCK_CASE("19", "room-sensor-timeout")
HC_BLOCK_CASE("20", "room-pwm")
HC_BLOCK_CASE("21", "room-two-point")
HC_BLOCK_CASE("22", "room-boost")
HC_BLOCK_CASE("23", "room-valve-protection")
HC_BLOCK_CASE("24", "room-modes")
HC_BLOCK_CASE("25", "room-integral-reboot")
HC_BLOCK_CASE("26", "fault-disabled-swap")
HC_BLOCK_CASE("27", "demand-hysteresis-min-times")
HC_BLOCK_CASE("28", "demand-top-mean-stale")
HC_BLOCK_CASE("29", "demand-pump-run-on")
HC_BLOCK_CASE("30", "demand-offset-persist")
HC_BLOCK_CASE("31", "demand-32-valves")
HC_BLOCK_CASE("32", "load-refused-multiple")
HC_BLOCK_CASE("33", "load-refused-demand-thresholds")
HC_BLOCK_CASE("34", "curve-forms")
HC_BLOCK_CASE("35", "curve-filter-persist")
HC_BLOCK_CASE("36", "curve-summer-switch")
HC_BLOCK_CASE("37", "curve-frost")
HC_BLOCK_CASE("38", "curve-sensor-timeout")
HC_BLOCK_CASE("39", "load-refused-curve-points")
HC_BLOCK_CASE("40", "heating-chain")
HC_BLOCK_CASE("41", "demand-image-swap-keeps-hold")
HC_BLOCK_CASE("42", "shading-sun-on-facade")
HC_BLOCK_CASE("43", "shading-slat-tracking")
HC_BLOCK_CASE("44", "shading-brightness")
HC_BLOCK_CASE("45", "shading-wind")
HC_BLOCK_CASE("46", "shading-rain-frost")
HC_BLOCK_CASE("47", "shading-manual-duration-sun-change")
HC_BLOCK_CASE("48", "shading-manual-midnight")
HC_BLOCK_CASE("49", "shading-heat-protection")
HC_BLOCK_CASE("50", "shading-min-interval-reboot")
HC_BLOCK_CASE("51", "load-refused-own-output")
HC_BLOCK_CASE("52", "load-refused-shading-horizon")
HC_BLOCK_CASE("53", "shading-failsafe-parameters")
HC_BLOCK_CASE("54", "shading-sensors-one-by-one")

TEST_CASE("BlocksCorpus every case has a test") {
	std::vector<std::string> files = hc_test::corpus_files("blocks");
	CHECK(files.size() == 54);
}
