/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/log.h"

#include "hc/log_ring.h"

#include <cstdio>
#include <cstring>

namespace hc {

namespace {

LogSink* g_sink = nullptr;
LogRing* g_ring = nullptr;
LogLevel g_level = LogLevel::info;

} // namespace

const char* log_level_letter(LogLevel level) {
	switch (level) {
	case LogLevel::error:
		return "E";
	case LogLevel::warn:
		return "W";
	case LogLevel::info:
		return "I";
	case LogLevel::debug:
		return "D";
	}
	return "?";
}

void Log::set_sink(LogSink* sink) {
	g_sink = sink;
}

LogSink* Log::sink() {
	return g_sink;
}

void Log::set_ring(LogRing* ring) {
	g_ring = ring;
}

LogRing* Log::ring() {
	return g_ring;
}

void Log::set_level(LogLevel threshold) {
	g_level = threshold;
}

LogLevel Log::level() {
	return g_level;
}

bool Log::enabled(LogLevel level) {
	return g_sink != nullptr && static_cast<uint8_t>(level) <= static_cast<uint8_t>(g_level);
}

void Log::vwrite(LogLevel level, const char* tag, const char* fmt, va_list args) {
	if (!enabled(level)) {
		return;
	}
	char buffer[kMaxMessage];
	int length = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
	if (length < 0) {
		std::snprintf(buffer, sizeof(buffer), "<log format error: %s>", fmt);
	} else if (static_cast<size_t>(length) >= sizeof(buffer)) {
		// Truncated: mark it visibly.
		std::memcpy(buffer + sizeof(buffer) - 4, "...", 4);
	}
	g_sink->write(level, tag, buffer);
	if (g_ring != nullptr) {
		g_ring->append(level, tag, buffer);
	}
}

void Log::write(LogLevel level, const char* tag, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vwrite(level, tag, fmt, args);
	va_end(args);
}

#define HC_LOG_LEVEL_FUNCTION(name, lvl) \
	void Log::name(const char* tag, const char* fmt, ...) { \
		va_list args; \
		va_start(args, fmt); \
		vwrite(lvl, tag, fmt, args); \
		va_end(args); \
	}

HC_LOG_LEVEL_FUNCTION(error, LogLevel::error)
HC_LOG_LEVEL_FUNCTION(warn, LogLevel::warn)
HC_LOG_LEVEL_FUNCTION(info, LogLevel::info)
HC_LOG_LEVEL_FUNCTION(debug, LogLevel::debug)

#undef HC_LOG_LEVEL_FUNCTION

} // namespace hc
