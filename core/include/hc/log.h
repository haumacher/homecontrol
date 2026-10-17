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

/// Log severity, most severe first. A message is emitted if its level is at most the threshold.
enum class LogLevel : uint8_t {
	error = 0,
	warn = 1,
	info = 2,
	debug = 3,
};

/// One-letter abbreviation of a level ("E", "W", "I", "D").
const char* log_level_letter(LogLevel level);

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
/// Thread safety: formatting is reentrant; whether concurrent writes interleave cleanly is up to
/// the sink. set_sink()/set_level() are meant for start-up, not for concurrent use.
class Log {
public:
	/// Size of the formatting buffer including the terminating NUL.
	static constexpr size_t kMaxMessage = 256;

	static void set_sink(LogSink* sink);
	static LogSink* sink();

	static void set_level(LogLevel threshold);
	static LogLevel level();
	static bool enabled(LogLevel level);

	static void write(LogLevel level, const char* tag, const char* fmt, ...) HC_PRINTF_FORMAT(3, 4);
	static void vwrite(LogLevel level, const char* tag, const char* fmt, va_list args);

	static void error(const char* tag, const char* fmt, ...) HC_PRINTF_FORMAT(2, 3);
	static void warn(const char* tag, const char* fmt, ...) HC_PRINTF_FORMAT(2, 3);
	static void info(const char* tag, const char* fmt, ...) HC_PRINTF_FORMAT(2, 3);
	static void debug(const char* tag, const char* fmt, ...) HC_PRINTF_FORMAT(2, 3);
};

} // namespace hc

#endif // HC_LOG_H
