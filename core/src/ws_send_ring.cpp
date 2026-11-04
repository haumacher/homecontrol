/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#include "hc/ws_send_ring.h"

#include <cstddef>
#include <cstring>

namespace hc {

WsSendRing::WsSendRing(uint8_t* storage, size_t capacity)
	: storage_(storage), capacity_(capacity), lowest_free_(capacity) {}

char* WsSendRing::claim(size_t max, bool bulk) {
	if (claimed_ != 0 || max > kMaxMessage || capacity_ < 2 * cost(kMaxMessage) || (capacity_ & (capacity_ - 1)) != 0) {
		return nullptr;
	}
	uint32_t head = head_.load(std::memory_order_relaxed);
	uint32_t tail = tail_.load(std::memory_order_acquire);
	size_t free = capacity_ - (head - tail);
	if (free < lowest_free_) {
		lowest_free_ = free;
	}
	size_t offset = head & (capacity_ - 1);
	size_t contiguous = capacity_ - offset;
	size_t need = cost(max);
	// A message never straddles the end: the rest before it is padding.
	size_t pad = contiguous < need ? contiguous : 0;
	if (free < pad + need || (bulk && free - pad - need < kBulkReserve)) {
		// Refused because of the rest before the end (homecontrol #74): it would fit at the front once
		// the reader passed what is queued. Pad the rest now, so the reader frees it with the rest:
		// otherwise an empty ring with its head in the wrong place refuses every bulk claim for good,
		// and the feed's retry on writable() spins the loop.
		if (pad != 0 && free >= pad && free >= need && (!bulk || free - need >= kBulkReserve)) {
			write_pad(head, offset, contiguous);
		}
		return nullptr;
	}
	if (pad != 0) {
		head = write_pad(head, offset, contiguous);
		offset = 0;
	}
	claimed_ = 1;
	claimed_max_ = max;
	return reinterpret_cast<char*>(storage_ + offset + sizeof(Header));
}

uint32_t WsSendRing::write_pad(uint32_t head, size_t offset, size_t contiguous) {
	if (contiguous >= sizeof(Header)) {
		Header header = {};
		header.flags = kPad;
		std::memcpy(storage_ + offset, &header, sizeof(header));
	} // else: too short for a header; the reader skips such a rest by itself
	head += static_cast<uint32_t>(contiguous);
	head_.store(head, std::memory_order_release);
	return head;
}

void WsSendRing::commit(const uint32_t* clients, size_t count, size_t length, bool close) {
	if (claimed_ == 0) {
		return;
	}
	claimed_ = 0;
	if (count == 0 || length > claimed_max_) {
		return; // nobody to send to, or a length the claim did not hold: nothing is queued
	}
	if (count > kMaxRecipients) {
		count = kMaxRecipients;
	}
	uint32_t head = head_.load(std::memory_order_relaxed);
	Header header = {};
	header.length = static_cast<uint16_t>(length);
	header.flags = close ? kClose : 0;
	header.count = static_cast<uint8_t>(count);
	for (size_t i = 0; i < count; ++i) {
		header.clients[i] = clients[i];
	}
	std::memcpy(storage_ + (head & (capacity_ - 1)), &header, sizeof(header));
	head_.store(head + static_cast<uint32_t>(cost(length)), std::memory_order_release);
}

bool WsSendRing::push(const uint32_t* clients, size_t count, const char* data, size_t length, bool close, bool bulk) {
	char* room = claim(length, bulk);
	if (room == nullptr) {
		return false;
	}
	if (length > 0) {
		std::memcpy(room, data, length);
	}
	commit(clients, count, length, close);
	return true;
}

bool WsSendRing::front(Entry& entry) {
	uint32_t tail = tail_.load(std::memory_order_relaxed);
	uint32_t head = head_.load(std::memory_order_acquire);
	while (tail != head) {
		size_t offset = tail & (capacity_ - 1);
		size_t contiguous = capacity_ - offset;
		Header header;
		if (contiguous >= sizeof(Header)) {
			std::memcpy(&header, storage_ + offset, sizeof(header));
		}
		if (contiguous < sizeof(Header) || (header.flags & kPad) != 0) {
			tail += static_cast<uint32_t>(contiguous);
			tail_.store(tail, std::memory_order_release);
			continue;
		}
		entry.clients = reinterpret_cast<const uint32_t*>(storage_ + offset + offsetof(Header, clients));
		entry.count = header.count;
		entry.close = (header.flags & kClose) != 0;
		entry.data = reinterpret_cast<const char*>(storage_ + offset + sizeof(Header));
		entry.length = header.length;
		front_cost_ = cost(header.length);
		return true;
	}
	return false;
}

void WsSendRing::pop() {
	if (front_cost_ == 0) {
		return;
	}
	uint32_t tail = tail_.load(std::memory_order_relaxed);
	tail_.store(tail + static_cast<uint32_t>(front_cost_), std::memory_order_release);
	front_cost_ = 0;
}

} // namespace hc
