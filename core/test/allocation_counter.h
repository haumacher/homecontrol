/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Allocation bookkeeping of the test binary: allocation_counter.cpp replaces the global allocation
// functions and records, while an AllocationScope is active, the number of allocations, the largest
// single block and the peak of the bytes held (what the steady-state and memory-budget checks
// assert).
#ifndef HC_TEST_ALLOCATION_COUNTER_H
#define HC_TEST_ALLOCATION_COUNTER_H

#include <cstddef>

namespace hc_test {

/// Counts the allocations made while it lives (scopes do not nest).
class AllocationScope {
public:
	AllocationScope();
	~AllocationScope();
	AllocationScope(const AllocationScope&) = delete;
	AllocationScope& operator=(const AllocationScope&) = delete;

	/// Allocations made in the scope.
	size_t count() const;
	/// The largest single block allocated in the scope.
	size_t largest() const;
	/// The most bytes held at once by the blocks allocated in the scope.
	size_t peak() const;
	/// Bytes still held by the blocks allocated in the scope.
	size_t live() const;
};

/// The whole heap of the test binary, counted always (every block, inside a scope or not): the bytes
/// held now, and the most held at once since the last heap_mark() (homecontrol #70: the peak of each
/// phase of an image swap).
size_t heap_live();
/// The blocks held now.
size_t heap_blocks();
/// The most bytes held at once since the last mark (or the start).
size_t heap_peak();
/// Starts a new peak at what is held now; returns the peak that ended.
size_t heap_mark();
/// The same figures as the ESP32-C6's heap charges them (its TLSF allocator: each block at least 12
/// bytes, rounded up to 4, plus a 4-byte header): what heap_caps_get_free_size() would lose. Exact
/// for the sizes of the 32-bit build (doc/image.md "Memory of a swap"), a model on the 64-bit host.
size_t heap_charged();
/// The most charged at once since the last heap_mark().
size_t heap_charged_peak();
/// The largest single block allocated since the last heap_mark() (what needs one contiguous piece).
size_t heap_largest();

/// While one lives, every counted allocation (not one under a HeapExclusion) larger than `bytes`
/// fails: a nothrow allocation returns nullptr, a throwing one aborts. The model of a fragmented heap
/// whose largest free block is `bytes` (homecontrol #70).
class BlockLimit {
public:
	explicit BlockLimit(size_t bytes);
	~BlockLimit();
	BlockLimit(const BlockLimit&) = delete;
	BlockLimit& operator=(const BlockLimit&) = delete;
	/// The allocations it refused.
	size_t refused() const;
};

/// While one lives (they nest), new blocks are left out of the whole-heap figures above: the test
/// fakes of what the device keeps outside its heap (the blob store's flash, the NVS, the log ring).
class HeapExclusion {
public:
	HeapExclusion();
	~HeapExclusion();
	HeapExclusion(const HeapExclusion&) = delete;
	HeapExclusion& operator=(const HeapExclusion&) = delete;
};

} // namespace hc_test

#endif // HC_TEST_ALLOCATION_COUNTER_H
