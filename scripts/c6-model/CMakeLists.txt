# Copyright (c) 2026 Bernhard Haumacher
#
# The 32-bit model of the core's heap on the ESP32-C6 (homecontrol #70, doc/image.md "Memory of a
# swap"): the core and the swap memory test, compiled by the ESP-IDF's RISC-V compiler with the C6's
# sizes (4-byte pointers, its alignment) and run under qemu-riscv32 user mode; the allocation
# counter charges each block as the C6's heap does. Not part of the gate.
#
#   source ~/tools/esp/esp-idf-v5.5/export.sh
#   cmake -S scripts/c6-model -B build-c6model -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/scripts/c6-model/toolchain.cmake \
#       [-DHC_SWAP_IMAGE=<an image without blocks>]
#   cmake --build build-c6model && qemu-riscv32-static build-c6model/swap_c6
cmake_minimum_required(VERSION 3.16)
project(hc_c6_model C CXX)
get_filename_component(HC_REPO_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(CMAKE_CXX_STANDARD 17)
set(HC_SWAP_IMAGE "" CACHE FILEPATH "An image without blocks for the per-phase measurement (empty: none)")
# As the firmware: -Og, no exceptions; no thread-safe statics (newlib's `sim` has no threads).
add_compile_options(-Og -fno-exceptions $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>)
add_subdirectory("${HC_REPO_DIR}/core" core)
add_executable(swap_c6
	"${HC_REPO_DIR}/core/test/test_main.cpp"
	"${HC_REPO_DIR}/core/test/allocation_counter.cpp"
	"${HC_REPO_DIR}/core/test/swap_memory_test.cpp"
	sim_stubs.c)
target_link_libraries(swap_c6 PRIVATE hc_core)
target_include_directories(swap_c6 SYSTEM PRIVATE "${HC_REPO_DIR}/third_party/doctest")
target_compile_definitions(swap_c6 PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
	DOCTEST_CONFIG_NO_POSIX_SIGNALS DOCTEST_CONFIG_NO_MULTITHREADING HC_SPEC_DIR="${HC_REPO_DIR}/spec")
if(HC_SWAP_IMAGE)
	# newlib's `sim` passes no environment: the path is compiled in.
	target_compile_definitions(swap_c6 PRIVATE HC_SWAP_IMAGE_DEFAULT="${HC_SWAP_IMAGE}")
endif()
