/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_LOG_H
#define HC_LOG_H

#include <cstdarg>
#include <cstddef>
#include <cstdint>

#if defined(__GNUC__)
#define HC_PRINTF_FORMAT(fmt_index, args_index) __attribute__((format(printf, fmt_index, args_index)))
#else
#define HC_PRINTF_FORMAT(fmt_index, args_index)
#endif

namespace hc {

class LogRing;

/// Log severity, most severe first. A message is emitted if its level is at most the threshold.
enum class LogLevel : uint8_t {
	error = 0,
	warn = 1,
	info = 2,
	debug = 3,
};

/// One-letter abbreviation of a level ("E", "W", "I", "D").
const char* log_level_letter(LogLevel level);

/// The level's name ("error", "warn", "info", "debug").
const char* log_level_name(LogLevel level);

/// Parses a level name ("error", "warn", "info", "debug"; also "warning" and the one-letter
/// abbreviations, any case). False if `text` names no level.
bool log_level_parse(const char* text, LogLevel& out);

/// Where formatted log messages go. The platform implements it (stderr on POSIX, ESP_LOG* on the
/// device) and installs it with Log::set_sink().
class LogSink {
public:
	/// Receives one complete, NUL-terminated message without a trailing newline. `tag` names the
	/// emitting subsystem (for example "hc.loop"). Called on the thread that logs.
	virtual void write(LogLevel level, const char* tag, const char* message) = 0;

protected:
	~LogSink() = default;
};

/// The logging front end everything in the core uses. Formats printf-style into a fixed stack
/// buffer of kMaxMessage bytes (longer messages are truncated and end in "..."), never touches
/// the heap. Messages logged while no sink is installed are discarded: the platform installs its
/// sink before anything else runs (the Controller does it in its constructor).
///
/// The threshold is global (set_level()) with up to kMaxTagLevels overrides for single tags
/// (set_tag_level(), the console's `log level <tag> <level>`): a message is emitted if its level is
/// at most its tag's override, or the global threshold for a tag without one.
///
/// Thread safety: formatting is reentrant; whether concurrent writes interleave cleanly is up to
/// the sink. set_sink() is meant for start-up. The levels may change at run time from one thread
/// (the event loop): readers on other threads see either the old or the new level of a tag.
class Log {
public:
	/// Size of the formatting buffer including the terminating NUL.
	static constexpr size_t kMaxMessage = 256;

	static void set_sink(LogSink* sink);
	static LogSink* sink();

	/// Installs the ring every emitted line is mirrored into (nullptr: none). hc::LogRing.
	static void set_ring(LogRing* ring);
	static LogRing* ring();

	/// Longest tag an override can name (characters, without the NUL).
	static constexpr size_t kMaxTagLength = 23;
	/// Number of tags with an override of their own.
	static constexpr size_t kMaxTagLevels = 8;

	/// The global threshold (the overrides stay).
	static void set_level(LogLevel threshold);
	static LogLevel level();

	/// Sets the threshold of one tag; "*" sets the global threshold and removes every override
	/// (as esp_log_level_set("*") does on the device). Refused (false, `error` names the reason) for
	/// an empty or too long tag, or when kMaxTagLevels other tags already have an override.
	static bool set_tag_level(const char* tag, LogLevel threshold, const char** error = nullptr);
	/// The threshold in effect for `tag` (its override or the global threshold).
	static LogLevel level(const char* tag);
	/// Removes every override.
	static void clear_tag_levels();
	/// Number of overrides, and the i-th of them (false past the end).
	static size_t tag_level_count();
	static bool tag_level_at(size_t index, const char*& tag, LogLevel& threshold);

	/// Whether a message of `level` would be emitted with the global threshold, resp. for `tag`.
	static bool enabled(LogLevel level);
	static bool enabled(LogLevel level, const char* tag);

	static void write(LogLevel level, const char* tag, const char* fmt, ...) HC_PRINTF_FORMAT(3, 4);
	static void vwrite(LogLevel level, const char* tag, const char* fmt, va_list args);

	static void error(const char* tag, const char* fmt, ...) HC_PRINTF_FORMAT(2, 3);
	static void warn(const char* tag, const char* fmt, ...) HC_PRINTF_FORMAT(2, 3);
	static void info(const char* tag, const char* fmt, ...) HC_PRINTF_FORMAT(2, 3);
	static void debug(const char* tag, const char* fmt, ...) HC_PRINTF_FORMAT(2, 3);
};

} // namespace hc

#endif // HC_LOG_H
