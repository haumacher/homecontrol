/*
 * Copyright (c) 2026 Bernhard Haumacher
 */

/*
 * DRAFT block ABI of the WebAssembly spike (issue #59, doc/blocks-wasm.md). Not the contract: the
 * block contract is issue #60 (spec/blocks.md); this header is what the spike's test blocks were
 * written against, to measure the runtime. Freestanding C: no libc, no WASI.
 *
 * A module is one block *type*. It may hold several block *instances* in its one linear memory:
 * every export takes the instance number `self` (0..hc_instances()-1), and every import refers to
 * the instance the host is currently calling. The host never calls two instances at once.
 *
 * Imports (module "hc"):
 *   i64 now()                    monotonic milliseconds of the controller (doc/core.md)
 *   f64 read(i32 port)           value of an input port of the current instance (NaN: no value)
 *   i32 quality(i32 port)        HC_GOOD, HC_STALE or HC_UNKNOWN
 *   f64 param(i32 id)            value of a parameter of the current instance
 *   void write(i32 port, f64 v)  commands an output port of the current instance
 *   void log(i32 ptr, i32 len)   one UTF-8 log line from linear memory
 *
 * Exports:
 *   memory                       the linear memory (no memory.grow: the host shrinks it)
 *   i32 hc_abi()                 HC_ABI_VERSION
 *   i32 hc_instances()           how many instances this module can hold
 *   i32 hc_state(i32 self)       address of the instance's persistent state in linear memory
 *   i32 hc_state_size()          its size in bytes (the host saves and restores the bytes)
 *   void hc_start(i32 self, i32 reason, i32 restored)
 *   void hc_input(i32 self, i32 port)
 *   void hc_period(i32 self)
 */

#ifndef HC_BLOCK_H
#define HC_BLOCK_H

#define HC_ABI_VERSION 1

#define HC_GOOD 0
#define HC_STALE 1
#define HC_UNKNOWN 2

#define HC_START_BOOT 0
#define HC_START_IMAGE 1
#define HC_START_RESTART 2

typedef int int32_t;
typedef long long int64_t;
typedef unsigned int uint32_t;

#define HC_IMPORT(name) __attribute__((import_module("hc"), import_name(name)))
#define HC_EXPORT(name) __attribute__((export_name(name)))

HC_IMPORT("now") int64_t hc_now(void);
HC_IMPORT("read") double hc_read(int32_t port);
HC_IMPORT("quality") int32_t hc_quality(int32_t port);
HC_IMPORT("param") double hc_param(int32_t id);
HC_IMPORT("write") void hc_write(int32_t port, double value);
HC_IMPORT("log") void hc_log_raw(const char *text, int32_t length);

static inline void hc_log(const char *text)
{
	int32_t n = 0;
	while (text[n])
		n++;
	hc_log_raw(text, n);
}

#endif
