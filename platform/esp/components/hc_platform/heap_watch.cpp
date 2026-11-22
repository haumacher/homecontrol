/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// The heap's lowest point and the failed allocations, recorded where they happen (homecontrol #110).
// The hook and the callback run inside the heap's allocation functions, in the allocating task: they
// are in IRAM, never log, never allocate, and call flash code only in a task with the scheduler
// running (then the cache is on: a flash operation suspends the scheduler before it turns the cache
// off).

#include "heap_watch.h"

#include <atomic>
#include <cstdio>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace hc {
namespace esp {
namespace heap_watch {

namespace {

constexpr size_t kTaskName = configMAX_TASK_NAME_LEN;

/// The heap at one moment, with the allocation that led there.
struct Moment {
	int64_t at_us = 0;
	uint32_t free = 0;        ///< All of the heap (MALLOC_CAP_DEFAULT).
	uint32_t free_dma = 0;    ///< Of it, the DMA-capable RAM.
	uint32_t block_dma = 0;   ///< The largest free block of the DMA-capable RAM ...
	uint32_t block_lp = 0;    ///< ... and of the LP RAM,
	uint32_t blocks_at = 0;   ///< measured at this free heap (0: not measured).
	uint32_t request = 0;
	uint32_t caps = 0;
	char task[kTaskName] = {};
};

portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> g_started{false};
uint32_t g_walk_below = 0;

/// The lowest free heap an allocation left; UINT32_MAX until the first.
std::atomic<uint32_t> g_low_free{UINT32_MAX};
/// The free heap of the last walk (UINT32_MAX: none yet).
std::atomic<uint32_t> g_walked_at{UINT32_MAX};
Moment g_low;          ///< Under g_lock.

uint32_t g_failures = 0; ///< Under g_lock: failures since the last take.
Moment g_failure;      ///< Under g_lock: the first of them.

/// A task allocates with the scheduler running: the cache is on and flash code may run.
IRAM_ATTR bool in_task() {
	return g_started.load(std::memory_order_relaxed) && !xPortInIsrContext() &&
		xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
}

IRAM_ATTR void copy_task_name(char* out) {
	const char* name = pcTaskGetName(nullptr);
	size_t i = 0;
	if (name != nullptr) {
		for (; i + 1 < kTaskName && name[i] != '\0'; ++i) {
			out[i] = name[i];
		}
	}
	out[i] = '\0';
}

/// The largest blocks, a walk of the heap under its lock. Not inside a critical section.
IRAM_ATTR void measure_blocks(Moment& m) {
	m.block_dma = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
	m.block_lp = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_RTCRAM));
	m.blocks_at = m.free;
}

/// "HP 0 B (block 0 B), LP 1840 B (block 1840 B)"; the blocks only when measured.
int describe_regions(const Moment& m, char* out, size_t size) {
	uint32_t lp = m.free >= m.free_dma ? m.free - m.free_dma : 0;
	if (m.blocks_at == 0) {
		return std::snprintf(out, size, "HP %u B, LP %u B", static_cast<unsigned>(m.free_dma),
			static_cast<unsigned>(lp));
	}
	return std::snprintf(out, size, "HP %u B (block %u B), LP %u B (block %u B)",
		static_cast<unsigned>(m.free_dma), static_cast<unsigned>(m.block_dma), static_cast<unsigned>(lp),
		static_cast<unsigned>(m.block_lp));
}

const char* kind(uint32_t caps) {
	return (caps & MALLOC_CAP_DMA) != 0 ? " DMA" : "";
}

/// "20.94 s" from microseconds since boot.
void seconds(int64_t us, char* out, size_t size) {
	int64_t ms = us / 1000;
	std::snprintf(out, size, "%lld.%02lld s", static_cast<long long>(ms / 1000), static_cast<long long>(ms % 1000 / 10));
}

IRAM_ATTR void failed(size_t size, uint32_t caps, const char* function) {
	(void)function;
	if (!in_task()) {
		portENTER_CRITICAL_SAFE(&g_lock);
		++g_failures;
		portEXIT_CRITICAL_SAFE(&g_lock);
		return;
	}
	portENTER_CRITICAL(&g_lock);
	bool first = g_failures++ == 0;
	portEXIT_CRITICAL(&g_lock);
	if (!first) {
		return;
	}
	Moment m;
	m.at_us = esp_timer_get_time();
	m.free = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
	m.free_dma = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_DMA));
	m.request = static_cast<uint32_t>(size);
	m.caps = caps;
	copy_task_name(m.task);
	if (xPortCanYield()) {
		measure_blocks(m);
	}
	portENTER_CRITICAL(&g_lock);
	g_failure = m;
	portEXIT_CRITICAL(&g_lock);
}

} // namespace

