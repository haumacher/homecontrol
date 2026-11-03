# Copyright (c) 2026 Bernhard Haumacher
#
# The measurement app of the WebAssembly block spike for the ESP32-C6 (issue #59,
# doc/blocks-wasm.md). Not the controller: a separate application that is flashed to ota_0 for the
# measurements and replaced by the controller again afterwards. WAMR is the unpatched upstream
# release 2.4.5 (its repository root is an IDF component), downloaded and checked by SHA-256 at the
# first configure.
#
#   idf.py -B build-<variant> -D SPIKE_VARIANT=<variant> set-target esp32c6
#   idf.py -B build-<variant> build
#
# SPIKE_VARIANT (sdkconfig.defaults.<variant>):
#   both     fast interpreter + AOT: the measurement app that is flashed
#   interp   fast interpreter only (the flash cost of the interpreter)
#   aot      AOT only (the flash cost of the AOT loader)
#   classic  classic interpreter only (for comparison)
# -D SPIKE_WAMR_O2=ON compiles WAMR at -O2 (default: the project's -Og, as in the controller).
# A changed variant needs the build directory's sdkconfig deleted.

cmake_minimum_required(VERSION 3.16)

set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
set(SPIKE_VARIANT "both" CACHE STRING "both | interp | aot | classic")
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
	"${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.${SPIKE_VARIANT}")

# ---- WAMR: the pinned upstream release ------------------------------------------------------
set(WAMR_VERSION 2.4.5)
set(WAMR_SHA256 1ab09d51099f276ca4a1d6629f6b589aab2bd0caa01445e05031a4bed22c199b)
set(WAMR_TARBALL "" CACHE FILEPATH "A local copy of the WAMR ${WAMR_VERSION} source tarball (else downloaded)")
set(_wamr_src "${CMAKE_BINARY_DIR}/wamr-src/wasm-micro-runtime-WAMR-${WAMR_VERSION}")
if(NOT EXISTS "${_wamr_src}/CMakeLists.txt")
	set(_tar "${CMAKE_BINARY_DIR}/wamr-${WAMR_VERSION}.tar.gz")
	if(WAMR_TARBALL)
		configure_file("${WAMR_TARBALL}" "${_tar}" COPYONLY)
	else()
		file(DOWNLOAD "https://github.com/bytecodealliance/wasm-micro-runtime/archive/refs/tags/WAMR-${WAMR_VERSION}.tar.gz"
			"${_tar}" STATUS _status)
	endif()
	file(SHA256 "${_tar}" _sha)
	if(NOT _sha STREQUAL WAMR_SHA256)
		message(FATAL_ERROR "WAMR tarball ${_tar}: SHA-256 ${_sha}, expected ${WAMR_SHA256}")
	endif()
	file(ARCHIVE_EXTRACT INPUT "${_tar}" DESTINATION "${CMAKE_BINARY_DIR}/wamr-src")
endif()
# The IDF names a component after its directory: give the release root the name "wamr".
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/components")
if(NOT EXISTS "${CMAKE_BINARY_DIR}/components/wamr")
	file(CREATE_LINK "${_wamr_src}" "${CMAKE_BINARY_DIR}/components/wamr" SYMBOLIC)
endif()
set(EXTRA_COMPONENT_DIRS "${CMAKE_BINARY_DIR}/components/wamr")

# WAMR options the IDF's Kconfig of WAMR does not cover; build-scripts/runtime_lib.cmake reads them
# when the component is configured.
set(WAMR_BUILD_INSTRUCTION_METERING 1 CACHE INTERNAL "")
set(WAMR_BUILD_BULK_MEMORY 0 CACHE INTERNAL "")
set(WAMR_BUILD_REF_TYPES 0 CACHE INTERNAL "")
set(WAMR_BUILD_SIMD 0 CACHE INTERNAL "")
set(WAMR_BUILD_LIBC_BUILTIN 0 CACHE INTERNAL "")
set(WAMR_BUILD_LIBC_WASI 0 CACHE INTERNAL "")
set(WAMR_BUILD_MULTI_MODULE 0 CACHE INTERNAL "")
set(WAMR_BUILD_SHRUNK_MEMORY 1 CACHE INTERNAL "")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.esp32c6")
project(spike_wasm)

# SPIKE_WAMR_O2: WAMR at -O2 instead of the project's -Og (the controller's level), for the speed
# and size comparison of doc/blocks-wasm.md.
option(SPIKE_WAMR_O2 "Compile WAMR with -O2" OFF)
if(SPIKE_WAMR_O2)
	idf_component_get_property(_wamr_lib wamr COMPONENT_LIB)
	target_compile_options(${_wamr_lib} PRIVATE -O2)
endif()

partition_table_add_check_size_target(spike_check_size
	DEPENDS gen_project_binary
	BINARY_PATH "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin"
	PARTITION_TYPE app PARTITION_SUBTYPE ota_0)
