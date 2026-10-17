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

#include "doctest.h"
#include "hc/datapoint.h"
#include "hc/event_loop.h"
#include "hc/rules/engine.h"
#include "hc/rules/parameter_store.h"
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

std::vector<std::string> rule_corpus_files() {
	std::vector<std::string> files;
	std::error_code error;
	for (const auto& entry : std::filesystem::directory_iterator(HC_SPEC_DIR "/fixtures/rules", error)) {
		std::string name = entry.path().filename().string();
		if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0) {
			files.push_back(entry.path().string());
		}
	}
	std::sort(files.begin(), files.end());
	return files;
}

} // namespace hc_test

namespace {

constexpr uint64_t kBase = 1000000; ///< Monotonic time of the corpus's time 0.

/// The wall clock and the sun of a case.
class CaseTime : public hc::rules::TimeSource {
public:
	bool local_now(hc::DateTime& out) const override {
		if (!synchronized) {
			return false;
		}
		int64_t s = start_seconds + static_cast<int64_t>((clock->now_ms - kBase) / 1000);
		int64_t days = s >= 0 ? s / 86400 : (s - 86399) / 86400;
		int64_t rest = s - days * 86400;
		int32_t year = 0;
		unsigned month = 0, day = 0;
		hc::rules::civil_of(days, year, month, day);
		out = hc::DateTime();
		out.year = static_cast<uint16_t>(year);
		out.month = static_cast<uint8_t>(month);
		out.day = static_cast<uint8_t>(day);
		out.hour = static_cast<uint8_t>(rest / 3600);
		out.minute = static_cast<uint8_t>(rest % 3600 / 60);
		out.second = static_cast<uint8_t>(rest % 60);
		out.day_of_week = hc::rules::weekday_of(year, month, day);
		return true;
	}

	bool sun_time(hc::rules::SunEvent event, hc::TimeOfDay& out) const override {
		const Val& v = sun[static_cast<size_t>(event)];
		if (!v.has()) {
			return false;
		}
		out = v.u.tod;
		return true;
	}

	const hc_test::FakeClock* clock = nullptr;
	bool synchronized = false;
	int64_t start_seconds = 0;
	Val sun[4];
};

struct LogEntry {
	uint64_t at;
	std::string rule;
	std::string code;
	std::string level;
	std::string message;
};

struct CommandEntry {
	uint64_t at;
	std::string rule;
	std::string datapoint;
	Val value;
};

class Recorder : public hc::rules::EngineObserver {
public:
	explicit Recorder(const hc_test::FakeClock& clock) : clock_(clock) {}

	void rule_logged(const char* rule, LogCode code, hc::LogLevel level, const char* text) override {
		const char* level_name = level == hc::LogLevel::error ? "error" : (level == hc::LogLevel::warn ? "warning" : "info");
		logs.push_back(LogEntry{clock_.now_ms - kBase, rule, hc::rules::log_code_name(code), level_name, text});
	}

	void rule_commanded(const char* rule, const hc::Datapoint& datapoint, const hc::Value& value) override {
		commands.push_back(CommandEntry{clock_.now_ms - kBase, rule, datapoint.name(), hc::rules::val_from_datapoint(value)});
	}

