/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_LOG_RING_H
#define HC_LOG_RING_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hc/log.h"
#include "hc/platform/clock.h"

namespace hc {

/// One line of the ring as LogRing::read() hands it out.
struct LogLine {
	uint32_t seq;       ///< 1, 2, 3, ... since boot (gaps: lines overwritten or dropped).
	uint64_t ms;        ///< Monotonic time of the line.
	LogLevel level;
	const char* tag;    ///< NUL-terminated.
	const char* message; ///< NUL-terminated.
};

/// Receives the lines of LogRing::read().
class LogLineVisitor {
public:
	/// Returns false to stop.
	virtual bool line(const LogLine& line) = 0;

protected:
	~LogLineVisitor() = default;
};

/// The last kCapacity bytes of what the core logged since boot (doc/web.md; served by
/// GET /api/log, extended by the diagnostics of issue #4). hc::Log mirrors every emitted line into
/// the ring installed with Log::set_ring() (the Controller installs its own); the oldest lines are
/// dropped to make room. Statically sized, no heap.
///
/// Threading: append() may run on any thread. A writer that finds the ring busy (another thread
/// appending, or a reader copying) drops its line and counts it (dropped()) instead of waiting, so
/// a low-priority task holding the ring can never block a higher-priority one. Readers take a
/// consistent copy with copy_to() and read the copy.
class LogRing {
public:
	static constexpr size_t kCapacity = 4096;
	/// Longest stored message (tag and text together); longer ones are cut.
	static constexpr size_t kMaxLine = 300;

	explicit LogRing(const Clock* clock = nullptr) : clock_(clock) {}

	void set_clock(const Clock* clock) { clock_ = clock; }

	void append(LogLevel level, const char* tag, const char* message);

	/// Copies the content into `out` (which must not be in use by a writer). False if the ring was
	/// busy; try again.
	bool copy_to(LogRing& out) const;

	/// Calls `visitor` for the lines with seq > `after`, oldest first. Returns the number visited.
	/// Not thread-safe: use it on a copy (or on the thread that alone appends).
	size_t read(uint32_t after, LogLineVisitor& visitor) const;

	/// Calls `visitor` for the newest `n` lines (all if fewer are held), oldest first: the console's
	/// `log tail [n]`. Returns the number visited. Not thread-safe, like read().
	size_t read_tail(size_t n, LogLineVisitor& visitor) const;

	/// The seq of the newest line (0 if none yet).
	uint32_t last_seq() const { return next_seq_ - 1; }

	/// The seq of the oldest line still held (0 if empty).
	uint32_t first_seq() const { return count_ == 0 ? 0 : first_seq_; }

	/// Lines dropped because the ring was busy.
	uint32_t dropped() const { return dropped_; }

	size_t count() const { return count_; }

private:
	struct Header {
		uint64_t ms;
		uint32_t seq;
		uint16_t length; ///< tag + NUL + message + NUL
		uint8_t level;
		uint8_t tag_length;
	};

	void write_bytes(size_t at, const void* data, size_t length);
	void read_bytes(size_t at, void* data, size_t length) const;
	void drop_oldest();

	const Clock* clock_;
	mutable std::atomic_flag busy_ = ATOMIC_FLAG_INIT;
	uint8_t data_[kCapacity] = {};
	size_t head_ = 0; ///< Offset of the oldest entry.
	size_t used_ = 0;
	size_t count_ = 0;
	uint32_t first_seq_ = 1;
	uint32_t next_seq_ = 1;
	uint32_t dropped_ = 0;
};

} // namespace hc

#endif // HC_LOG_RING_H
