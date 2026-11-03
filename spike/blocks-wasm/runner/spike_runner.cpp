/*
 * Copyright (c) 2026 Bernhard Haumacher
 */

#include "spike_runner.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace spike {

AllocStats g_alloc {};

namespace {

/* ---- The counting allocator: every allocation of the runtime goes through it. ---- */

constexpr size_t kHeader = 8;

void *count_malloc(unsigned int size)
{
	uint8_t *p = static_cast<uint8_t *>(malloc(size + kHeader));
	if (!p)
		return nullptr;
	memcpy(p, &size, sizeof size);
	g_alloc.mallocs++;
	g_alloc.live += size;
	if (g_alloc.live > g_alloc.peak)
		g_alloc.peak = g_alloc.live;
	return p + kHeader;
}

void count_free(void *ptr)
{
	if (!ptr)
		return;
	uint8_t *p = static_cast<uint8_t *>(ptr) - kHeader;
	unsigned int size;
	memcpy(&size, p, sizeof size);
	g_alloc.frees++;
	g_alloc.live -= size;
	free(p);
}

void *count_realloc(void *ptr, unsigned int size)
{
	if (!ptr)
		return count_malloc(size);
	uint8_t *p = static_cast<uint8_t *>(ptr) - kHeader;
	unsigned int old;
	memcpy(&old, p, sizeof old);
	uint8_t *q = static_cast<uint8_t *>(realloc(p, size + kHeader));
	if (!q)
		return nullptr;
	memcpy(q, &size, sizeof size);
	g_alloc.reallocs++;
	g_alloc.live += static_cast<int64_t>(size) - old;
	if (g_alloc.live > g_alloc.peak)
		g_alloc.peak = g_alloc.live;
	return q + kHeader;
}

/* ---- The "hc" imports of the draft ABI (abi/hc_block.h). ---- */

Ports &ports_of(wasm_exec_env_t env)
{
	return static_cast<Block *>(wasm_runtime_get_user_data(env))->ports;
}

bool valid_port(int32_t port)
{
	return port >= 0 && port < kPorts;
}

int64_t hc_now(wasm_exec_env_t env)
{
	return ports_of(env).now_ms;
}

double hc_read(wasm_exec_env_t env, int32_t port)
{
	Ports &p = ports_of(env);
	p.reads++;
	return valid_port(port) ? p.in[port] : NAN;
}

int32_t hc_quality(wasm_exec_env_t env, int32_t port)
{
	Ports &p = ports_of(env);
	return valid_port(port) ? p.quality[port] : 2;
}

double hc_param(wasm_exec_env_t env, int32_t id)
{
	Ports &p = ports_of(env);
	return valid_port(id) ? p.param[id] : NAN;
}

void hc_write(wasm_exec_env_t env, int32_t port, double value)
{
	Ports &p = ports_of(env);
	p.writes++;
	if (valid_port(port))
		p.out[port] = value;
}

/* "(*~)": WAMR checks that [text, text + length) lies inside the module's memory before the
 * call, so the host never reads outside it. */
void hc_log(wasm_exec_env_t env, const char *text, int32_t length)
{
	Ports &p = ports_of(env);
	p.logs++;
	if (p.echo_log)
		out("block log: %.*s", static_cast<int>(length), text);
}

NativeSymbol g_natives[] = {
	{"now", reinterpret_cast<void *>(hc_now), "()I", nullptr},
	{"read", reinterpret_cast<void *>(hc_read), "(i)F", nullptr},
	{"quality", reinterpret_cast<void *>(hc_quality), "(i)i", nullptr},
	{"param", reinterpret_cast<void *>(hc_param), "(i)F", nullptr},
	{"write", reinterpret_cast<void *>(hc_write), "(iF)", nullptr},
	{"log", reinterpret_cast<void *>(hc_log), "(*~)", nullptr},
};

} // namespace

bool runtime_init()
{
	RuntimeInitArgs args;
	memset(&args, 0, sizeof args);
	args.mem_alloc_type = Alloc_With_Allocator;
	args.mem_alloc_option.allocator.malloc_func = reinterpret_cast<void *>(count_malloc);
	args.mem_alloc_option.allocator.realloc_func = reinterpret_cast<void *>(count_realloc);
	args.mem_alloc_option.allocator.free_func = reinterpret_cast<void *>(count_free);
	if (!wasm_runtime_full_init(&args)) {
		out("wasm_runtime_full_init failed");
		return false;
	}
	if (!wasm_runtime_register_natives("hc", g_natives, sizeof g_natives / sizeof g_natives[0])) {
		out("registering the hc imports failed");
		return false;
	}
	return true;
}

