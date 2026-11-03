/*
 * Copyright (c) 2026 Bernhard Haumacher
 */

/*
 * Probes of the spike (issue #59): faulty block calls (questions 3 and 4), each export misbehaving
 * in one way, which the host must stop with the reason while it keeps running; and the benchmarks
 * of question 5 (the cost of an import call and of a call into the module). Values the optimizer
 * must not see through come from hc_read(0), which the host answers with 0.
 */

#include "../../abi/hc_block.h"

HC_EXPORT("hc_abi") int32_t abi(void)
{
	return HC_ABI_VERSION;
}

/* An endless loop: bounded execution must terminate it. */
HC_EXPORT("loop_forever") int32_t loop_forever(void)
{
	volatile int32_t n = 0;
	for (;;)
		n++;
	return n;
}

/* A load far outside the linear memory. */
HC_EXPORT("oob") int32_t oob(void)
{
	volatile int32_t *p = (volatile int32_t *)(uint32_t)(0x7ffffff0u + (uint32_t)hc_read(0));
	return *p;
}

/* Recursion without a shadow-stack frame: overflows the runtime's operand and frame stack. */
static int32_t deep(int32_t n)
{
	if (n == 0)
		return 0;
	return deep(n - 1) + 1 + (int32_t)hc_read(0);
}

HC_EXPORT("stack_overflow") int32_t stack_overflow(void)
{
	return deep(1000000000);
}

/* Recursion with a frame in linear memory (an address-taken array): overflows the C stack that
 * lives in the module's memory (the shadow stack, below the data with --stack-first). */
static int32_t deep_frame(int32_t n)
{
	volatile int32_t frame[256];
	frame[n & 255] = n;
	if (n == 0)
		return frame[0];
	return deep_frame(n - 1) + frame[n & 255];
}

HC_EXPORT("shadow_stack_overflow") int32_t shadow_stack_overflow(void)
{
	return deep_frame(1000000000);
}

HC_EXPORT("unreachable") void unreachable(void)
{
	__builtin_unreachable();
}

HC_EXPORT("trap") void trap(void)
{
	__builtin_trap();
}

/* Integer division by zero traps in WebAssembly (floating-point division does not). */
HC_EXPORT("div_zero") int32_t div_zero(void)
{
	int32_t zero = (int32_t)hc_read(0);
	return 100 / zero;
}

/* A well-behaved call, to show the instance (or a sibling) still runs; also the empty call of
 * the module-call benchmark. */
HC_EXPORT("ok") int32_t ok(void)
{
	return 42;
}

/* The C stack pointer as the block sees it: the address of a local in its frame. A trap skips the
 * epilogue that restores the stack pointer global, so this shows whether a trap leaks C stack
 * for the next call into the same instance. */
__attribute__((noinline)) static int32_t address_of(volatile int32_t *p)
{
	return (int32_t)(uint32_t)p;
}

HC_EXPORT("stack_pointer") int32_t stack_pointer(void)
{
	volatile int32_t local = 0;
	return address_of(&local);
}

/* Benchmarks: n import calls each, so the host divides the time by n. */
HC_EXPORT("bench_now") int64_t bench_now(int32_t n)
{
	int64_t sum = 0;
	for (int32_t i = 0; i < n; i++)
		sum += hc_now();
	return sum;
}

HC_EXPORT("bench_read") double bench_read(int32_t n)
{
	double sum = 0.0;
	for (int32_t i = 0; i < n; i++)
		sum += hc_read(i & 3);
	return sum;
}

HC_EXPORT("bench_write") void bench_write(int32_t n)
{
	for (int32_t i = 0; i < n; i++)
		hc_write(0, (double)i);
}

HC_EXPORT("bench_log") void bench_log(int32_t n)
{
	for (int32_t i = 0; i < n; i++)
		hc_log("x");
}

/* A loop without import calls: the price of the loop itself (and the interpreter's speed). */
HC_EXPORT("bench_loop") int32_t bench_loop(int32_t n)
{
	volatile int32_t sum = 0;
	for (int32_t i = 0; i < n; i++)
		sum += i;
	return sum;
}

/* Double arithmetic as in the controller (the C6 has no FPU: soft float in every runtime). */
HC_EXPORT("bench_float") double bench_float(int32_t n)
{
	double x = 20.0 + hc_read(0), y = 0.0;
	for (int32_t i = 0; i < n; i++) {
		y += (x - y) * 0.01;
		x = x * 1.0000001 + 0.5 / (1.0 + y);
	}
	return x + y;
}
