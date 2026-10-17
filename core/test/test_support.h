/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_TEST_SUPPORT_H
#define HC_TEST_SUPPORT_H

#include <cstdint>
#include <string>
#include <vector>

#include "hc/log.h"
#include "hc/platform/clock.h"
#include "hc/platform/system_info.h"

namespace hc_test {

/// A clock the test advances by hand. Never reads the real time.
class FakeClock : public hc::Clock {
public:
	uint64_t monotonic_ms() const override { return now_ms; }
	hc::WallTime wall_time() const override { return hc::WallTime{wall_seconds, synchronized}; }

	void advance(uint64_t ms) { now_ms += ms; }

	uint64_t now_ms = 1000;
	int64_t wall_seconds = 0;
	bool synchronized = false;
};

/// Fixed memory figures.
class FakeSystemInfo : public hc::SystemInfo {
public:
	hc::MemoryInfo memory() const override { return hc::MemoryInfo{free_bytes, min_free_bytes}; }

	uint32_t free_bytes = 300000;
	uint32_t min_free_bytes = 250000;
};

/// Records every message as "<letter> <tag>: <message>".
class CapturingSink : public hc::LogSink {
public:
	void write(hc::LogLevel level, const char* tag, const char* message) override {
		lines.push_back(std::string(hc::log_level_letter(level)) + " " + tag + ": " + message);
	}

	/// Number of recorded lines containing `text`.
	size_t count(const std::string& text) const {
		size_t result = 0;
		for (const std::string& line : lines) {
			if (line.find(text) != std::string::npos) {
				++result;
			}
		}
		return result;
	}

	bool contains(const std::string& text) const { return count(text) > 0; }

	std::vector<std::string> lines;
};

/// Installs a CapturingSink for the scope of a test and restores the previous sink and level.
class ScopedCapture {
public:
	explicit ScopedCapture(hc::LogLevel level = hc::LogLevel::info)
		: previous_sink_(hc::Log::sink()), previous_level_(hc::Log::level()) {
		hc::Log::set_sink(&sink);
		hc::Log::set_level(level);
	}
	~ScopedCapture() {
		hc::Log::set_sink(previous_sink_);
		hc::Log::set_level(previous_level_);
	}

	CapturingSink sink;

private:
	hc::LogSink* previous_sink_;
	hc::LogLevel previous_level_;
};

} // namespace hc_test

#endif // HC_TEST_SUPPORT_H
