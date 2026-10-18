/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Replaces the global allocation functions of the test binary (one definition for all tests).
// Every block carries a header with its size and whether it was allocated in a scope.
#include "allocation_counter.h"

#include <atomic>
#include <cstdlib>
#include <new>

namespace {

std::atomic<bool> g_counting{false};
std::atomic<size_t> g_count{0};
std::atomic<size_t> g_largest{0};
std::atomic<size_t> g_live{0};
std::atomic<size_t> g_peak{0};

struct alignas(alignof(std::max_align_t)) Header {
	size_t size;
	bool counted;
};

void* allocate(size_t size) noexcept {
	void* raw = std::malloc(sizeof(Header) + (size ? size : 1));
	if (raw == nullptr) {
		return nullptr;
	}
	Header* header = static_cast<Header*>(raw);
	header->size = size;
	header->counted = g_counting.load();
	if (header->counted) {
		++g_count;
		size_t largest = g_largest.load();
		while (size > largest && !g_largest.compare_exchange_weak(largest, size)) {
		}
		size_t live = g_live.fetch_add(size) + size;
		size_t peak = g_peak.load();
		while (live > peak && !g_peak.compare_exchange_weak(peak, live)) {
		}
	}
	return header + 1;
}

void release(void* p) noexcept {
	if (p == nullptr) {
		return;
	}
	Header* header = static_cast<Header*>(p) - 1;
	if (header->counted) {
		g_live.fetch_sub(header->size);
	}
	std::free(header);
}

} // namespace

namespace hc_test {

AllocationScope::AllocationScope() {
	g_count = 0;
	g_largest = 0;
	g_live = 0;
	g_peak = 0;
	g_counting = true;
}

AllocationScope::~AllocationScope() { g_counting = false; }

size_t AllocationScope::count() const { return g_count.load(); }
size_t AllocationScope::largest() const { return g_largest.load(); }
size_t AllocationScope::peak() const { return g_peak.load(); }
size_t AllocationScope::live() const { return g_live.load(); }

} // namespace hc_test

void* operator new(size_t size) {
	void* p = allocate(size);
	if (p == nullptr) {
		std::abort();
	}
	return p;
}
void* operator new[](size_t size) { return operator new(size); }
void* operator new(size_t size, const std::nothrow_t&) noexcept { return allocate(size); }
void* operator new[](size_t size, const std::nothrow_t&) noexcept { return allocate(size); }
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, size_t) noexcept { release(p); }
void operator delete[](void* p, size_t) noexcept { release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }
