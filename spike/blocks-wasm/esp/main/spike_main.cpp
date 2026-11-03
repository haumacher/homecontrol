/*
 * Copyright (c) 2026 Bernhard Haumacher
 */

/*
 * The measurement app of the WebAssembly block spike on the ESP32-C6 (issue #59,
 * doc/blocks-wasm.md). Runs every measurement once after each boot (opening the serial port resets
 * the board, so a capture always sees a complete run) and prints lines "SPIKE ..." on the console;
 * the last one is "SPIKE DONE". Afterwards it prints a heartbeat every 2 s.
 *
 * Flash: the app lives in ota_0 (0x100000) like the controller. Its scratch region is inside ota_0
 * too, at offset 0x180000 (0x280000 absolute, 256 KB: a LittleFS of 192 KB and 64 KB for AOT code
 * executed in place), past the end of this app and below the end of the controller's image, so
 * restoring the controller overwrites it. Nothing outside ota_0 is written.
 *
 * A stage that crashes the board is skipped at the next boot (a marker in RTC memory), so a crash
 * cannot turn into a boot loop that locks the USB port.
 */

#include "spike_runner.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_partition.h"
#include "esp_pthread.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define EMBED(name)                                                                      \
	extern const uint8_t name##_start[] asm("_binary_" #name "_start");                \
	extern const uint8_t name##_end[] asm("_binary_" #name "_end");

EMBED(room_controller_wasm)
EMBED(room_controller_rs_wasm)
EMBED(probes_wasm)
EMBED(bulk_wasm)
EMBED(room_controller_riscv32_aot)
EMBED(probes_riscv32_aot)
EMBED(bulk_riscv32_aot)
EMBED(room_controller_noxip_riscv32_aot)
EMBED(series_csv)
EMBED(expected_csv)

#if CONFIG_WAMR_INTERP_CLASSIC
#define INTERP "classic interpreter"
/* The classic interpreter runs the code from the binary: it must be kept. */
constexpr bool kBinaryFreeable = false;
#else
#define INTERP "fast interpreter"
/* The fast interpreter compiles the code into buffers of its own (host test: "the binary may be
 * discarded after the load"). */
constexpr bool kBinaryFreeable = true;
#endif

namespace spike {

uint64_t now_us()
{
	return static_cast<uint64_t>(esp_timer_get_time());
}

void out(const char *format, ...)
{
	char line[256];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof line, format, args);
	va_end(args);
	printf("SPIKE %s\n", line);
	fflush(stdout);
}

} // namespace spike

using namespace spike;

namespace {

/* ---- Crash protection: the stage in progress survives a panic reset in RTC memory. ---- */

/* Per build: a new build starts with no stage skipped. */
constexpr uint32_t build_magic(const char *s, uint32_t h = 0x5917b10c)
{
	return *s ? build_magic(s + 1, (h ^ static_cast<uint8_t>(*s)) * 16777619u) : h;
}
constexpr uint32_t kMagic = build_magic(__DATE__ " " __TIME__);
RTC_NOINIT_ATTR uint32_t g_rtc_magic;
RTC_NOINIT_ATTR uint32_t g_rtc_stage;
RTC_NOINIT_ATTR uint32_t g_rtc_skip;

bool enter(uint32_t stage, const char *name)
{
	if (g_rtc_skip & (1u << stage)) {
		out("stage %" PRIu32 " %s: SKIPPED (it crashed the board before)", stage, name);
		return false;
	}
	g_rtc_stage = stage;
	out("stage %" PRIu32 " %s", stage, name);
	esp_task_wdt_reset();
	return true;
}

/* Gives the idle task its turn (the task watchdog also watches it) between the measurements. */
void breathe()
{
	esp_task_wdt_reset();
	vTaskDelay(pdMS_TO_TICKS(20));
}

void leave()
{
	g_rtc_stage = 0;
	breathe();
}

/* ---- Heap figures ---- */

struct Heap {
	size_t free;
	size_t largest;
};

Heap heap()
{
	return {heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)};
}

long used_since(const Heap &before)
{
	return static_cast<long>(before.free) - static_cast<long>(heap().free);
}

uint64_t fnv(uint64_t hash, uint64_t value)
{
	for (int i = 0; i < 8; i++) {
		hash ^= (value >> (8 * i)) & 0xff;
		hash *= 0x100000001b3ull;
	}
	return hash;
}