	std::vector<LogEntry> logs;
	std::vector<CommandEntry> commands;

private:
	const hc_test::FakeClock& clock_;
};

std::string text_of(const Val& v) {
	char buffer[256];
	hc::rules::literal_text(v, buffer, sizeof(buffer));
	return v.has() ? std::string(hc::rules::type_name(v.type)) + " " + buffer : "null";
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

class CaseRunner {
public:
	explicit CaseRunner(const Json& json) : case_(json) { refusing_.refuse = true; }

	void run() {
		time_.clock = &clock_;
		clock_.now_ms = kBase;
		if (case_.has("clock")) {
			const Json& c = case_["clock"];
			int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
			REQUIRE(std::sscanf(c["start"].text.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6);
			time_.start_seconds = hc::rules::days_of(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400 +
				h * 3600 + mi * 60 + s;
			time_.synchronized = !c.has("synchronized") || c["synchronized"].boolean;
		}
		static const char* const kSun[] = {"sunrise", "sunset", "dawn", "dusk"};
		for (size_t i = 0; i < 4; ++i) {
			if (case_["astro"].has(kSun[i])) {
				REQUIRE(hc::rules::val_from_json(Type::time_of_day, tree_.value(case_["astro"][kSun[i]]), time_.sun[i]) ==
					hc::rules::LiteralStatus::ok);
			}
		}
		for (const Json& s : case_["schedules"].items) {
			schedules_.ids.push_back(s.text);
		}
		load_model();
		for (const auto& entry : case_["persisted"].members) {
			size_t slash = entry.first.find('/');
			REQUIRE(slash != std::string::npos);
			Val v;
			Type type = hc::rules::type_from_name(entry.second["type"].text.c_str());
			REQUIRE(hc::rules::val_from_json(type, tree_.value(entry.second["value"]), v) == hc::rules::LiteralStatus::ok);
			hc::rules::ParameterStore store(settings_);
			REQUIRE(store.write(entry.first.substr(0, slash).c_str(), entry.first.substr(slash + 1).c_str(), v));
		}
		for (const auto& entry : case_["initial"].members) {
			report(entry.first, entry.second);
		}
		image_ = &case_["rules"];
		engine_ = std::make_unique<Engine>(loop_, model_, clock_, settings_, time_);
		engine_->set_observer(&recorder_);
		LoadResult result = engine_->load(tree_.value(*image_), &schedules_);
		if (case_["expected"].has("loadRefused")) {
			CHECK_MESSAGE(!result.ok(), "the load must be refused");
			CHECK(std::string(hc::rules::load_error_name(result.error)) == case_["expected"]["loadRefused"].text);
			MESSAGE("refusal: " << std::string(result.rule) << " at " << std::string(result.path) << ": " << std::string(result.text));
			return;
		}
		REQUIRE_MESSAGE(result.ok(), "load refused: " << std::string(hc::rules::load_error_name(result.error)) << " rule "
			<< std::string(result.rule) << " at " << std::string(result.path) << ": " << std::string(result.text));
		engine_->start(StartReason::boot);
		advance(0);
		for (const Json& step : case_["timeline"].items) {
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
		std::vector<std::string> ids;
	};

	void load_model() {
		names_.clear();
		specs_.clear();
		for (const Json& dp : case_["datapoints"].items) {
			names_.push_back(dp["name"].text);
		}
		for (size_t i = 0; i < names_.size(); ++i) {
			const Json& dp = case_["datapoints"].items[i];
			hc::ValueType type = hc::rules::datapoint_type_from_name(dp["type"].text.c_str());
			REQUIRE_MESSAGE(type != hc::ValueType::none, "datapoint type " << dp["type"].text);
			bool writable = !dp.has("writable") || dp["writable"].boolean;
			uint8_t flags = hc::DatapointFlags::kReadable |
				(writable ? hc::DatapointFlags::kWritable | hc::DatapointFlags::kTransmit : 0);
			specs_.push_back(DatapointSpec{names_[i].c_str(), type, flags});
		}
		REQUIRE(model_.load(specs_.data(), specs_.size()) == hc::DatapointStatus::ok);
		for (size_t i = 0; i < names_.size(); ++i) {
			const Json& dp = case_["datapoints"].items[i];
			bool refuse = dp.has("refuseCommands") && dp["refuseCommands"].boolean;
			REQUIRE(model_.bind(static_cast<uint16_t>(i), refuse ? refusing_ : accepting_) == hc::DatapointStatus::ok);
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
		if (type == Type::date_time && json.has("dayOfWeek")) {
			// The corpus notation adds the day of the week to a date-time (README "Values"); the
			// image's literal (spec 3.1) has none: it is computed from the date.
			Json copy = json;
			copy.members.erase("dayOfWeek");
			REQUIRE(hc::rules::val_from_json(type, tree_.value(copy), v, &reason) == hc::rules::LiteralStatus::ok);
			CHECK_MESSAGE(v.u.dt.day_of_week == static_cast<uint8_t>(json["dayOfWeek"].number),
				"day of week of an expected date-time");
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
			REQUIRE(model_.command(dp.id(), datapoint_value(dp, step["command"]["value"])) == hc::DatapointStatus::ok);
		} else if (step.has("schedule")) {
			engine_->schedule_fired(step["schedule"].text.c_str());
		} else if (step.has("setParameter")) {
			const Json& p = step["setParameter"];
			engine_->set_parameter(p["rule"].text.c_str(), p["parameter"].text.c_str(), tree_.value(p["value"]));
		} else if (step.has("clockSync")) {
			time_.synchronized = step["clockSync"].boolean;
		} else if (step.has("image")) {
			LoadResult result = engine_->load(tree_.value(step["image"]["rules"]), &schedules_);
			REQUIRE_MESSAGE(result.ok(), "image at " << at << " refused: " << std::string(hc::rules::load_error_name(result.error))
				<< " rule " << std::string(result.rule) << " at " << std::string(result.path) << ": " << std::string(result.text));
			image_ = &step["image"]["rules"];
			engine_->start(StartReason::image);
		} else if (step.has("reboot")) {
			engine_.reset();
			load_model();
			engine_ = std::make_unique<Engine>(loop_, model_, clock_, settings_, time_);
			engine_->set_observer(&recorder_);
			LoadResult result = engine_->load(tree_.value(*image_), &schedules_);
			REQUIRE(result.ok());
			engine_->start(StartReason::boot);
		} else if (step.has("check")) {
			compare_state(step["check"], "check at " + std::to_string(at));
		} else {
			FAIL("unknown timeline step at " << at);
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
		for (const auto& entry : expected["firings"].members) {
			hc::rules::RuleDiagnostics d;
			REQUIRE(engine_->diagnostics(static_cast<size_t>(rule_index(entry.first)), d));
			CHECK_MESSAGE(d.firings == static_cast<uint32_t>(entry.second.number), where << ": firings of "
				<< entry.first << " are " << d.firings << ", expected " << entry.second.text);
		}
		if (expected.has("persisted")) {
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
			want_commands += c["at"].text + " " + c["rule"].text + " " + c["datapoint"].text + " = " + text_of(want) + "\n";
			if (commands_ok) {
				const CommandEntry& h = recorder_.commands[i];
				commands_ok = h.at == static_cast<uint64_t>(c["at"].number) && h.rule == c["rule"].text &&
					h.datapoint == c["datapoint"].text && same(h.value, want);
			}
		}
		CHECK_MESSAGE(commands_ok, "commands differ\nexpected:\n" << want_commands << "actual:\n" << have_commands);

		std::string have_logs, want_logs;
		for (const LogEntry& l : recorder_.logs) {
			have_logs += std::to_string(l.at) + " " + l.rule + " " + l.code;
			have_logs += l.code == "log" ? " " + l.level + " \"" + l.message + "\"\n" : " (" + l.message + ")\n";
		}
		bool logs_ok = recorder_.logs.size() == expected["logs"].items.size();
		for (size_t i = 0; i < expected["logs"].items.size(); ++i) {
			const Json& l = expected["logs"].items[i];
			want_logs += l["at"].text + " " + l["rule"].text + " " + l["code"].text;
			want_logs += l["code"].text == "log" ? " " + l["level"].text + " \"" + l["message"].text + "\"\n" : "\n";
			if (logs_ok) {
				const LogEntry& h = recorder_.logs[i];
				logs_ok = h.at == static_cast<uint64_t>(l["at"].number) && h.rule == l["rule"].text && h.code == l["code"].text;
				if (logs_ok && h.code == "log") {
					logs_ok = h.level == l["level"].text && h.message == l["message"].text;
				}
			}
		}
		CHECK_MESSAGE(logs_ok, "logs differ\nexpected:\n" << want_logs << "actual:\n" << have_logs);
	}

	const Json& case_;
	JsonTree tree_;
	hc_test::FakeClock clock_;
	hc_test::FakeSettings settings_;
	CaseTime time_;
	EventLoop loop_;
	DatapointModel model_{clock_};
	hc_test::TestTechnology accepting_;
	hc_test::TestTechnology refusing_;
	Schedules schedules_;
	Recorder recorder_{clock_};
	std::vector<std::string> names_;
	std::vector<DatapointSpec> specs_;
	const Json* image_ = nullptr;
	std::unique_ptr<Engine> engine_;
};

void run_case(const char* prefix) {
	std::string path;
	for (const std::string& file : hc_test::rule_corpus_files()) {
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
	CaseRunner runner(json);
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

TEST_CASE("RulesCorpus every case has a test") {
	std::vector<std::string> files = hc_test::rule_corpus_files();
	CHECK(files.size() == 35);
}
