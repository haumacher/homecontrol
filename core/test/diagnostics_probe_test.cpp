/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Probe tests written at review time for the diagnostics package (issue #4): per-tag log levels
// composed with the ring, the tail of the ring, and the boot status of a crashed previous run.

#include <string>
#include <vector>

#include "doctest.h"
#include "hc/diagnostics.h"
#include "hc/json.h"
#include "hc/log.h"
#include "hc/log_ring.h"
#include "hc/platform/diagnostics.h"
#include "test_support.h"

using hc::LogLevel;
using hc::ResetReason;

namespace {

struct Crashed : hc::Diagnostics {
	ResetReason reset_reason() const override { return ResetReason::task_watchdog; }
	void visit_boot(hc::DiagnosticsVisitor& v) const override {
		v.number("count", 57);
		v.number("brownouts", 4);
		v.number("previous_uptime_s", 1260);
		v.flag("coredump", true);
	}
	size_t coredump_size() const override { return 4096; }
};

struct Tail : hc::LogLineVisitor {
	std::vector<uint32_t> seqs;
	bool line(const hc::LogLine& l) override {
		seqs.push_back(l.seq);
		return true;
	}
};

} // namespace

TEST_CASE("DiagnosticsProbe.PerTagLevelsReachTheRingAndTheTailIsTheNewest") {
	hc_test::ScopedCapture capture(LogLevel::info);
	hc_test::FakeClock clock;
	hc::LogRing ring(&clock);
	hc::LogRing* previous = hc::Log::ring();
	hc::Log::set_ring(&ring);
	hc::Log::clear_tag_levels();
	const char* error = nullptr;
	// Debug of one tag at run time, the rest stays at info.
	REQUIRE(hc::Log::set_tag_level("hc.knx", LogLevel::debug, &error));
	hc::Log::debug("hc.knx", "frame %d", 1);
	hc::Log::debug("hc.web", "hidden %d", 2);
	hc::Log::info("hc.web", "shown %d", 3);
	CHECK(capture.sink.count("frame 1") == 1);
	CHECK(capture.sink.count("hidden 2") == 0);
	CHECK(ring.count() == 2);
	CHECK(hc::Log::level("hc.knx") == LogLevel::debug);
	CHECK(hc::Log::level("hc.web") == LogLevel::info);
	// Tag table exhaustion is a refusal, not a crash.
	size_t before = hc::Log::tag_level_count();
	bool refused = false;
	for (int i = 0; i < 64 && !refused; ++i) {
		std::string tag = "hc.t" + std::to_string(i);
		refused = !hc::Log::set_tag_level(tag.c_str(), LogLevel::warn, &error);
	}
	CHECK(refused);
	CHECK(error != nullptr);
	CHECK(hc::Log::tag_level_count() > before);
	hc::Log::clear_tag_levels();
	CHECK(hc::Log::tag_level_count() == 0);
	CHECK(hc::Log::level("hc.knx") == LogLevel::info);
	// The tail: the newest n lines in order; more than stored gives all.
	for (int i = 0; i < 10; ++i) {
		hc::Log::warn("hc.t", "line %d", i);
	}
	Tail tail;
	CHECK(ring.read_tail(3, tail) == 3);
	REQUIRE(tail.seqs.size() == 3);
	CHECK(tail.seqs[2] == ring.last_seq());
	CHECK(tail.seqs[0] == ring.last_seq() - 2);
	Tail all;
	CHECK(ring.read_tail(1000, all) == ring.count());
	hc::Log::set_ring(previous);
}

TEST_CASE("DiagnosticsProbe.BootStatusOfACrashedRunIsCompleteJson") {
	Crashed diag;
	char buffer[512];
	hc::JsonWriter json(buffer, sizeof(buffer));
	json.begin_object();
	hc::write_boot_status(json, &diag);
	json.end_object();
	REQUIRE_FALSE(json.overflowed());
	std::string text(json.data(), json.length());
	hc::JsonSpan root;
	REQUIRE(hc::json_parse(text.c_str(), text.size(), root));
	hc::JsonSpan boot, reason, count, dump;
	REQUIRE(hc::json_member(root, "boot", boot));
	REQUIRE(hc::json_member(boot, "reason", reason));
	CHECK(hc::json_string_equals(reason, hc::reset_reason_name(ResetReason::task_watchdog)));
	REQUIRE(hc::json_member(boot, "count", count));
	double n = 0;
	CHECK(hc::json_number(count, n));
	CHECK(n == 57);
	REQUIRE(hc::json_member(boot, "coredump", dump));
	bool flag = false;
	CHECK(hc::json_bool(dump, flag));
	CHECK(flag);
	CHECK(hc::reset_reason_is_crash(ResetReason::task_watchdog));
	CHECK_FALSE(hc::reset_reason_is_crash(ResetReason::software));
	// Without a platform, the member is still well-formed.
	hc::JsonWriter none(buffer, sizeof(buffer));
	none.begin_object();
	hc::write_boot_status(none, nullptr);
	none.end_object();
	CHECK_FALSE(none.overflowed());
	std::string t2(none.data(), none.length());
	CHECK(hc::json_parse(t2.c_str(), t2.size(), root));
}
