# Copyright (c) 2026 Bernhard Haumacher
#
# AOT code has no instruction metering (issue #59, question 3). The way WAMR offers to stop it is
# wasm_runtime_terminate() from another thread, which AOT code notices only when it was compiled
# with thread-manager support (wamrc --enable-multi-thread: suspend-flag checks in every loop) and
# the runtime has the thread manager. This project builds that runtime of its own; the host test's
# CMakeLists.txt builds it as an external project and passes WAMR_ROOT_DIR.

cmake_minimum_required(VERSION 3.16)
project(blocks_wasm_aot_terminate C CXX)
set(CMAKE_CXX_STANDARD 17)

set(WAMR_BUILD_PLATFORM "linux")
set(WAMR_BUILD_TARGET "X86_64")
set(WAMR_BUILD_INTERP 0)
set(WAMR_BUILD_AOT 1)
set(WAMR_BUILD_JIT 0)
set(WAMR_BUILD_LIBC_BUILTIN 0)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_THREAD_MGR 1)
set(WAMR_BUILD_SHARED_MEMORY 0)
set(WAMR_BUILD_BULK_MEMORY 1)
set(WAMR_BUILD_REF_TYPES 0)
set(WAMR_BUILD_SIMD 0)
set(WAMR_DISABLE_HW_BOUND_CHECK 1)
set(WAMR_DISABLE_STACK_HW_BOUND_CHECK 1)
include("${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake")
add_library(vmlib_mt STATIC ${WAMR_RUNTIME_LIB_SOURCE})
target_compile_options(vmlib_mt PRIVATE -w)
target_link_libraries(vmlib_mt PUBLIC m pthread)

add_executable(blocks_wasm_aot_terminate_test "${CMAKE_CURRENT_LIST_DIR}/aot_terminate_test.cpp")
target_include_directories(blocks_wasm_aot_terminate_test PRIVATE "${WAMR_ROOT_DIR}/core/iwasm/include")
target_compile_options(blocks_wasm_aot_terminate_test PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
target_link_libraries(blocks_wasm_aot_terminate_test PRIVATE vmlib_mt)