/* The hash replay() computes, over the rows of expected.csv (t and three 64-bit hex fields). */
uint64_t expected_hash(uint32_t *rows)
{
	const char *p = reinterpret_cast<const char *>(expected_csv_start);
	const char *end = reinterpret_cast<const char *>(expected_csv_end);
	uint64_t hash = 0xcbf29ce484222325ull;
	*rows = 0;
	while (p < end && *p != '\n')
		p++;
	while (p < end) {
		while (p < end && (*p == '\n' || *p == '\r' || *p == 0))
			p++;
		if (p >= end)
			break;
		char *next;
		hash = fnv(hash, static_cast<uint64_t>(strtoll(p, &next, 10)));
		for (int i = 0; i < 3; i++) {
			p = next + 1;
			hash = fnv(hash, strtoull(p, &next, 16));
		}
		p = next;
		(*rows)++;
	}
	return hash;
}

size_t size_of(const uint8_t *start, const uint8_t *end)
{
	return static_cast<size_t>(end - start);
}

/* ---- The scratch region in ota_0 ---- */

constexpr uint32_t kScratchOffset = 0x180000; /* inside ota_0 */
constexpr uint32_t kFsSize = 0x30000;
constexpr uint32_t kXipOffset = kScratchOffset + kFsSize; /* 0x1B0000, 64 KB */
constexpr uint32_t kXipSize = 0x10000;

const esp_partition_t *g_ota0;
const uint8_t *g_xip_base; /* the mapped XIP area */
esp_partition_mmap_handle_t g_xip_handle;
uint32_t g_xip_room, g_xip_probes, g_xip_bulk; /* offsets of the AOT files in it */

bool prepare_xip()
{
	if (esp_partition_erase_range(g_ota0, kXipOffset, kXipSize) != ESP_OK)
		return false;
	uint32_t at = 0;
	struct {
		const uint8_t *start, *end;
		uint32_t *offset;
	} files[] = {
		{room_controller_riscv32_aot_start, room_controller_riscv32_aot_end, &g_xip_room},
		{probes_riscv32_aot_start, probes_riscv32_aot_end, &g_xip_probes},
		{bulk_riscv32_aot_start, bulk_riscv32_aot_end, &g_xip_bulk},
	};
	for (auto &f : files) {
		size_t n = size_of(f.start, f.end);
		if (at + n > kXipSize || esp_partition_write(g_ota0, kXipOffset + at, f.start, n) != ESP_OK)
			return false;
		*f.offset = at;
		at = (at + n + 0xfff) & ~0xfffu;
	}
	const void *p;
	if (esp_partition_mmap(g_ota0, kXipOffset, kXipSize, ESP_PARTITION_MMAP_INST, &p, &g_xip_handle) != ESP_OK)
		return false;
	g_xip_base = static_cast<const uint8_t *>(p);
	out("  AOT files written to flash 0x%" PRIx32 " and mapped for execution at %p", g_ota0->address + kXipOffset, p);
	return true;
}

/* ---- Stages ---- */

bool open_wasm(Block &block, const uint8_t *start, const uint8_t *end, uint32_t stack = 2048)
{
	/* A .wasm must be loaded from RAM: the loader rewrites the buffer. */
	if (!block.load(const_cast<uint8_t *>(start), static_cast<uint32_t>(size_of(start, end)), true)) {
		out("  load failed: %s", block.error());
		return false;
	}
	if (!block.instantiate(stack)) {
		out("  instantiate failed: %s", block.error());
		return false;
	}
	return true;
}

bool open_xip(Block &block, uint32_t offset, const uint8_t *start, const uint8_t *end, uint32_t stack = 2048)
{
	if (!g_xip_base) {
		out("  no XIP mapping");
		return false;
	}
	if (!block.load(const_cast<uint8_t *>(g_xip_base + offset), static_cast<uint32_t>(size_of(start, end)))) {
		out("  AOT load failed: %s", block.error());
		return false;
	}
	if (!block.instantiate(stack)) {
		out("  AOT instantiate failed: %s", block.error());
		return false;
	}
	return true;
}

/* Sets the native stack boundary once, so WAMR does not ask FreeRTOS for it at every call
 * (vTaskGetInfo with the high-water mark scans the task's stack). */