void start(uint32_t walk_below) {
	g_walk_below = walk_below;
	heap_caps_register_failed_alloc_callback(&failed);
	g_started.store(true);
}

bool low(char* out, size_t size) {
	portENTER_CRITICAL(&g_lock);
	Moment m = g_low;
	portEXIT_CRITICAL(&g_lock);
	if (m.at_us == 0) {
		return false;
	}
	char at[24];
	seconds(m.at_us, at, sizeof(at));
	int n = std::snprintf(out, size, "lowest %u B at %s: ", static_cast<unsigned>(m.free), at);
	if (n < 0 || static_cast<size_t>(n) >= size) {
		return true;
	}
	int r = describe_regions(m, out + n, size - static_cast<size_t>(n));
	if (r < 0) {
		return true;
	}
	n += r;
	if (static_cast<size_t>(n) < size && m.blocks_at != 0 && m.blocks_at != m.free) {
		r = std::snprintf(out + n, size - static_cast<size_t>(n), ", blocks at %u B free",
			static_cast<unsigned>(m.blocks_at));
		n += r > 0 ? r : 0;
	}
	if (static_cast<size_t>(n) < size) {
		std::snprintf(out + n, size - static_cast<size_t>(n), "; %s took %u B%s", m.task,
			static_cast<unsigned>(m.request), kind(m.caps));
	}
	return true;
}

uint32_t take_failures(char* out, size_t size) {
	portENTER_CRITICAL(&g_lock);
	uint32_t count = g_failures;
	Moment m = g_failure;
	g_failures = 0;
	g_failure = Moment();
	portEXIT_CRITICAL(&g_lock);
	if (count == 0) {
		return 0;
	}
	if (m.at_us == 0) {
		// Failures in an interrupt or with the scheduler suspended, or the first is still being recorded.
		std::snprintf(out, size, "not measured (in an interrupt or with the scheduler suspended)");
		return count;
	}
	char at[24];
	seconds(m.at_us, at, sizeof(at));
	int n = std::snprintf(out, size, "%u B%s for %s at %s; free %u B: ", static_cast<unsigned>(m.request), kind(m.caps),
		m.task, at, static_cast<unsigned>(m.free));
	if (n > 0 && static_cast<size_t>(n) < size) {
		describe_regions(m, out + n, size - static_cast<size_t>(n));
	}
	return count;
}

void describe_now(char* out, size_t size) {
	Moment m;
	m.free = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
	m.free_dma = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_DMA));
	measure_blocks(m);
	int n = std::snprintf(out, size, "heap %u B: ", static_cast<unsigned>(m.free));
	if (n > 0 && static_cast<size_t>(n) < size) {
		describe_regions(m, out + n, size - static_cast<size_t>(n));
	}
}

} // namespace heap_watch
} // namespace esp
} // namespace hc

#if CONFIG_HEAP_USE_HOOKS
// Called by the heap after every successful allocation, in the allocating task (or interrupt).
// IRAM through the declaration's HEAP_IRAM_ATTR (esp_heap_caps.h).
extern "C" void esp_heap_trace_alloc_hook(void* ptr, size_t size, uint32_t caps) {
	using namespace hc::esp::heap_watch;
	(void)ptr;
	if (!in_task()) {
		return;
	}
	uint32_t free = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
	if (free >= g_low_free.load(std::memory_order_relaxed)) {
		return;
	}
	Moment m;
	m.at_us = esp_timer_get_time();
	m.free = free;
	m.free_dma = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_DMA));
	m.request = static_cast<uint32_t>(size);
	m.caps = caps;
	copy_task_name(m.task);
	uint32_t walked_at = g_walked_at.load(std::memory_order_relaxed);
	bool walk = free < g_walk_below && (walked_at == UINT32_MAX || free + kWalkStep <= walked_at) && xPortCanYield();
	if (walk) {
		measure_blocks(m);
	}
	portENTER_CRITICAL(&g_lock);
	if (free < g_low_free.load(std::memory_order_relaxed)) {
		g_low_free.store(free, std::memory_order_relaxed);
		if (!walk) {
			// Keep the blocks of the last walk (named with the heap they were measured at).
			m.block_dma = g_low.block_dma;
			m.block_lp = g_low.block_lp;
			m.blocks_at = g_low.blocks_at;
		} else {
			g_walked_at.store(free, std::memory_order_relaxed);
		}
		g_low = m;
	}
	portEXIT_CRITICAL(&g_lock);
}
#endif