void runtime_destroy()
{
	wasm_runtime_destroy();
}

bool Block::load(uint8_t *bytes, uint32_t size, bool copy, bool freeable)
{
	error_[0] = 0;
	if (copy) {
		copy_ = static_cast<uint8_t *>(malloc(size));
		if (!copy_) {
			snprintf(error_, sizeof error_, "no memory for a copy of %u bytes", static_cast<unsigned>(size));
			return false;
		}
		memcpy(copy_, bytes, size);
		bytes = copy_;
	}
	LoadArgs args;
	memset(&args, 0, sizeof args);
	args.wasm_binary_freeable = freeable;
	module = wasm_runtime_load_ex(bytes, size, &args, error_, sizeof error_);
	return module != nullptr;
}

bool Block::instantiate(uint32_t stack_size)
{
	error_[0] = 0;
	stopped_ = false;
	reason_[0] = 0;
	/* No app heap: blocks do not allocate from the host (heap_size 0). */
	instance = wasm_runtime_instantiate(module, stack_size, 0, error_, sizeof error_);
	if (!instance)
		return false;
	env = wasm_runtime_create_exec_env(instance, stack_size);
	if (!env) {
		snprintf(error_, sizeof error_, "cannot create the execution environment");
		return false;
	}
	wasm_runtime_set_user_data(env, this);
	return true;
}

wasm_function_inst_t Block::lookup(const char *name)
{
	return wasm_runtime_lookup_function(instance, name);
}

bool Block::call(const char *name, uint32_t argc, uint32_t *argv, int32_t instruction_limit)
{
	wasm_function_inst_t function = lookup(name);
	if (!function) {
		snprintf(error_, sizeof error_, "no export %s", name);
		return false;
	}
	return call(function, argc, argv, instruction_limit);
}

bool Block::call(wasm_function_inst_t function, uint32_t argc, uint32_t *argv, int32_t instruction_limit)
{
	if (stopped_)
		return false;
#if WASM_ENABLE_INSTRUCTION_METERING != 0
	wasm_runtime_set_instruction_count_limit(env, instruction_limit);
#else
	(void)instruction_limit;
#endif
	if (wasm_runtime_call_wasm(env, function, argc, argv))
		return true;
	const char *exception = wasm_runtime_get_exception(instance);
	snprintf(reason_, sizeof reason_, "%s", exception ? exception : "call failed");
	stopped_ = true;
	return false;
}

void *Block::native(uint32_t app_address, uint32_t size)
{
	if (!wasm_runtime_validate_app_addr(instance, app_address, size))
		return nullptr;
	return wasm_runtime_addr_app_to_native(instance, app_address);
}

uint32_t Block::memory_size() const
{
	wasm_memory_inst_t memory = wasm_runtime_get_default_memory(instance);
	if (!memory)
		return 0;
	return static_cast<uint32_t>(wasm_memory_get_bytes_per_page(memory) * wasm_memory_get_cur_page_count(memory));
}

void Block::restart()
{
	wasm_runtime_clear_exception(instance);
	stopped_ = false;
	reason_[0] = 0;
}

void Block::deinstantiate()
{
	if (env)
		wasm_runtime_destroy_exec_env(env);
	env = nullptr;
	if (instance)
		wasm_runtime_deinstantiate(instance);
	instance = nullptr;
}

void Block::unload()
{
	deinstantiate();
	if (module)
		wasm_runtime_unload(module);
	module = nullptr;
	free(copy_);
	copy_ = nullptr;
}

void room_parameters(Ports &ports)
{
	ports.param[0] = 2.0;  /* xp, K */
	ports.param[1] = 90.0; /* tn, min */
	ports.param[2] = 5.0;  /* filter, min */
	ports.param[3] = 30.0; /* fallback, % */
}

