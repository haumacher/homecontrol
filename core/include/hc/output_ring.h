/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_OUTPUT_RING_H
#define HC_OUTPUT_RING_H

#include <cstddef>
#include <cstdint>

namespace hc {

/// The queue between the tasks that print and the one task that writes a console port (issue #46):
/// a byte ring over storage the owner provides, with an append that never waits. The device's
/// console output (log lines, command output, the line editor, Improv frames) goes through one
/// such ring to a writer task, so that no task blocks on a slow or absent reader.
///
/// What does not fit is dropped line by line and counted, never cut in the middle of a line: a
/// text line either goes in whole (with the CRLF translation when enabled) or not at all, and once
/// part of a line was dropped the rest of that line is dropped too (a line's head may have gone in,
/// its tail never comes without the head). A record (append_record(), an Improv frame) goes in
/// whole or is dropped as one line. The next output that finds room is preceded, once, by the
/// marker line "… N line(s) dropped (console output could not keep up)"; when nothing follows, the
/// consumer receives the marker as soon as the ring has run empty (peek()).
///
/// Not synchronized: the owner serializes every call (the device holds a short critical section).
/// No allocation; an append costs a copy of its bytes.
class OutputRing {
public:
	/// `storage` of `capacity` bytes outlives the ring. `crlf`: text "\n" is written as "\r\n".
	OutputRing(uint8_t* storage, size_t capacity, bool crlf);

	/// Appends text (any number of lines, the last one may be partial: its rest follows in a
	/// later call). Returns the number of lines dropped by this call (0: all went in).
	uint32_t append_text(const char* data, size_t length);

	/// Appends a record as it is (binary, no translation): whole or not at all. Returns 1 when it was
	/// dropped, 0 when it went in.
	uint32_t append_record(const uint8_t* data, size_t length);

	/// The oldest queued bytes as one contiguous run (up to the wrap): sets `data` and returns the
	/// length; 0 when empty. When the ring has run empty with drops not yet reported, the marker
	/// line is queued first and returned. The run stays valid until consume().
	size_t peek(const uint8_t** data);

	/// Removes `length` bytes (at most what peek() returned) from the front.
	void consume(size_t length);

	size_t queued() const { return used_; }
	size_t capacity() const { return capacity_; }

	/// Lines dropped since start, and those not yet reported by a marker line.
	uint64_t dropped() const { return dropped_; }
	uint32_t unreported() const { return unreported_; }

	/// The longest marker line (with a leading line break and the CRLF ending).
	static constexpr size_t kMaxMarker = 80;

private:
	size_t free_space() const { return capacity_ - used_; }
	size_t text_length(const char* data, size_t length) const;
	/// The marker's length for `unreported_` (0: nothing to report), without formatting it.
	size_t marker_length() const;
	/// Formats the marker for `unreported_` into `out` (kMaxMarker bytes); returns its length.
	size_t format_marker(char* out) const;
	void put(uint8_t byte);
	void put_text(const char* data, size_t length);
	/// Queues the marker if drops are unreported; the caller made sure it fits (marker_length()).
	void put_marker();
	/// One segment of append_text(): a whole line with its "\n", or a trailing partial line.
	uint32_t append_segment(const char* data, size_t length, bool line_end);

	uint8_t* storage_;
	size_t capacity_;
	bool crlf_;
	size_t head_ = 0; ///< The oldest queued byte.
	size_t used_ = 0;
	/// The last byte queued ended a line (a marker then needs no line break of its own).
	bool at_line_start_ = true;
	/// Part of the current line was dropped: its rest is dropped too, up to its "\n".
	bool discarding_ = false;
	uint64_t dropped_ = 0;
	uint32_t unreported_ = 0;
};

} // namespace hc

#endif // HC_OUTPUT_RING_H
