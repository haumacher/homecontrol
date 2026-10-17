/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/log.h"

#include "hc/log_ring.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace hc {

namespace {

LogSink* g_sink = nullptr;
LogRing* g_ring = nullptr;
std::atomic<uint8_t> g_level{static_cast<uint8_t>(LogLevel::info)};

/// One tag override. A slot is filled completely before g_tag_count publishes it; a published
/// slot's tag never changes until clear_tag_levels() (only its level).
struct TagLevel {
	char tag[Log::kMaxTagLength + 1];
	std::atomic<uint8_t> level;
};

TagLevel g_tags[Log::kMaxTagLevels];
std::atomic<size_t> g_tag_count{0};

/// The published slot of `tag`, or nullptr.
TagLevel* find_tag(const char* tag) {
	size_t count = g_tag_count.load(std::memory_order_acquire);
	for (size_t i = 0; i < count; ++i) {
		if (std::strcmp(g_tags[i].tag, tag) == 0) {
			return &g_tags[i];
		}
	}
	return nullptr;
}

} // namespace

const char* log_level_name(LogLevel level) {
	switch (level) {
	case LogLevel::error:
		return "error";
	case LogLevel::warn:
		return "warn";
	case LogLevel::info:
		return "info";
	case LogLevel::debug:
		return "debug";
	}
	return "?";
}

bool log_level_parse(const char* text, LogLevel& out) {
	if (text == nullptr) {
		return false;
	}
	char lower[8];
	size_t length = std::strlen(text);
	if (length == 0 || length >= sizeof(lower)) {
		return false;
	}
	for (size_t i = 0; i <= length; ++i) {
		lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
	}
	static const struct {
		const char* name;
		const char* letter;
		LogLevel level;
	} kLevels[] = {
		{"error", "e", LogLevel::error},
		{"warn", "w", LogLevel::warn},
		{"info", "i", LogLevel::info},
		{"debug", "d", LogLevel::debug},
	};
	for (const auto& entry : kLevels) {
		if (std::strcmp(lower, entry.name) == 0 || std::strcmp(lower, entry.letter) == 0) {
			out = entry.level;
			return true;
		}
	}
	if (std::strcmp(lower, "warning") == 0) {
		out = LogLevel::warn;
		return true;
	}
	return false;
}

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
	g_level.store(static_cast<uint8_t>(threshold), std::memory_order_relaxed);
}

LogLevel Log::level() {
	return static_cast<LogLevel>(g_level.load(std::memory_order_relaxed));
}

bool Log::set_tag_level(const char* tag, LogLevel threshold, const char** error) {
	const char* reason = nullptr;
	size_t length = tag == nullptr ? 0 : std::strlen(tag);
	if (length == 0) {
		reason = "the tag is empty";
	} else if (std::strcmp(tag, "*") == 0) {
		clear_tag_levels();
		set_level(threshold);
		return true;
	} else if (length > kMaxTagLength) {
		reason = "the tag is longer than 23 characters";
	} else if (TagLevel* slot = find_tag(tag)) {
		slot->level.store(static_cast<uint8_t>(threshold), std::memory_order_relaxed);
		return true;
	} else {
		size_t count = g_tag_count.load(std::memory_order_relaxed);
		if (count >= kMaxTagLevels) {
			reason = "8 tags already have a level of their own (log level * <level> resets them)";
		} else {
			std::memcpy(g_tags[count].tag, tag, length + 1);
			g_tags[count].level.store(static_cast<uint8_t>(threshold), std::memory_order_relaxed);
			g_tag_count.store(count + 1, std::memory_order_release);
			return true;
		}
	}
	if (error != nullptr) {
		*error = reason;
	}
	return false;
}

LogLevel Log::level(const char* tag) {
	if (tag != nullptr) {
		if (const TagLevel* slot = find_tag(tag)) {
			return static_cast<LogLevel>(slot->level.load(std::memory_order_relaxed));
		}
	}
	return level();
}

void Log::clear_tag_levels() {
	g_tag_count.store(0, std::memory_order_release);
}

size_t Log::tag_level_count() {
	return g_tag_count.load(std::memory_order_acquire);
}

bool Log::tag_level_at(size_t index, const char*& tag, LogLevel& threshold) {
	if (index >= tag_level_count()) {
		return false;
	}
	tag = g_tags[index].tag;
	threshold = static_cast<LogLevel>(g_tags[index].level.load(std::memory_order_relaxed));
	return true;
}

bool Log::enabled(LogLevel level) {
	return g_sink != nullptr && static_cast<uint8_t>(level) <= g_level.load(std::memory_order_relaxed);
}

bool Log::enabled(LogLevel level, const char* tag) {
	return g_sink != nullptr && static_cast<uint8_t>(level) <= static_cast<uint8_t>(Log::level(tag));
}

void Log::vwrite(LogLevel level, const char* tag, const char* fmt, va_list args) {
	if (!enabled(level, tag)) {
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
