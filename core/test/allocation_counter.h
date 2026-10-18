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

} // namespace hc_test

#endif // HC_TEST_ALLOCATION_COUNTER_H
