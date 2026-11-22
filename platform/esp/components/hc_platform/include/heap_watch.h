/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
#ifndef HC_ESP_HEAP_WATCH_H
#define HC_ESP_HEAP_WATCH_H

#include <cstddef>
#include <cstdint>

namespace hc {
namespace esp {

/// The heap's lowest point and the failed allocations, measured where they happen (homecontrol
/// #110, doc/diagnostics.md "Heap low and failed allocations").
///
/// - The low: the IDF's allocation hook (CONFIG_HEAP_USE_HOOKS) checks the free heap after every
///   allocation in a task (one pass over the half dozen heap regions, about a microsecond); at a
///   new lowest it records the free heap, its part in DMA-capable RAM, the allocation's size and
///   kind and the task that made it. Below `walk_below`, and then at most once per kWalkStep of
///   further descent, it also measures the largest free block of the DMA-capable RAM and of the LP
///   RAM (a walk of the heap with interrupts off, as GET /api/status does): at most 32 walks per
///   boot. Allocations in an interrupt, with the scheduler suspended or inside a critical section
///   are not measured (the walk is skipped inside a critical section).
/// - The failures: the IDF's failed-allocation callback counts every allocation that found no
///   memory and records the first one after each take_failures(), with the heap at that moment.
///
/// The figures stay in a few static records (no allocation, no log line from inside the heap); the
/// heartbeat logs them (EspSystemInfo::heap_low(), failed_allocations()).
///
/// The C6's heap has two kinds of memory: the HP SRAM, which DMA reaches (the AES and SHA
/// engines, WiFi), and the 16 KB LP SRAM (CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP), which
/// only the CPU reaches and which the heap hands out last. The free heap the IDF reports counts both;
/// an allocation that needs DMA-capable memory fails while LP RAM is still free.
namespace heap_watch {

/// Walks for the largest blocks happen at new lows this far apart (bytes).
constexpr uint32_t kWalkStep = 512;

/// Starts watching; call once from app_main (the scheduler runs). `walk_below`: the free heap below
/// which a new low also measures the largest blocks.
void start(uint32_t walk_below);

/// The account of the lowest point so far, one line: "lowest 1840 B at 20.94 s: HP 0 B (block 0 B),
/// LP 1840 B (block 1840 B); hc_ehome_tx took 1600 B DMA" (HP: the DMA-capable HP SRAM, LP: the LP
/// SRAM). The blocks are those of the last walk, followed by ", blocks at 2300 B free" when it was not
/// at the low itself, and left out before the first walk. False before the first allocation was
/// watched.
bool low(char* out, size_t size);

/// The lowest point since the previous call, when it was below `walk_below` (the heartbeat's
/// window, homecontrol #110): the same account as low(). Starts a new window. False: none below.
/// Lows above `walk_below` are not tracked for the window (the hook's cost stays one figure per
/// allocation in steady state); its walks follow the same rule as the boot-wide low's, per window.
bool take_window(char* out, size_t size);

/// The DMA-capable memory now (free, largest block; a walk). False: no such memory.
bool dma_room(uint32_t& free, uint32_t& largest);

/// Calls `fn(context)` once, from the task that frees, as soon as a free leaves at least `dma_free`
/// bytes of DMA-capable memory (the free hook; not in an interrupt or a critical section). `fn` must
/// not block, allocate or log. Replaces an earlier request.
void notify_free_at(uint32_t dma_free, void (*fn)(void*), void* context);
void cancel_free_notify();

/// The failed allocations since the previous call; the first of them described in `out`: "1600 B
/// DMA for hc_ehome_tx at 20.94 s; free 1840 B: HP 0 B (block 0 B), LP 1840 B (block 1840 B)".
/// Returns 0 (and leaves `out` alone) when none failed.
uint32_t take_failures(char* out, size_t size);

/// The heap now, one short text for a failure report: "heap 1840 B: HP 0 B (block 0 B), LP 1840 B
/// (block 1840 B)". Walks the heap: not for a steady-state path.
void describe_now(char* out, size_t size);

} // namespace heap_watch

} // namespace esp
} // namespace hc

#endif // HC_ESP_HEAP_WATCH_H