namespace {

uint64_t fnv(uint64_t hash, uint64_t value)
{
	for (int i = 0; i < 8; i++) {
		hash ^= (value >> (8 * i)) & 0xff;
		hash *= 0x100000001b3ull;
	}
	return hash;
}

uint64_t bits(double v)
{
	uint64_t b;
	memcpy(&b, &v, sizeof b);
	return b;
}

/* One field of a CSV line; advances *p past it and the comma. */
const char *field(const char **p, const char *end, char *buffer, size_t size)
{
	size_t n = 0;
	while (*p < end && **p != ',' && **p != '\n' && **p != '\r') {
		if (n + 1 < size)
			buffer[n++] = **p;
		(*p)++;
	}
	buffer[n] = 0;
	if (*p < end && **p == ',')
		(*p)++;
	return buffer;
}

double number(const char *text)
{
	if (text[0] == 'n')
		return NAN;
	return strtod(text, nullptr);
}

} // namespace

ReplayResult replay(Block &block, const char *csv, size_t length, int32_t self,
		void (*row)(void *, int64_t, uint64_t, uint64_t, uint64_t), void *context)
{
	ReplayResult result {};
	result.hash = 0xcbf29ce484222325ull;
	const char *p = csv, *end = csv + length;
	while (p < end && *p != '\n') /* the header line */
		p++;

	wasm_function_inst_t f_input = block.lookup("hc_input");
	wasm_function_inst_t f_period = block.lookup("hc_period");
	uint32_t argv[3] = {static_cast<uint32_t>(self), 0, 0};
	if (!f_input || !f_period || !block.call("hc_state", 1, argv)) {
		snprintf(result.reason, sizeof result.reason, "not a room controller block");
		return result;
	}
	const uint8_t *state = static_cast<const uint8_t *>(block.native(argv[0], 32));
	if (!state) {
		snprintf(result.reason, sizeof result.reason, "state outside the memory");
		return result;
	}

	Ports &ports = block.ports;
	room_parameters(ports);
	char buffer[32];
	bool started = false;
	while (p < end) {
		while (p < end && (*p == '\n' || *p == '\r'))
			p++;
		if (p >= end || *p == 0) /* an embedded text file ends in a NUL */
			break;
		int64_t t = strtoll(field(&p, end, buffer, sizeof buffer), nullptr, 10);
		char call[4];
		field(&p, end, call, sizeof call);
		ports.in[0] = number(field(&p, end, buffer, sizeof buffer));
		ports.quality[0] = static_cast<int32_t>(strtol(field(&p, end, buffer, sizeof buffer), nullptr, 10));
		ports.in[1] = number(field(&p, end, buffer, sizeof buffer));
		ports.in[2] = number(field(&p, end, buffer, sizeof buffer));
		ports.quality[1] = ports.quality[2] = 0;
		ports.now_ms = t;

		uint64_t begin = now_us();
		bool ok;
		if (!started) {
			uint32_t start_argv[3] = {static_cast<uint32_t>(self), 0, 0};
			ok = block.call("hc_start", 3, start_argv, 100000);
			started = true;
			if (!ok)
				break;
		}
		if (call[0] == 'p') {
			argv[0] = static_cast<uint32_t>(self);
			ok = block.call(f_period, 1, argv, 100000);
		} else {
			argv[0] = static_cast<uint32_t>(self);
			argv[1] = static_cast<uint32_t>(strtol(call, nullptr, 10));
			ok = block.call(f_input, 2, argv, 100000);
		}
		uint64_t spent = now_us() - begin;
		result.total_us += spent;
		if (spent > result.max_call_us)
			result.max_call_us = spent;
		if (!ok)
			break;

		double integral, filtered;
		memcpy(&integral, state, 8);
		memcpy(&filtered, state + 8, 8);
		uint64_t valve_bits = bits(ports.out[0]);
		result.hash = fnv(result.hash, static_cast<uint64_t>(t));
		result.hash = fnv(result.hash, valve_bits);
		result.hash = fnv(result.hash, bits(integral));
		result.hash = fnv(result.hash, bits(filtered));
		result.rows++;
		if (row)
			row(context, t, valve_bits, bits(integral), bits(filtered));
	}
	if (block.stopped())
		snprintf(result.reason, sizeof result.reason, "%s", block.reason());
	else
		result.ok = true;
	return result;
}

} // namespace spike