void set_boundary(Block &block)
{
	TaskStatus_t status;
	vTaskGetInfo(nullptr, &status, pdFALSE, eRunning);
	/* 3 KB above the stack's end: what WAMR's exception path and a ROM printf (1.25 KB) need. */
	wasm_runtime_set_native_stack_boundary(block.env, reinterpret_cast<uint8_t *>(status.pxStackBase) + 3072);
}

void stage_loads()
{
	struct {
		const char *name;
		const uint8_t *start, *end;
	} modules[] = {
		{"room_controller.wasm", room_controller_wasm_start, room_controller_wasm_end},
		{"probes.wasm", probes_wasm_start, probes_wasm_end},
		{"bulk.wasm", bulk_wasm_start, bulk_wasm_end},
	};
	for (auto &m : modules) {
		size_t size = size_of(m.start, m.end);
		for (int freeable = 0; freeable < (kBinaryFreeable ? 2 : 1); freeable++) {
			Heap h0 = heap();
			AllocStats a0 = g_alloc;
			uint8_t *copy = static_cast<uint8_t *>(malloc(size));
			memcpy(copy, m.start, size);
			Block block;
			uint64_t t0 = now_us();
			bool ok = block.load(copy, static_cast<uint32_t>(size), false, freeable != 0);
			uint64_t t1 = now_us();
			if (freeable) {
				free(copy);
				copy = nullptr;
			}
			long loaded = used_since(h0);
			Heap h1 = heap();
			ok = ok && block.instantiate(2048);
			uint64_t t2 = now_us();
			out("  %s %u B%s: load+validate %" PRIu64 " us, heap %ld B (runtime %" PRId64 " B, %" PRIu32 " allocs); "
					"instance %" PRIu64 " us, heap %ld B, linear memory %" PRIu32 " B%s%s",
					m.name, static_cast<unsigned>(size), freeable ? " (binary freed after load)" : " (binary kept)",
					t1 - t0, loaded, g_alloc.live - a0.live, g_alloc.mallocs - a0.mallocs, t2 - t1, used_since(h1),
					ok ? block.memory_size() : 0, ok ? "" : " FAILED: ", ok ? "" : block.error());
			block.unload();
			free(copy);
		}
	}
	/* AOT, executed in place from the flash mapping: only the module's data is in RAM. */
	struct {
		const char *name;
		uint32_t *offset;
		const uint8_t *start, *end;
	} aots[] = {
		{"room_controller.riscv32.aot", &g_xip_room, room_controller_riscv32_aot_start, room_controller_riscv32_aot_end},
		{"bulk.riscv32.aot", &g_xip_bulk, bulk_riscv32_aot_start, bulk_riscv32_aot_end},
	};
	for (auto &m : aots) {
		Heap h0 = heap();
		Block block;
		uint64_t t0 = now_us();
		bool ok = g_xip_base && block.load(const_cast<uint8_t *>(g_xip_base + *m.offset),
				static_cast<uint32_t>(size_of(m.start, m.end)));
		uint64_t t1 = now_us();
		long loaded = used_since(h0);
		Heap h1 = heap();
		ok = ok && block.instantiate(2048);
		uint64_t t2 = now_us();
		out("  %s %u B (XIP from flash): load %" PRIu64 " us, heap %ld B; instance %" PRIu64 " us, heap %ld B%s%s",
				m.name, static_cast<unsigned>(size_of(m.start, m.end)), t1 - t0, loaded, t2 - t1, used_since(h1),
				ok ? "" : " FAILED: ", ok ? "" : block.error());
		block.unload();
	}
	/* AOT without XIP: the loader copies the code to executable RAM, which the PMP split of the
	 * controller's configuration does not offer (MALLOC_CAP_EXEC is empty). */
	{
		size_t size = size_of(room_controller_noxip_riscv32_aot_start, room_controller_noxip_riscv32_aot_end);
		uint8_t *copy = static_cast<uint8_t *>(malloc(size));
		memcpy(copy, room_controller_noxip_riscv32_aot_start, size);
		Block block;
		bool ok = block.load(copy, static_cast<uint32_t>(size));
		out("  room_controller.noxip.riscv32.aot (code to RAM): %s%s", ok ? "loaded" : "FAILED: ", ok ? "" : block.error());
		block.unload();
		free(copy);
	}
}

