/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include <string>

#include "doctest.h"
#include "hc/log.h"
#include "test_support.h"

using hc::Log;
using hc::LogLevel;
using hc_test::ScopedCapture;

TEST_CASE("Log.FormatsPrintfStyleWithLevelAndTag") {
	ScopedCapture capture;
	Log::info("hc.test", "value %d of %s", 42, "answer");
	Log::error("hc.test", "broken %u", 7u);
	REQUIRE(capture.sink.lines.size() == 2);
	CHECK(capture.sink.lines[0] == "I hc.test: value 42 of answer");
	CHECK(capture.sink.lines[1] == "E hc.test: broken 7");
}

TEST_CASE("Log.ThresholdFiltersLessSevereLevels") {
	ScopedCapture capture(LogLevel::warn);
	Log::debug("hc.test", "d");
	Log::info("hc.test", "i");
	Log::warn("hc.test", "w");
	Log::error("hc.test", "e");
	REQUIRE(capture.sink.lines.size() == 2);
	CHECK(capture.sink.lines[0] == "W hc.test: w");
	CHECK(capture.sink.lines[1] == "E hc.test: e");

	Log::set_level(LogLevel::debug);
	Log::debug("hc.test", "now visible");
	CHECK(capture.sink.contains("D hc.test: now visible"));
}

TEST_CASE("Log.TruncatesLongMessagesVisibly") {
	ScopedCapture capture;
	std::string long_text(2 * Log::kMaxMessage, 'x');
	Log::info("hc.test", "%s", long_text.c_str());
	REQUIRE(capture.sink.lines.size() == 1);
	const std::string& line = capture.sink.lines[0];
	std::string message = line.substr(std::string("I hc.test: ").size());
	CHECK(message.size() == Log::kMaxMessage - 1);
	CHECK(message.substr(message.size() - 3) == "...");
}

TEST_CASE("Log.WithoutSinkDiscards") {
	ScopedCapture capture;
	Log::set_sink(nullptr);
	CHECK_FALSE(Log::enabled(LogLevel::error));
	Log::error("hc.test", "nobody hears this"); // Must not crash.
	CHECK(capture.sink.lines.empty());
}
