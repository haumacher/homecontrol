# Copyright (c) 2026 Bernhard Haumacher
#
# The host test of the WebAssembly block spike (issue #59, doc/blocks-wasm.md): WAMR 2.4.5 (the
# upstream release, fetched by URL and hash, unpatched) built for Linux with the configuration
# chosen for the device - fast interpreter plus AOT, instruction metering, no WASI, no libc
# builtins, software bounds checks as on the C6 (no MMU guard pages there).
#
#   cmake -S spike/blocks-wasm/host -B build-blocks-wasm -G Ninja
#   cmake --build build-blocks-wasm && ctest --test-dir build-blocks-wasm --output-on-failure
#
# Not part of the main build: the core does not depend on WAMR (yet); see doc/blocks-wasm.md.

cmake_minimum_required(VERSION 3.16)
project(blocks_wasm_host C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
set(WAMR_URL "https://github.com/bytecodealliance/wasm-micro-runtime/archive/refs/tags/WAMR-2.4.5.tar.gz"
	CACHE STRING "WAMR release tarball")
FetchContent_Declare(wamr
	URL "${WAMR_URL}"
	URL_HASH SHA256=1ab09d51099f276ca4a1d6629f6b589aab2bd0caa01445e05031a4bed22c199b
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(wamr)
if(NOT wamr_POPULATED)
	FetchContent_Populate(wamr)
endif()

set(WAMR_BUILD_PLATFORM "linux")
set(WAMR_BUILD_TARGET "X86_64")
set(WAMR_BUILD_INTERP 1)
set(WAMR_BUILD_FAST_INTERP 1)
set(WAMR_BUILD_AOT 1)
set(WAMR_BUILD_JIT 0)
set(WAMR_BUILD_LIBC_BUILTIN 0)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_MULTI_MODULE 0)
set(WAMR_BUILD_SHARED_MEMORY 0)
set(WAMR_BUILD_THREAD_MGR 0)
set(WAMR_BUILD_BULK_MEMORY 0)
set(WAMR_BUILD_REF_TYPES 0)
set(WAMR_BUILD_SIMD 0)
set(WAMR_BUILD_INSTRUCTION_METERING 1)
set(WAMR_DISABLE_HW_BOUND_CHECK 1)
set(WAMR_DISABLE_STACK_HW_BOUND_CHECK 1)
set(WAMR_ROOT_DIR "${wamr_SOURCE_DIR}")
include("${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake")
add_library(vmlib STATIC ${WAMR_RUNTIME_LIB_SOURCE})
target_compile_options(vmlib PRIVATE -w)
target_link_libraries(vmlib PUBLIC m pthread)

set(SPIKE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
add_executable(blocks_wasm_host_test
	"${CMAKE_CURRENT_LIST_DIR}/host_test.cpp"
	"${SPIKE_DIR}/runner/spike_runner.cpp")
target_include_directories(blocks_wasm_host_test PRIVATE "${SPIKE_DIR}/runner" "${WAMR_ROOT_DIR}/core/iwasm/include")
target_compile_options(blocks_wasm_host_test PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
target_compile_definitions(blocks_wasm_host_test PRIVATE WASM_ENABLE_INSTRUCTION_METERING=1)
target_link_libraries(blocks_wasm_host_test PRIVATE vmlib)

enable_testing()
add_test(NAME BlocksWasm COMMAND blocks_wasm_host_test "${SPIKE_DIR}")

# The AOT termination test needs a WAMR with the thread manager: a project of its own, so the two
# WAMR configurations do not share their compile definitions.
include(ExternalProject)
ExternalProject_Add(thread_mgr_test
	SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/thread-mgr"
	BINARY_DIR "${CMAKE_BINARY_DIR}/thread-mgr"
	CMAKE_ARGS -DWAMR_ROOT_DIR=${WAMR_ROOT_DIR} -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
	INSTALL_COMMAND ""
	BUILD_ALWAYS TRUE)
add_test(NAME BlocksWasmAotTerminate
	COMMAND "${CMAKE_BINARY_DIR}/thread-mgr/blocks_wasm_aot_terminate_test" "${SPIKE_DIR}")
