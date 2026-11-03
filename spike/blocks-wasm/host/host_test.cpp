/*
 * Copyright (c) 2026 Bernhard Haumacher
 */

/*
 * The host test of the WebAssembly block spike (issue #59, doc/blocks-wasm.md).
 *
 *   blocks_wasm_host_test <spike/blocks-wasm> [--write-expected]
 *
 * Checks, each a line PASS/FAIL (exit code 1 if any fails):
 * - the C room controller replays fixtures/series.csv to exactly the bits of fixtures/expected.csv
 *   (the same file the Java test and the device compare against);
 * - the Rust and Zig builds of the block and the x86-64 AOT build of the C block give the same bits;
 * - several instances in one module instance keep their state apart;
 * - no allocation by the runtime per call in steady state;
 * - every fault probe (endless loop, out-of-bounds, both stack overflows, unreachable, trap,
 *   division by zero) stops its block with a reason, the host continues, and nothing leaks;
 * - 200 load/instantiate/unload cycles (an image swap each) leak nothing.
 * Measurements (memory per step, call costs) are printed, not checked.
 */

#include "spike_runner.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>
#include <vector>

namespace spike {

uint64_t now_us()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<uint64_t>(ts.tv_sec) * 1000000u + static_cast<uint64_t>(ts.tv_nsec) / 1000u;
}

void out(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
	putchar('\n');
}

} // namespace spike

using namespace spike;