void replay_line(const char *what, Block &block, uint64_t expected)
{
	ReplayResult r = replay(block, reinterpret_cast<const char *>(series_csv_start), size_of(series_csv_start, series_csv_end),
			0, nullptr, nullptr);
	out("  %s: %" PRIu32 " rows, hash %016" PRIx64 " %s, %" PRIu64 " us in calls (%" PRIu64 " us per call), longest %" PRIu64
			" us%s%s",
			what, r.rows, r.hash, r.ok && r.hash == expected ? "== expected" : "DIFFERS", r.total_us,
			r.rows ? r.total_us / r.rows : 0, r.max_call_us, r.ok ? "" : ", stopped: ", r.reason);
	out("HASH %s %016" PRIx64, what, r.hash);
}

void stage_replay()
{
	uint32_t rows;
	uint64_t expected = expected_hash(&rows);
	out("  expected.csv: %" PRIu32 " rows, hash %016" PRIx64, rows, expected);
	{
		Block block;
		if (open_wasm(block, room_controller_wasm_start, room_controller_wasm_end))
			replay_line("C block, " INTERP, block, expected);
		block.unload();
	}
	{
		Block block;
		if (open_wasm(block, room_controller_rs_wasm_start, room_controller_rs_wasm_end))
			replay_line("Rust block, " INTERP, block, expected);
		block.unload();
	}
	{
		Block block;
		if (open_xip(block, g_xip_room, room_controller_riscv32_aot_start, room_controller_riscv32_aot_end))
			replay_line("C block, AOT XIP", block, expected);
		block.unload();
	}
}

void steady(const char *what, Block &block, bool boundary)
{
	if (boundary)
		set_boundary(block);
	room_parameters(block.ports);
	block.ports.in[0] = 20.0;
	block.ports.in[1] = 21.0;
	block.ports.in[2] = 1.0;
	uint32_t argv[3] = {0, 0, 0};
	block.call("hc_start", 3, argv, 100000);
	wasm_function_inst_t period = block.lookup("hc_period");
	const int n = 2000;
	Heap h0 = heap();
	AllocStats a0 = g_alloc;
	uint64_t t0 = now_us();
	for (int i = 0; i < n; i++) {
		block.ports.now_ms += 1000;
		argv[0] = 0;
		block.call(period, 1, argv, 100000);
	}
	uint64_t t1 = now_us();
	out("  %s%s: PI step %.1f us per call; %d calls: %" PRIu32 " mallocs, %" PRIu32 " reallocs, %" PRIu32
			" frees by the runtime, heap %ld B",
			what, boundary ? ", stack boundary set" : "", (t1 - t0) / static_cast<double>(n), n, g_alloc.mallocs - a0.mallocs,
			g_alloc.reallocs - a0.reallocs, g_alloc.frees - a0.frees, used_since(h0));
}

/* The instructions one PI step costs: the smallest budget it completes with (bisection). */
void instructions_per_step()
{
	int32_t lo = 1, hi = 100000;
	while (lo < hi) {
		int32_t mid = lo + (hi - lo) / 2;
		Block block;
		if (!open_wasm(block, room_controller_wasm_start, room_controller_wasm_end))
			return;
		room_parameters(block.ports);
		block.ports.in[0] = 20.0;
		block.ports.in[1] = 21.0;
		block.ports.in[2] = 1.0;
		uint32_t argv[3] = {0, 0, 0};
		block.call("hc_start", 3, argv, 100000);
		block.ports.now_ms = 60000;
		argv[0] = 0;
		if (block.call("hc_period", 1, argv, mid))
			hi = mid;
		else
			lo = mid + 1;
		block.unload();
	}
	out("  one PI step (hc_period) needs a budget of %" PRId32 " instructions", lo);
}

void stage_steady()
{
	instructions_per_step();
	for (int boundary = 0; boundary < 2; boundary++) {
		Block block;
		if (open_wasm(block, room_controller_wasm_start, room_controller_wasm_end))
			steady(INTERP, block, boundary != 0);
		block.unload();
		Block aot;
		if (open_xip(aot, g_xip_room, room_controller_riscv32_aot_start, room_controller_riscv32_aot_end))
			steady("AOT XIP", aot, boundary != 0);
		aot.unload();
	}
}

