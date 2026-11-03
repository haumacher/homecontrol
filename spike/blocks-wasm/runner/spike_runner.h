/*
 * Copyright (c) 2026 Bernhard Haumacher
 */

/*
 * The spike's block host on WAMR (issue #59, doc/blocks-wasm.md), shared by the host test
 * (host/host_test.cpp) and the ESP32-C6 measurement app (esp/main/spike_main.cpp). Not core code:
 * it is what a block host on WAMR needs, small enough to measure what each step costs.
 *
 * - runtime_init(): WAMR with a counting allocator (every malloc/realloc/free of the runtime is
 *   counted, so the steady state can be checked for allocations) and the "hc" imports.
 * - Block: one loaded module, one instance of it, one execution environment, reused for every
 *   call. A call has an instruction budget (WAMR's instruction metering, interpreter only); a call
 *   that traps or exceeds it stops the block with the reason, as the contract (#60) wants.
 * - replay(): runs the recorded input series (fixtures/series.csv) through the room controller
 *   and hashes the bits of every output and of the state, for the determinism check.
 */

#ifndef SPIKE_RUNNER_H
#define SPIKE_RUNNER_H

#include <stddef.h>
#include <stdint.h>

#include "wasm_export.h"

namespace spike {

/* Provided by the platform's main: a microsecond clock for measurements. */
uint64_t now_us();
/* Provided by the platform's main: one line of output. */
void out(const char *format, ...) __attribute__((format(printf, 1, 2)));

struct AllocStats {
	uint32_t mallocs;
	uint32_t reallocs;
	uint32_t frees;
	int64_t live;
	int64_t peak;
};

extern AllocStats g_alloc;

/* Initializes WAMR with the counting allocator and registers the "hc" imports. */
bool runtime_init();
void runtime_destroy();

constexpr int kPorts = 4;

/* What a block sees through its imports, and what it wrote. */
struct Ports {
	double in[kPorts];
	int32_t quality[kPorts];
	double param[kPorts];
	double out[kPorts];
	int64_t now_ms;
	uint32_t reads;
	uint32_t writes;
	uint32_t logs;
	bool echo_log;
};

class Block {
public:
	/* Loads a module (.wasm or .aot). WAMR's loader rewrites the bytes of a .wasm in place (a
	 * second load of the same buffer fails) and keeps pointers into them (an AOT file in XIP mode
	 * runs its code from it), so the bytes must live until unload(), unless `freeable`: then the
	 * loader copies what it keeps and the caller may free the bytes at once (interpreter only).
	 * copy: load from a private heap copy that unload() frees (the bytes stay untouched). */
	bool load(uint8_t *bytes, uint32_t size, bool copy = false, bool freeable = false);
	/* Instantiates with the given interpreter stack (operand stack and frames) in bytes. */
	bool instantiate(uint32_t stack_size);
	/* Calls an export. instruction_limit < 0: no budget. On a trap or an exhausted budget the
	 * block is stopped: the reason is kept and every later call is refused. */
	bool call(const char *name, uint32_t argc, uint32_t *argv, int32_t instruction_limit = -1);
	/* Calls through a function handle looked up once (the steady-state path). */
	bool call(wasm_function_inst_t function, uint32_t argc, uint32_t *argv, int32_t instruction_limit = -1);
	wasm_function_inst_t lookup(const char *name);
	/* Native address of a linear-memory address of the module, or null. */
	void *native(uint32_t app_address, uint32_t size);
	uint32_t memory_size() const;
	void restart(); /* clears the stop and the exception: the contract's "restart" command */
	void deinstantiate();
	void unload();

	bool stopped() const { return stopped_; }
	const char *reason() const { return reason_; }
	const char *error() const { return error_; }

	Ports ports {};
	wasm_module_t module = nullptr;
	wasm_module_inst_t instance = nullptr;
	wasm_exec_env_t env = nullptr;

private:
	uint8_t *copy_ = nullptr;
	bool stopped_ = false;
	char reason_[128] {};
	char error_[128] {};
};

/* The room controller's parameters for the series: xp 2 K, tn 90 min, filter 5 min, 30 % fallback. */
void room_parameters(Ports &ports);

struct ReplayResult {
	bool ok;
	uint32_t rows;
	uint64_t hash;          /* FNV-1a 64 over t, valve, integral, filtered bits of every row */
	uint64_t total_us;      /* time inside the block calls */
	uint64_t max_call_us;
	char reason[128];
};

/* Replays a series (the text of series.csv) through instance `self` of a room controller block.
 * row(): optional, called with every row's output bits (for expected.csv and its comparison). */
ReplayResult replay(Block &block, const char *csv, size_t length, int32_t self,
		void (*row)(void *context, int64_t t, uint64_t valve, uint64_t integral, uint64_t filtered),
		void *context);

} // namespace spike

#endif
