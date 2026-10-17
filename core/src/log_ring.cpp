/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/log_ring.h"

#include <cstring>

namespace hc {

void LogRing::write_bytes(size_t at, const void* data, size_t length) {
	const uint8_t* p = static_cast<const uint8_t*>(data);
	at %= kCapacity;
	size_t first = kCapacity - at < length ? kCapacity - at : length;
	std::memcpy(data_ + at, p, first);
	std::memcpy(data_, p + first, length - first);
}

void LogRing::read_bytes(size_t at, void* data, size_t length) const {
	uint8_t* p = static_cast<uint8_t*>(data);
	at %= kCapacity;
	size_t first = kCapacity - at < length ? kCapacity - at : length;
	std::memcpy(p, data_ + at, first);
	std::memcpy(p + first, data_, length - first);
}

void LogRing::drop_oldest() {
	Header header;
	read_bytes(head_, &header, sizeof(header));
	size_t size = sizeof(header) + header.length;
	head_ = (head_ + size) % kCapacity;
	used_ -= size;
	--count_;
	first_seq_ = header.seq + 1;
}

void LogRing::append(LogLevel level, const char* tag, const char* message) {
	if (busy_.test_and_set(std::memory_order_acquire)) {
		++dropped_; // racy count: a diagnostic, not an invariant
		return;
	}
	size_t tag_length = tag != nullptr ? std::strlen(tag) : 0;
	if (tag_length > 32) {
		tag_length = 32;
	}
	size_t message_length = message != nullptr ? std::strlen(message) : 0;
	if (tag_length + message_length + 2 > kMaxLine) {
		message_length = kMaxLine - tag_length - 2;
	}
	Header header;
	header.ms = clock_ != nullptr ? clock_->monotonic_ms() : 0;
	header.seq = next_seq_++;
	header.length = static_cast<uint16_t>(tag_length + message_length + 2);
	header.level = static_cast<uint8_t>(level);
	header.tag_length = static_cast<uint8_t>(tag_length);
	size_t size = sizeof(header) + header.length;
	while (kCapacity - used_ < size && count_ > 0) {
		drop_oldest();
	}
	if (count_ == 0) {
		head_ = 0;
		first_seq_ = header.seq;
	}
	size_t at = (head_ + used_) % kCapacity;
	write_bytes(at, &header, sizeof(header));
	at += sizeof(header);
	write_bytes(at, tag != nullptr ? tag : "", tag_length);
	at += tag_length;
	const uint8_t zero = 0;
	write_bytes(at, &zero, 1);
	at += 1;
	write_bytes(at, message != nullptr ? message : "", message_length);
	at += message_length;
	write_bytes(at, &zero, 1);
	used_ += size;
	++count_;
	busy_.clear(std::memory_order_release);
}

bool LogRing::copy_to(LogRing& out) const {
	if (busy_.test_and_set(std::memory_order_acquire)) {
		return false;
	}
	std::memcpy(out.data_, data_, kCapacity);
	out.head_ = head_;
	out.used_ = used_;
	out.count_ = count_;
	out.first_seq_ = first_seq_;
	out.next_seq_ = next_seq_;
	out.dropped_ = dropped_;
	busy_.clear(std::memory_order_release);
	return true;
}

size_t LogRing::read(uint32_t after, LogLineVisitor& visitor) const {
	size_t visited = 0;
	size_t at = head_;
	char text[kMaxLine];
	for (size_t i = 0; i < count_; ++i) {
		Header header;
		read_bytes(at, &header, sizeof(header));
		size_t next = at + sizeof(header) + header.length;
		if (header.seq > after) {
			read_bytes(at + sizeof(header), text, header.length);
			LogLine line{header.seq, header.ms, static_cast<LogLevel>(header.level), text,
				text + header.tag_length + 1};
			++visited;
			if (!visitor.line(line)) {
				break;
			}
		}
		at = next % kCapacity;
	}
	return visited;
}

} // namespace hc