void bench(const char *what, Block &block)
{
	set_boundary(block);
	const int n = 2000;
	const char *benches[] = {"bench_loop", "bench_now", "bench_read", "bench_write", "bench_log", "bench_float"};
	for (const char *b : benches) {
		uint32_t argv[2] = {static_cast<uint32_t>(n), 0};
		uint64_t t0 = now_us();
		bool ok = block.call(b, 1, argv);
		out("  %s %s: %.2f us per iteration%s", what, b, (now_us() - t0) / static_cast<double>(n), ok ? "" : " FAILED");
	}
	wasm_function_inst_t ok = block.lookup("ok");
	uint64_t t0 = now_us();
	for (int i = 0; i < n; i++) {
		uint32_t argv[1] = {0};
		block.call(ok, 0, argv, 1000);
	}
	out("  %s call into the module (empty export): %.2f us", what, (now_us() - t0) / static_cast<double>(n));
}

void stage_bench()
{
	Block block;
	if (open_wasm(block, probes_wasm_start, probes_wasm_end))
		bench("interp", block);
	block.unload();
	Block aot;
	if (open_xip(aot, g_xip_probes, probes_riscv32_aot_start, probes_riscv32_aot_end))
		bench("aot", aot);
	aot.unload();
}

void stage_budget()
{
	/* How long an instruction budget lets an endless loop run (fast interpreter). */
	const int32_t budgets[] = {10000, 100000, 1000000, 5000000};
	for (int32_t budget : budgets) {
		Block block;
		if (!open_wasm(block, probes_wasm_start, probes_wasm_end))
			return;
		set_boundary(block);
		uint32_t argv[1] = {0};
		uint64_t t0 = now_us();
		bool ok = block.call("loop_forever", 0, argv, budget);
		uint64_t spent = now_us() - t0;
		breathe();
		out("  budget %" PRId32 " instructions: loop_forever stopped after %" PRIu64 " us (%.1f instructions/us): %s",
				budget, spent, budget / static_cast<double>(spent), ok ? "NOT STOPPED" : block.reason());
		block.unload();
		esp_task_wdt_reset();
	}
}

void faults(const char *what, bool aot)
{
	struct {
		const char *name;
		bool on_aot;
	} probes[] = {
		{"loop_forever", false}, {"oob", true}, {"stack_overflow", true}, {"shadow_stack_overflow", true},
		{"unreachable", true}, {"trap", true}, {"div_zero", true},
	};
	Heap h0 = heap();
	Block sibling;
	open_wasm(sibling, room_controller_wasm_start, room_controller_wasm_end);
	room_parameters(sibling.ports);
	uint32_t sv[3] = {0, 0, 0};
	sibling.call("hc_start", 3, sv, 100000);
	for (auto &p : probes) {
		if (aot && !p.on_aot) {
			out("  %s %s: not run: AOT code has no instruction metering, the loop would hold the task until the watchdog",
					what, p.name);
			continue;
		}
		Block block;
		bool opened = aot ? open_xip(block, g_xip_probes, probes_riscv32_aot_start, probes_riscv32_aot_end)
				: open_wasm(block, probes_wasm_start, probes_wasm_end);
		if (!opened)
			continue;
		set_boundary(block);
		out("  %s %s ...", what, p.name);
		uint32_t argv[2] = {0, 0};
		uint64_t t0 = now_us();
		bool ok = block.call(p.name, 0, argv, 1000000);
		uint64_t spent = now_us() - t0;
		bool refused = !block.call("ok", 0, argv);
		sibling.ports.now_ms += 60000;
		sibling.ports.in[0] = 20.0;
		sibling.ports.in[1] = 21.0;
		sibling.ports.in[2] = 1.0;
		uint32_t pv[1] = {0};
		bool sibling_runs = sibling.call("hc_period", 1, pv, 100000);
		out("  %s %s: %s after %" PRIu64 " us: \"%s\"; later calls %s; the room controller beside it %s",
				what, p.name, ok ? "NOT STOPPED" : "stopped", spent, block.reason(), refused ? "refused" : "RUN",
				sibling_runs ? "runs" : "FAILS");
		block.unload();
		breathe();
	}
	sibling.unload();
	out("  %s faults: heap afterwards %ld B against before", what, used_since(h0));
}

void stage_faults()
{
	faults("interp", false);
	faults("aot", true);
}

