/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/output_ring.h"

#include <cstring>

namespace hc {

OutputRing::OutputRing(uint8_t* storage, size_t capacity, bool crlf)
	: storage_(storage), capacity_(capacity), crlf_(crlf) {}

size_t OutputRing::text_length(const char* data, size_t length) const {
	if (!crlf_) {
		return length;
	}
	size_t result = length;
	for (size_t i = 0; i < length; ++i) {
		if (data[i] == '\n') {
			++result;
		}
	}
	return result;
}

namespace {

constexpr char kMarkerText[] = "\xE2\x80\xA6 %u line(s) dropped (console output could not keep up)";
/// The marker's fixed bytes: the text without "%u" and the terminating NUL.
constexpr size_t kMarkerFixed = sizeof(kMarkerText) - 1 - 2;

size_t digits(uint32_t value) {
	size_t n = 1;
	while (value >= 10) {
		value /= 10;
		++n;
	}
	return n;
}

} // namespace

size_t OutputRing::marker_length() const {
	if (unreported_ == 0) {
		return 0;
	}
	size_t eol = crlf_ ? 2 : 1;
	return (at_line_start_ ? 0 : eol) + kMarkerFixed + digits(unreported_) + eol;
}

size_t OutputRing::format_marker(char* out) const {
	if (unreported_ == 0) {
		return 0;
	}
	// By hand, not snprintf: this runs in out_write() on whichever task logs, inside its own printf
	// and a critical section; a nested printf of the ROM needs another 1.25 KB of that task's stack,
	// which once overflowed the IDF's sys_evt and the console writer (#48).
	size_t n = 0;
	auto add = [&](const char* text, size_t length) {
		for (size_t i = 0; i < length && n + 1 < kMaxMarker; ++i) {
			out[n++] = text[i];
		}
	};
	const char* eol = crlf_ ? "\r\n" : "\n";
	size_t eol_length = crlf_ ? 2 : 1;
	if (!at_line_start_) {
		add(eol, eol_length);
	}
	const char* hole = std::strstr(kMarkerText, "%u");
	add(kMarkerText, static_cast<size_t>(hole - kMarkerText));
	char number[10];
	size_t count = 0;
	uint32_t value = unreported_;
	do {
		number[count++] = static_cast<char>('0' + value % 10);
		value /= 10;
	} while (value != 0);
	while (count > 0) {
		add(&number[--count], 1);
	}
	add(hole + 2, std::strlen(hole + 2));
	add(eol, eol_length);
	out[n] = '\0';
	return n;
}

void OutputRing::put(uint8_t byte) {
	size_t tail = head_ + used_;
	if (tail >= capacity_) {
		tail -= capacity_;
	}
	storage_[tail] = byte;
	++used_;
}

void OutputRing::put_text(const char* data, size_t length) {
	for (size_t i = 0; i < length; ++i) {
		if (crlf_ && data[i] == '\n') {
			put('\r');
		}
		put(static_cast<uint8_t>(data[i]));
	}
}

void OutputRing::put_marker() {
	if (unreported_ == 0) {
		return;
	}
	char marker[kMaxMarker];
	size_t length = format_marker(marker);
	for (size_t i = 0; i < length; ++i) {
		put(static_cast<uint8_t>(marker[i]));
	}
	unreported_ = 0;
	at_line_start_ = true;
}

uint32_t OutputRing::append_segment(const char* data, size_t length, bool line_end) {
	if (discarding_) {
		// The rest of a line whose earlier part was dropped (and counted then).
		if (line_end) {
			discarding_ = false;
		}
		return 0;
	}
	size_t needed = text_length(data, length);
	if (marker_length() + needed <= free_space()) {
		put_marker();
		put_text(data, length);
		at_line_start_ = line_end;
		return 0;
	}
	++dropped_;
	++unreported_;
	discarding_ = !line_end;
	return 1;
}

uint32_t OutputRing::append_text(const char* data, size_t length) {
	uint32_t dropped = 0;
	size_t start = 0;
	while (start < length) {
		const void* nl = std::memchr(data + start, '\n', length - start);
		size_t end = nl != nullptr ? static_cast<size_t>(static_cast<const char*>(nl) - data) + 1 : length;
		dropped += append_segment(data + start, end - start, nl != nullptr);
		start = end;
	}
	return dropped;
}

uint32_t OutputRing::append_record(const uint8_t* data, size_t length) {
	if (length == 0) {
		return 0;
	}
	if (marker_length() + length > free_space()) {
		++dropped_;
		++unreported_;
		return 1;
	}
	put_marker();
	for (size_t i = 0; i < length; ++i) {
		put(data[i]);
	}
	at_line_start_ = data[length - 1] == '\n';
	return 0;
}

size_t OutputRing::peek(const uint8_t** data) {
	if (used_ == 0 && unreported_ > 0 && marker_length() <= capacity_) {
		put_marker();
	}
	*data = storage_ + head_;
	size_t run = capacity_ - head_;
	return used_ < run ? used_ : run;
}

void OutputRing::consume(size_t length) {
	if (length > used_) {
		length = used_;
	}
	head_ += length;
	if (head_ >= capacity_) {
		head_ -= capacity_;
	}
	used_ -= length;
	if (used_ == 0) {
		head_ = 0; // the next run starts at the front: fewer wrapped peeks
	}
}

} // namespace hc