namespace {

int g_failures = 0;
std::string g_dir;

void check(bool ok, const char *what)
{
	out("%s %s", ok ? "PASS" : "FAIL", what);
	if (!ok)
		g_failures++;
}

std::vector<uint8_t> read_file(const std::string &name)
{
	std::vector<uint8_t> data;
	FILE *f = fopen((g_dir + "/" + name).c_str(), "rb");
	if (!f) {
		out("cannot read %s", name.c_str());
		exit(2);
	}
	uint8_t buffer[4096];
	size_t n;
	while ((n = fread(buffer, 1, sizeof buffer, f)) > 0)
		data.insert(data.end(), buffer, buffer + n);
	fclose(f);
	return data;
}

struct Rows {
	std::string text;
};

void collect(void *context, int64_t t, uint64_t valve, uint64_t integral, uint64_t filtered)
{
	char line[96];
	snprintf(line, sizeof line, "%lld,%016llx,%016llx,%016llx\n", static_cast<long long>(t),
			static_cast<unsigned long long>(valve), static_cast<unsigned long long>(integral),
			static_cast<unsigned long long>(filtered));
	static_cast<Rows *>(context)->text += line;
}

/* Loads and instantiates; the bytes stay owned by the caller's vector. */
bool open_block(Block &block, std::vector<uint8_t> &bytes, uint32_t stack = 2048)
{
	if (!block.load(bytes.data(), static_cast<uint32_t>(bytes.size()), true)) {
		out("load failed: %s", block.error());
		return false;
	}
	if (!block.instantiate(stack)) {
		out("instantiate failed: %s", block.error());
		return false;
	}
	return true;
}

ReplayResult replay_file(const char *name, Rows *rows, int32_t self = 0)
{
	std::vector<uint8_t> bytes = read_file(name);
	std::vector<uint8_t> csv = read_file("fixtures/series.csv");
	Block block;
	ReplayResult r {};
	if (!open_block(block, bytes))
		return r;
	r = replay(block, reinterpret_cast<const char *>(csv.data()), csv.size(), self, rows ? collect : nullptr, rows);
	out("  %s: %u rows, hash %016llx, %llu us in calls, longest call %llu us%s%s", name, r.rows,
			static_cast<unsigned long long>(r.hash), static_cast<unsigned long long>(r.total_us),
			static_cast<unsigned long long>(r.max_call_us), r.ok ? "" : ", stopped: ", r.reason);
	block.unload();
	return r;
}

void test_determinism(bool write_expected)
{
	Rows c_rows;
	ReplayResult c = replay_file("wasm/room_controller.wasm", &c_rows);
	check(c.ok && c.rows > 2000, "C block replays the series");
	if (write_expected) {
		FILE *f = fopen((g_dir + "/fixtures/expected.csv").c_str(), "w");
		fputs("t,valve,integral,filtered\n", f);
		fputs(c_rows.text.c_str(), f);
		fclose(f);
		out("  wrote fixtures/expected.csv");
	}
	std::vector<uint8_t> expected = read_file("fixtures/expected.csv");
	std::string text(reinterpret_cast<const char *>(expected.data()), expected.size());
	check(text == "t,valve,integral,filtered\n" + c_rows.text, "C block: every output and state bit equals fixtures/expected.csv");

	ReplayResult rs = replay_file("wasm/room_controller_rs.wasm", nullptr);
	check(rs.ok && rs.hash == c.hash, "Rust block gives the same bits as the C block");
	ReplayResult zig = replay_file("wasm/room_controller_zig.wasm", nullptr);
	check(zig.ok && zig.hash == c.hash, "Zig block gives the same bits as the C block");
	ReplayResult aot = replay_file("wasm/room_controller.x86_64.aot", nullptr);
	check(aot.ok && aot.hash == c.hash, "C block as x86-64 AOT gives the same bits as interpreted");

	/* The fast interpreter compiles the code into buffers of its own: with wasm_binary_freeable
	 * the binary can be freed right after the load (overwritten here, to be sure nothing reads it). */
	{
		std::vector<uint8_t> bin = read_file("wasm/room_controller.wasm");
		std::vector<uint8_t> series = read_file("fixtures/series.csv");
		Block block;
		bool ok = block.load(bin.data(), static_cast<uint32_t>(bin.size()), false, true);
		memset(bin.data(), 0xff, bin.size());
		ok = ok && block.instantiate(2048);
		ReplayResult r = replay(block, reinterpret_cast<const char *>(series.data()), series.size(), 0, nullptr, nullptr);
		check(ok && r.ok && r.hash == c.hash, "fast interpreter: the binary may be discarded after the load");
		block.unload();
	}

	/* Several instances in one module instance: instance 5 after instance 0, and instance 0's
	 * state untouched by it. */
	std::vector<uint8_t> bytes = read_file("wasm/room_controller.wasm");
	std::vector<uint8_t> csv = read_file("fixtures/series.csv");
	Block block;
	if (open_block(block, bytes)) {
		ReplayResult r0 = replay(block, reinterpret_cast<const char *>(csv.data()), csv.size(), 0, nullptr, nullptr);
		uint32_t argv[1] = {0};
		block.call("hc_state", 1, argv);
		uint8_t before[32];
		memcpy(before, block.native(argv[0], 32), 32);
		ReplayResult r5 = replay(block, reinterpret_cast<const char *>(csv.data()), csv.size(), 5, nullptr, nullptr);
		argv[0] = 0;
		block.call("hc_state", 1, argv);
		check(r0.hash == c.hash && r5.hash == c.hash && memcmp(before, block.native(argv[0], 32), 32) == 0,
				"8 instances in one module instance keep their states apart");
		out("  module memory after shrinking: %u bytes (data + 4 KB stack), instead of 65536", block.memory_size());

		/* Steady state: no allocation per call. */
		AllocStats a = g_alloc;
		wasm_function_inst_t f_period = block.lookup("hc_period");
		uint64_t t0 = now_us();
		for (int i = 0; i < 10000; i++) {
			block.ports.now_ms += 1000;
			uint32_t pv[1] = {0};
			block.call(f_period, 1, pv, 100000);
		}
		uint64_t t1 = now_us();
		check(g_alloc.mallocs == a.mallocs && g_alloc.reallocs == a.reallocs && g_alloc.frees == a.frees,
				"10000 block calls in steady state allocate nothing");
		out("  PI step (call + 7 imports): %.0f ns per call", (t1 - t0) * 1000.0 / 10000);
		block.unload();
	}
}

struct Fault {
	const char *name;
	const char *expect;
};

void test_faults()
{
	static const Fault faults[] = {
		{"loop_forever", "instruction limit exceeded"},
		{"oob", "out of bounds memory access"},
		{"stack_overflow", "wasm operand stack overflow"},
		{"shadow_stack_overflow", "auxiliary stack overflow"},
		{"unreachable", "unreachable"},
		{"trap", "unreachable"},
		{"div_zero", "integer divide by zero"},
	};
	std::vector<uint8_t> probes = read_file("wasm/probes.wasm");
	std::vector<uint8_t> room = read_file("wasm/room_controller.wasm");
	int64_t baseline = g_alloc.live;
	Block sibling;
	open_block(sibling, room);
	room_parameters(sibling.ports);
	uint32_t sv[3] = {0, 0, 0};
	sibling.call("hc_start", 3, sv, 100000);

	for (const Fault &fault : faults) {
		Block block;
		if (!open_block(block, probes))
			continue;
		uint32_t argv[2] = {0, 0};
		block.call("stack_pointer", 0, argv);
		uint32_t sp_before = argv[0];
		uint64_t t0 = now_us();
		bool ok = block.call(fault.name, 0, argv, 10 * 1000 * 1000);
		uint64_t spent = now_us() - t0;
		bool refused = !block.call("ok", 0, argv);
		char what[192];
		snprintf(what, sizeof what, "%s: stopped with \"%s\" after %llu us, later calls refused", fault.name,
				block.reason(), static_cast<unsigned long long>(spent));
		check(!ok && block.stopped() && strstr(block.reason(), fault.expect) && refused, what);

		/* The restart command: the same instance runs again after clearing the exception. */
		block.restart();
		argv[0] = 0;
		bool runs = block.call("ok", 0, argv) && argv[0] == 42;
		block.call("stack_pointer", 0, argv);
		out("  after restart: ok() %s, C stack pointer %u -> %u%s", runs ? "runs" : "FAILS", sp_before, argv[0],
				argv[0] != sp_before ? " (the trap leaked C stack: restart must reinstantiate)" : "");

		/* The sibling block keeps running. */
		sibling.ports.now_ms += 60000;
		sibling.ports.in[0] = 20.0;
		sibling.ports.in[1] = 21.0;
		sibling.ports.in[2] = 1.0;
		uint32_t pv[1] = {0};
		check(sibling.call("hc_period", 1, pv, 100000), "  the room controller beside it still runs");
		block.unload();
	}
	sibling.unload();
	check(g_alloc.live == baseline, "no memory left behind by the faulted blocks");
}

void test_swap()
{
	std::vector<uint8_t> room = read_file("wasm/room_controller.wasm");
	int64_t baseline = g_alloc.live;
	AllocStats a = g_alloc;
	for (int i = 0; i < 200; i++) {
		Block block;
		if (!open_block(block, room)) {
			check(false, "load in a swap cycle");
			return;
		}
		uint32_t argv[3] = {0, 0, 0};
		block.call("hc_start", 3, argv);
		block.unload();
	}
	check(g_alloc.live == baseline, "200 load/instantiate/unload cycles leak nothing");
	out("  per cycle: %u mallocs, %u reallocs", (g_alloc.mallocs - a.mallocs) / 200, (g_alloc.reallocs - a.reallocs) / 200);
}

void measure_memory(const char *name)
{
	std::vector<uint8_t> bytes = read_file(name);
	int64_t base = g_alloc.live;
	Block block;
	uint64_t t0 = now_us();
	if (!block.load(bytes.data(), static_cast<uint32_t>(bytes.size()))) {
		out("load %s: %s", name, block.error());
		return;
	}
	uint64_t t1 = now_us();
	int64_t loaded = g_alloc.live;
	block.instantiate(2048);
	uint64_t t2 = now_us();
	out("  %s (%zu bytes): load %llu us, %lld bytes; instance + exec env (2 KB stack) %llu us, %lld bytes, "
			"linear memory %u bytes (not counted: allocated by the platform's mmap)",
			name, bytes.size(), static_cast<unsigned long long>(t1 - t0), static_cast<long long>(loaded - base),
			static_cast<unsigned long long>(t2 - t1), static_cast<long long>(g_alloc.live - loaded), block.memory_size());
	block.unload();
}

void measure_calls()
{
	std::vector<uint8_t> probes = read_file("wasm/probes.wasm");
	Block block;
	if (!open_block(block, probes))
		return;
	const int n = 100000;
	const char *benches[] = {"bench_loop", "bench_now", "bench_read", "bench_write", "bench_log"};
	double loop_ns = 0;
	for (const char *b : benches) {
		uint32_t argv[2] = {n, 0};
		uint64_t t0 = now_us();
		block.call(b, 1, argv);
		double ns = (now_us() - t0) * 1000.0 / n;
		if (!strcmp(b, "bench_loop"))
			loop_ns = ns;
		out("  %s: %.1f ns per iteration%s", b, ns, strcmp(b, "bench_loop") ? "" : " (loop alone)");
		(void)loop_ns;
	}
	wasm_function_inst_t ok = block.lookup("ok");
	uint64_t t0 = now_us();
	for (int i = 0; i < n; i++) {
		uint32_t argv[1] = {0};
		block.call(ok, 0, argv, 1000);
	}
	out("  call into the module (empty export): %.1f ns", (now_us() - t0) * 1000.0 / n);
	block.unload();
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <spike/blocks-wasm> [--write-expected]\n", argv[0]);
		return 2;
	}
	setvbuf(stdout, nullptr, _IOLBF, 0);
	g_dir = argv[1];
	bool write_expected = argc > 2 && !strcmp(argv[2], "--write-expected");
	if (!runtime_init())
		return 2;
	out("WAMR %s, fast interpreter + AOT, instruction metering, software bounds checks", "2.4.5");
	test_determinism(write_expected);
	test_faults();
	test_swap();
	out("Memory (counted allocations of the runtime):");
	measure_memory("wasm/room_controller.wasm");
	measure_memory("wasm/room_controller.x86_64.aot");
	measure_memory("wasm/probes.wasm");
	out("Call costs on this host:");
	measure_calls();
	runtime_destroy();
	out("%d failure(s)", g_failures);
	return g_failures ? 1 : 0;
}