void stage_instances()
{
	/* Eight room controllers as eight module instances, against eight block instances in one
	 * module instance (the module holds MAX_INSTANCES 8 state records). */
	Heap h0 = heap();
	static Block blocks[8]; /* static: eight Blocks would take 3 KB of the 9 KB stack */
	for (Block &b : blocks)
		open_wasm(b, room_controller_wasm_start, room_controller_wasm_end);
	long eight = used_since(h0);
	for (Block &b : blocks)
		b.unload();
	Heap h1 = heap();
	Block one;
	open_wasm(one, room_controller_wasm_start, room_controller_wasm_end);
	long single = used_since(h1);
	one.unload();
	out("  room controller, " INTERP ": one module instance %ld B; 8 separate instances %ld B (%ld B each); "
			"8 blocks in one instance: %ld B (the module holds 8 state records of 32 B)",
			single, eight, eight / 8, single);
	Heap h2 = heap();
	static Block aots[8];
	for (Block &b : aots)
		open_xip(b, g_xip_room, room_controller_riscv32_aot_start, room_controller_riscv32_aot_end);
	long eight_aot = used_since(h2);
	for (Block &b : aots)
		b.unload();
	out("  room controller, AOT XIP: 8 separate instances %ld B (%ld B each, module loaded 8 times)", eight_aot, eight_aot / 8);
}

void stage_swap()
{
	/* An image swap: unload the old module, load the new; repeated, nothing may stay behind. */
	Heap h0 = heap();
	AllocStats a0 = g_alloc;
	uint64_t worst = 0;
	for (int i = 0; i < 100; i++) {
		uint64_t t0 = now_us();
		Block block;
		const uint8_t *s = (i & 1) ? bulk_wasm_start : room_controller_wasm_start;
		const uint8_t *e = (i & 1) ? bulk_wasm_end : room_controller_wasm_end;
		if (!open_wasm(block, s, e))
			break;
		uint32_t argv[3] = {0, 0, 0};
		block.call("hc_abi", 0, argv);
		block.unload();
		uint64_t spent = now_us() - t0;
		if (spent > worst)
			worst = spent;
		if ((i & 7) == 7)
			breathe();
	}
	Heap h1 = heap();
	out("  100 swaps (room controller <-> bulk, load + instantiate + call + unload): longest %" PRIu64
			" us; heap %ld B against before, largest free block %u -> %u B; runtime live %" PRId64 " B",
			worst, used_since(h0), static_cast<unsigned>(h0.largest), static_cast<unsigned>(h1.largest),
			g_alloc.live - a0.live);
}

void stage_littlefs()
{
	/* A LittleFS of its own in the scratch region (a partition record not in the table: the
	 * esp_partition_* functions only use its address and size). */
	static esp_partition_t part;
	part = *g_ota0;
	part.address = g_ota0->address + kScratchOffset;
	part.size = kFsSize;
	part.type = ESP_PARTITION_TYPE_DATA;
	part.subtype = ESP_PARTITION_SUBTYPE_DATA_LITTLEFS;
	strlcpy(part.label, "spikefs", sizeof part.label);
	esp_vfs_littlefs_conf_t conf = {};
	conf.base_path = "/spike";
	conf.partition = &part;
	conf.format_if_mount_failed = true;
	Heap h0 = heap();
	esp_err_t err = esp_vfs_littlefs_register(&conf);
	if (err != ESP_OK) {
		out("  LittleFS mount failed: %s", esp_err_to_name(err));
		return;
	}
	out("  LittleFS of 192 KB mounted at 0x%" PRIx32 " (heap %ld B)", part.address, used_since(h0));
	const struct {
		const char *path;
		const uint8_t *start, *end;
	} files[] = {
		{"/spike/bulk.wasm", bulk_wasm_start, bulk_wasm_end},
		{"/spike/room.wasm", room_controller_wasm_start, room_controller_wasm_end},
	};
	for (auto &f : files) {
		size_t size = size_of(f.start, f.end);
		FILE *w = fopen(f.path, "wb");
		if (!w) {
			out("  cannot write %s", f.path);
			continue;
		}
		fwrite(f.start, 1, size, w);
		fclose(w);
		/* The blob store's read path: open, size, read into a heap buffer, close. */
		uint64_t t0 = now_us();
		FILE *r = fopen(f.path, "rb");
		struct stat st;
		stat(f.path, &st);
		uint8_t *buffer = static_cast<uint8_t *>(malloc(st.st_size));
		size_t got = fread(buffer, 1, st.st_size, r);
		fclose(r);
		uint64_t t1 = now_us();
		Block block;
		bool ok = got == size && memcmp(buffer, f.start, size) == 0
				&& block.load(buffer, static_cast<uint32_t>(size), false, kBinaryFreeable);
		if (kBinaryFreeable) {
			free(buffer); /* the module keeps what it needs */
			buffer = nullptr;
		}
		uint64_t t2 = now_us();
		ok = ok && block.instantiate(2048);
		uint64_t t3 = now_us();
		out("  %s %u B: read %" PRIu64 " us, load+validate %" PRIu64 " us, instantiate %" PRIu64 " us: %s",
				f.path, static_cast<unsigned>(size), t1 - t0, t2 - t1, t3 - t2, ok ? "ok" : block.error());
		block.unload();
		free(buffer);
	}
	esp_vfs_littlefs_unregister_partition(&part);
}

