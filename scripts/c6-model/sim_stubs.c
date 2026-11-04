/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// What the ESP-IDF's newlib leaves to the IDF and newlib's `sim` system calls do not provide under
// qemu-riscv32 user mode: the reentrancy structure, a heap (sim's sbrk does not grow the mapping),
// open (sim uses the old open system call, qemu knows openat) and two stubs.
#include <errno.h>
#include <reent.h>
#include <stddef.h>
#include <string.h>

static struct _reent g_reent;
static int g_reent_ready;

struct _reent* __getreent(void) {
	if (!g_reent_ready) {
		_REENT_INIT_PTR(&g_reent);
		g_reent_ready = 1;
	}
	return &g_reent;
}

static char g_heap[64 << 20] __attribute__((aligned(16)));
static size_t g_brk;

void* _sbrk(ptrdiff_t increment) {
	if (increment < 0 || g_brk + (size_t)increment > sizeof(g_heap)) {
		errno = ENOMEM;
		return (void*)-1;
	}
	void* p = g_heap + g_brk;
	g_brk += (size_t)increment;
	return p;
}

int _open(const char* name, int flags, int mode) {
	register long a0 asm("a0") = -100; /* AT_FDCWD */
	register long a1 asm("a1") = (long)name;
	register long a2 asm("a2") = flags & 3; /* read and write only: newlib's other flags are not Linux's */
	register long a3 asm("a3") = mode;
	register long a7 asm("a7") = 56; /* openat */
	asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
	if (a0 < 0) {
		errno = (int)-a0;
		return -1;
	}
	return (int)a0;
}

int _fcntl(int fd, int cmd, int arg) {
	(void)fd, (void)cmd, (void)arg;
	errno = ENOSYS;
	return -1;
}

int _getentropy(void* buffer, size_t length) {
	(void)buffer, (void)length;
	errno = ENOSYS;
	return -1;
}
