// Copyright (c) 2026 Bernhard Haumacher

// The nothrow allocation functions of the firmware: they return nullptr when the heap has no block of
// the requested size, as the standard says.
//
// The firmware is built without C++ exceptions (CONFIG_COMPILER_CXX_EXCEPTIONS off). The toolchain's
// libstdc++ implements `new (std::nothrow)` as the throwing `operator new` inside a try block, and
// without exceptions the throw of std::bad_alloc ends in the stub's abort(): every
// `new (std::nothrow)` that should have been refused, with its reason, rebooted the controller
// instead (a 32 KB image body over the E-Home link with a fragmented heap, ehome_link.cpp,
// haumacher/homecontrol#51). The C++ standard lets a program replace these global functions; the
// replacements allocate with malloc and never throw. The matching operator delete of libstdc++ frees
// with free(), so the default delete fits them.

#include <cstdlib>
#include <new>

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
	return std::malloc(size == 0 ? 1 : size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
	return std::malloc(size == 0 ? 1 : size);
}