void *spike_task(void *)
{
	/* The USB Serial/JTAG console drops what is printed before the host has opened the port. */
	vTaskDelay(pdMS_TO_TICKS(4000));
	esp_task_wdt_add(nullptr);
	esp_reset_reason_t reason = esp_reset_reason();
	bool crashed = reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT;
	if (g_rtc_magic != kMagic) {
		g_rtc_magic = kMagic;
		g_rtc_skip = 0;
		g_rtc_stage = 0;
	}
	if (crashed && g_rtc_stage) {
		out("the previous boot crashed in stage %" PRIu32 " (reset reason %d): that stage is skipped from now on",
				g_rtc_stage, static_cast<int>(reason));
		g_rtc_skip |= 1u << g_rtc_stage;
	}
	g_rtc_stage = 0;

	Heap boot = heap();
	out("BEGIN WAMR 2.4.5 on %s, reset reason %d, heap free %u B, largest block %u B, minimum ever %u B", CONFIG_IDF_TARGET,
			static_cast<int>(reason), static_cast<unsigned>(boot.free), static_cast<unsigned>(boot.largest),
			static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)));
	out("  CPU %d MHz, exec heap (MALLOC_CAP_EXEC) free: %u B", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
			static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_EXEC)));

	g_ota0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
	if (!g_ota0) {
		out("no ota_0 partition");
		return nullptr;
	}
	if (enter(1, "runtime init")) {
		Heap h0 = heap();
		bool ok = runtime_init();
		out("  wasm_runtime_full_init: %s, heap %ld B (runtime %" PRId64 " B)", ok ? "ok" : "FAILED", used_since(h0), g_alloc.live);
		leave();
		if (!ok)
			return nullptr;
	}
	if (enter(2, "AOT files to flash, mapped for execution")) {
		if (!prepare_xip())
			out("  XIP preparation FAILED");
		leave();
	}
	if (enter(3, "loads"))
		stage_loads(), leave();
	if (enter(4, "replay (determinism)"))
		stage_replay(), leave();
	if (enter(5, "steady state"))
		stage_steady(), leave();
	if (enter(6, "benchmarks"))
		stage_bench(), leave();
	if (enter(7, "instruction budget"))
		stage_budget(), leave();
	if (enter(8, "faults"))
		stage_faults(), leave();
	if (enter(9, "instances"))
		stage_instances(), leave();
	if (enter(10, "image swap"))
		stage_swap(), leave();
	if (enter(11, "LittleFS"))
		stage_littlefs(), leave();
	UBaseType_t stack_free = uxTaskGetStackHighWaterMark(nullptr);
	Heap end = heap();
	out("END task stack 9216 B, never used: %u B; heap free %u B (at begin %u B), minimum ever %u B", static_cast<unsigned>(stack_free),
			static_cast<unsigned>(end.free), static_cast<unsigned>(boot.free),
			static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)));
	out("DONE");
	for (int beat = 0;; beat++) {
		esp_task_wdt_reset();
		vTaskDelay(pdMS_TO_TICKS(2000));
		out("heartbeat %d, uptime %" PRId64 " ms", beat, esp_timer_get_time() / 1000);
	}
}

} // namespace

extern "C" void app_main()
{
	/* The controller's event loop task: 9 KB stack, priority 5 (doc/core.md). WAMR's ESP-IDF port
	 * identifies the calling thread with pthread_self(), which asserts in a task that
	 * xTaskCreate() made: a thread that calls into WAMR must be a pthread. */
	esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
	cfg.stack_size = 9216;
	cfg.prio = 5;
	cfg.thread_name = "hc_loop";
	esp_pthread_set_cfg(&cfg);
	pthread_t thread;
	pthread_create(&thread, nullptr, spike_task, nullptr);
}
