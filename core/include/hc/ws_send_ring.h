/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_WS_SEND_RING_H
#define HC_WS_SEND_RING_H

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace hc {

/// The send queue of the WebSocket server (doc/web.md "ESP specifics", issue #45 item 7): one ring
/// of bytes holding whole messages of variable length, each with the clients it goes to. One writer
/// (the event loop: the live feed formats a message straight into a claim, or a copy is pushed) and
/// one reader (the server's sending task), without a lock.
///
/// - A message never straddles the ring's end: a claim that does not fit before the end pads the
///   rest and starts at the front, so the reader sends every message as one piece from the ring.
/// - A message is all or nothing: a claim is visible to the reader only once committed, and a
///   claim or push that does not fit fails without queuing anything the reader sends. One that
///   failed only because the rest before the end is too short pads that rest, which the reader
///   skips: the next claim after a pass of the reader starts at the front (homecontrol #74; an
///   empty ring would otherwise refuse a bulk message for good).
/// - One message to several clients (the status, a batch of updates) takes one entry, not one copy
///   per client.
/// - A bulk message (a snapshot chunk, a monitor batch: streams the feed resumes by itself) is
///   refused while it would leave less than kBulkReserve free, so the single messages (a command's
///   result, the updates) still find room beside a running snapshot.
///
/// The storage is the caller's (static on the device): its capacity is a power of two, at least
/// two entries of the largest message, aligned to 4.
class WsSendRing {
public:
	/// Clients one entry can name (the live feed's browser clients).
	static constexpr size_t kMaxRecipients = 4;
	/// The largest message (LiveFeed::kMessageSize).
	static constexpr size_t kMaxMessage = 1400;

	struct Header {
		uint16_t length;
		uint8_t flags;
		uint8_t count;
		uint32_t clients[kMaxRecipients];
	};
	/// Room one message of `length` bytes takes in the ring, its header included.
	static constexpr size_t cost(size_t length) { return (sizeof(Header) + length + 3) & ~size_t(3); }
	/// What a bulk message leaves free: one more message of the largest size (cost(kMaxMessage)).
	static constexpr size_t kBulkReserve = (sizeof(Header) + kMaxMessage + 3) & ~size_t(3);

	/// What the reader gets: one message and its clients, valid until pop().
	struct Entry {
		const uint32_t* clients = nullptr;
		size_t count = 0;
		bool close = false; ///< A close frame, not a text message.
		const char* data = nullptr;
		size_t length = 0;
	};

	WsSendRing(uint8_t* storage, size_t capacity);

	// ---- The writer (one thread) ----

	/// Room for one message of at most `max` bytes (contiguous), or nullptr when it does not fit now.
	/// One claim at a time; commit() or abandon() ends it.
	char* claim(size_t max, bool bulk);
	/// Queues the claim's first `length` (<= the claimed max) bytes to `clients` (1..kMaxRecipients).
	void commit(const uint32_t* clients, size_t count, size_t length, bool close = false);
	/// Gives the claim back (nothing is sent).
	void abandon() { claimed_ = 0; }
	/// Queues a copy of `data`; false when it does not fit now.
	bool push(const uint32_t* clients, size_t count, const char* data, size_t length, bool close, bool bulk);
	bool claimed() const { return claimed_ != 0; }

	// ---- The reader (one thread) ----

	/// The oldest queued message; false when there is none.
	bool front(Entry& entry);
	/// Releases the message front() returned.
	void pop();

	/// Free bytes (either side; a snapshot).
	size_t free_bytes() const { return capacity_ - (head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire)); }
	size_t capacity() const { return capacity_; }
	/// The lowest free space seen by a claim or push since the start (the watermark for diagnostics).
	size_t lowest_free() const { return lowest_free_; }

private:
	/// Pads the `contiguous` bytes from `offset` (the counter `head`) to the end; the new head.
	uint32_t write_pad(uint32_t head, size_t offset, size_t contiguous);

	static constexpr uint8_t kPad = 1;
	static constexpr uint8_t kClose = 2;

	uint8_t* storage_;
	size_t capacity_;
	/// Free-running byte counters (the offset is the counter modulo the capacity): the writer owns
	/// head_, the reader tail_.
	std::atomic<uint32_t> head_{0};
	std::atomic<uint32_t> tail_{0};
	/// The writer's claim: its counter + 1 (0: none), and its largest length.
	uint32_t claimed_ = 0;
	size_t claimed_max_ = 0;
	size_t lowest_free_;
	/// The reader's current entry: its cost (0: none).
	size_t front_cost_ = 0;
};

} // namespace hc

#endif // HC_WS_SEND_RING_H
