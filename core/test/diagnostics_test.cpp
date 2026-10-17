/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Diagnostics (issue #4, doc/diagnostics.md): the reset reason in the controller's first line, the
// boot facts in the status writer, per-tag log levels and the tail of the log ring.

#include <string>
#include <vector>

#include "doctest.h"
#include "hc/controller.h"
#include "hc/diagnostics.h"
#include "hc/json.h"
#include "hc/log_ring.h"
#include "hc/version.h"
#include "test_support.h"

using hc::Log;
using hc::LogLevel;
using hc::ResetReason;

namespace {

/// Reports a fixed reason and facts of every kind, like the device's boot record.
class FakeDiagnostics : public hc::Diagnostics {
public:
	ResetReason reset_reason() const override { return reason; }
	void visit_boot(hc::DiagnosticsVisitor& visitor) const override {
		if (!facts) {
			return;
		}
		visitor.number("count", 12);
		visitor.number("brownouts", 3);
		visitor.number("previous_uptime_s", 3600);
		visitor.flag("coredump", true);
		visitor.text("coredump_task", "hc_loop");
	}

	ResetReason reason = ResetReason::brownout;
	bool facts = true;
};

std::string boot_json(const hc::Diagnostics* diagnostics) {
	char buffer[512];
	hc::JsonWriter json(buffer, sizeof(buffer));
	json.begin_object();
	hc::write_boot_status(json, diagnostics);
	json.end_object();
	REQUIRE_FALSE(json.overflowed());
	return std::string(buffer, json.length());
}

struct Collector : hc::LogLineVisitor {
	bool line(const hc::LogLine& line) override {
		seqs.push_back(line.seq);
		messages.push_back(line.message);
		return true;
	}
	std::vector<uint32_t> seqs;
	std::vector<std::string> messages;
};

/// Restores the global threshold and removes the overrides a test set.
struct ScopedLevels {
	ScopedLevels() : previous(Log::level()) {}
	~ScopedLevels() {
		Log::clear_tag_levels();
		Log::set_level(previous);
	}
	LogLevel previous;
};

} // namespace

TEST_CASE("Diagnostics.ResetReasonNamesAreWords") {
	CHECK(std::string(hc::reset_reason_name(ResetReason::power_on)) == "power-on");
	CHECK(std::string(hc::reset_reason_name(ResetReason::software)) == "software restart");
	CHECK(std::string(hc::reset_reason_name(ResetReason::brownout)) == "brownout");
	CHECK(std::string(hc::reset_reason_name(ResetReason::task_watchdog)) == "task watchdog");
	CHECK(std::string(hc::reset_reason_name(ResetReason::interrupt_watchdog)) == "interrupt watchdog");
	CHECK(std::string(hc::reset_reason_name(ResetReason::panic)) == "panic");
	CHECK(std::string(hc::reset_reason_name(ResetReason::deep_sleep)) == "deep sleep wake-up");
	CHECK(std::string(hc::reset_reason_name(static_cast<ResetReason>(200))) == "unknown");
	CHECK(hc::reset_reason_is_crash(ResetReason::panic));
	CHECK(hc::reset_reason_is_crash(ResetReason::task_watchdog));
	CHECK_FALSE(hc::reset_reason_is_crash(ResetReason::brownout));
	CHECK_FALSE(hc::reset_reason_is_crash(ResetReason::software));
}

TEST_CASE("Diagnostics.ResetReasonNumbersAreFrozen") {
	// Stored in the device's boot record (NVS diag/reason): never renumber.
	CHECK(static_cast<int>(ResetReason::unknown) == 0);
	CHECK(static_cast<int>(ResetReason::power_on) == 1);
	CHECK(static_cast<int>(ResetReason::software) == 3);
	CHECK(static_cast<int>(ResetReason::panic) == 4);
	CHECK(static_cast<int>(ResetReason::task_watchdog) == 6);
	CHECK(static_cast<int>(ResetReason::brownout) == 9);
	CHECK(static_cast<int>(ResetReason::cpu_lockup) == 15);
}

TEST_CASE("Diagnostics.StatusWriterReportsReasonAndFacts") {
	FakeDiagnostics diagnostics;
	CHECK(boot_json(&diagnostics) ==
		"{\"boot\":{\"reason\":\"brownout\",\"count\":12,\"brownouts\":3,\"previous_uptime_s\":3600,"
		"\"coredump\":true,\"coredump_task\":\"hc_loop\"}}");
	diagnostics.facts = false;
	diagnostics.reason = ResetReason::power_on;
	CHECK(boot_json(&diagnostics) == "{\"boot\":{\"reason\":\"power-on\"}}");
	CHECK(boot_json(nullptr) == "{\"boot\":{\"reason\":\"unknown\"}}");
}

TEST_CASE("Diagnostics.FormatsFactsAsText") {
	FakeDiagnostics diagnostics;
	char buffer[160];
	size_t length = hc::format_boot_facts(diagnostics, buffer, sizeof(buffer));
	CHECK(std::string(buffer) ==
		"count 12, brownouts 3, previous_uptime_s 3600, coredump yes, coredump_task hc_loop");
	CHECK(length == std::string(buffer).size());

	char small[16];
	length = hc::format_boot_facts(diagnostics, small, sizeof(small));
	CHECK(length == sizeof(small) - 1);
	CHECK(std::string(small) == "count 12, brown");

	diagnostics.facts = false;
	CHECK(hc::format_boot_facts(diagnostics, buffer, sizeof(buffer)) == 0);
	CHECK(std::string(buffer).empty());
}

TEST_CASE("Diagnostics.ControllerLogsReasonInFirstLineAndFacts") {
	hc_test::FakePlatform p;
	FakeDiagnostics diagnostics;
	hc::Platform platform = p.platform();
	platform.diagnostics = &diagnostics;
	hc::Controller controller(platform);
	CHECK(controller.diagnostics() == &diagnostics);
	controller.start();
	REQUIRE(p.sink.lines.size() >= 2);
	// A brownout or a crash is a warning, so it stands out in the boot log.
	CHECK(p.sink.lines[0] ==
		std::string("W hc.ctl: HomeControl ") + hc::kVersion + " (build " + hc::build_id() + ") starting, reset reason: brownout");
	CHECK(p.sink.lines[1] ==
		"I hc.ctl: boot: count 12, brownouts 3, previous_uptime_s 3600, coredump yes, coredump_task hc_loop");
	// The ring (GET /api/log) has the same first line.
	Collector ring;
	controller.log_ring().read(0, ring);
	REQUIRE_FALSE(ring.messages.empty());
	CHECK(ring.messages[0].find("reset reason: brownout") != std::string::npos);
}

TEST_CASE("Diagnostics.ControllerLogsOrdinaryResetAsInfo") {
	hc_test::FakePlatform p;
	FakeDiagnostics diagnostics;
	diagnostics.reason = ResetReason::software;
	diagnostics.facts = false;
	hc::Platform platform = p.platform();
	platform.diagnostics = &diagnostics;
	hc::Controller controller(platform);
	controller.start();
	REQUIRE_FALSE(p.sink.lines.empty());
	CHECK(p.sink.lines[0].rfind("I hc.ctl: HomeControl ", 0) == 0);
	CHECK(p.sink.lines[0].find("reset reason: software restart") != std::string::npos);
	CHECK(p.sink.count("boot:") == 0); // no facts, no line
}

TEST_CASE("Diagnostics.LogLevelPerTagAtRunTime") {
	ScopedLevels restore;
	hc_test::ScopedCapture capture(LogLevel::info);
	Log::debug("hc.knx", "hidden");
	REQUIRE(Log::set_tag_level("hc.knx", LogLevel::debug));
	Log::debug("hc.knx", "visible");
	Log::debug("hc.web", "still hidden");
	REQUIRE(Log::set_tag_level("hc.web", LogLevel::error));
	Log::warn("hc.web", "suppressed warning");
	Log::error("hc.web", "error passes");
	CHECK(capture.sink.lines == std::vector<std::string>{"D hc.knx: visible", "E hc.web: error passes"});
	CHECK(Log::level("hc.knx") == LogLevel::debug);
	CHECK(Log::level("hc.other") == LogLevel::info);
	CHECK(Log::tag_level_count() == 2);
	const char* tag = nullptr;
	LogLevel level = LogLevel::info;
	REQUIRE(Log::tag_level_at(1, tag, level));
	CHECK(std::string(tag) == "hc.web");
	CHECK(level == LogLevel::error);
	CHECK_FALSE(Log::tag_level_at(2, tag, level));

	// Changing an override keeps its slot.
	REQUIRE(Log::set_tag_level("hc.knx", LogLevel::warn));
	CHECK(Log::tag_level_count() == 2);
	CHECK(Log::level("hc.knx") == LogLevel::warn);

	// "*" sets the global threshold and removes the overrides.
	REQUIRE(Log::set_tag_level("*", LogLevel::warn));
	CHECK(Log::level() == LogLevel::warn);
	CHECK(Log::tag_level_count() == 0);
	CHECK(Log::level("hc.knx") == LogLevel::warn);
	capture.sink.lines.clear();
	Log::info("hc.knx", "now hidden");
	CHECK(capture.sink.lines.empty());
}

TEST_CASE("Diagnostics.LogLevelRefusalsSpeak") {
	ScopedLevels restore;
	const char* error = nullptr;
	CHECK_FALSE(Log::set_tag_level("", LogLevel::debug, &error));
	CHECK(std::string(error) == "the tag is empty");
	CHECK_FALSE(Log::set_tag_level("hc.this-tag-is-far-too-long", LogLevel::debug, &error));
	CHECK(std::string(error).find("longer than 23") != std::string::npos);
	for (size_t i = 0; i < Log::kMaxTagLevels; ++i) {
		std::string tag = "hc.t" + std::to_string(i);
		REQUIRE(Log::set_tag_level(tag.c_str(), LogLevel::debug));
	}
	error = nullptr;
	CHECK_FALSE(Log::set_tag_level("hc.one-more", LogLevel::debug, &error));
	REQUIRE(error != nullptr);
	CHECK(std::string(error).find("log level *") != std::string::npos);
	CHECK(Log::set_tag_level("hc.t3", LogLevel::error)); // an existing one still changes
}

TEST_CASE("Diagnostics.LogLevelNamesParse") {
	LogLevel level = LogLevel::info;
	CHECK(hc::log_level_parse("debug", level));
	CHECK(level == LogLevel::debug);
	CHECK(hc::log_level_parse("WARNING", level));
	CHECK(level == LogLevel::warn);
	CHECK(hc::log_level_parse("e", level));
	CHECK(level == LogLevel::error);
	CHECK(hc::log_level_parse("Info", level));
	CHECK(level == LogLevel::info);
	CHECK_FALSE(hc::log_level_parse("verbose", level));
	CHECK_FALSE(hc::log_level_parse("", level));
	CHECK_FALSE(hc::log_level_parse(nullptr, level));
	CHECK(std::string(hc::log_level_name(LogLevel::warn)) == "warn");
}

TEST_CASE("Diagnostics.RingTailGivesTheNewestLines") {
	hc::LogRing ring;
	Collector none;
	CHECK(ring.read_tail(5, none) == 0);
	for (int i = 1; i <= 10; ++i) {
		std::string text = "line " + std::to_string(i);
		ring.append(LogLevel::info, "hc.t", text.c_str());
	}
	Collector tail;
	CHECK(ring.read_tail(3, tail) == 3);
	CHECK(tail.messages == std::vector<std::string>{"line 8", "line 9", "line 10"});
	Collector all;
	CHECK(ring.read_tail(100, all) == 10);
	CHECK(all.seqs.front() == 1);
	Collector zero;
	CHECK(ring.read_tail(0, zero) == 0);
}

TEST_CASE("Diagnostics.RingTailAfterWrap") {
	hc::LogRing ring;
	std::string text(100, 'x');
	for (int i = 0; i < 200; ++i) {
		std::string line = std::to_string(i) + text;
		ring.append(LogLevel::info, "hc.t", line.c_str());
	}
	Collector tail;
	CHECK(ring.read_tail(4, tail) == 4);
	CHECK(tail.seqs == std::vector<uint32_t>{197, 198, 199, 200});
	CHECK(tail.messages.back().rfind("199x", 0) == 0);
}
