# Copyright (c) 2026 Bernhard Haumacher
#
# The host build: the portable core, the POSIX platform, the `homecontrol` executable and the unit
# test runner `hc_tests`.
#
#   cmake -S platform/posix -B build -G Ninja && cmake --build build && ctest --test-dir build

cmake_minimum_required(VERSION 3.16)
project(homecontrol_posix LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(HC_REPO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# The core's rules apply to everything built here: no exceptions, no RTTI, warnings are errors.
add_compile_options(-Wall -Wextra -Werror -fno-exceptions -fno-rtti)

add_subdirectory("${HC_REPO_DIR}/core" core)

add_library(hc_posix STATIC src/platform.cpp src/file_settings.cpp)
target_include_directories(hc_posix PUBLIC include)
target_link_libraries(hc_posix PUBLIC hc_core)

add_executable(homecontrol src/main.cpp)
target_link_libraries(homecontrol PRIVATE hc_posix)

# Unit tests: doctest, vendored. The core's tests (core/test) and the POSIX platform's
# (platform/posix/test) share one runner. One CTest entry per test case (discovered after each build), so
# `ctest -R <TestName>` selects single cases.
enable_testing()
add_executable(hc_tests
	"${HC_REPO_DIR}/core/test/test_main.cpp"
	"${HC_REPO_DIR}/core/test/log_test.cpp"
	"${HC_REPO_DIR}/core/test/event_loop_test.cpp"
	"${HC_REPO_DIR}/core/test/controller_test.cpp"
	"${HC_REPO_DIR}/core/test/event_loop_probe_test.cpp"
	"${HC_REPO_DIR}/core/test/network_probe_test.cpp"
	"${HC_REPO_DIR}/core/test/improv_probe_test.cpp"
	"${HC_REPO_DIR}/core/test/backoff_test.cpp"
	"${HC_REPO_DIR}/core/test/connectivity_test.cpp"
	"${HC_REPO_DIR}/core/test/settings_test.cpp"
	"${HC_REPO_DIR}/core/test/tz_test.cpp"
	"${HC_REPO_DIR}/core/test/hold_button_test.cpp"
	"${HC_REPO_DIR}/core/test/status_led_test.cpp"
	"${HC_REPO_DIR}/core/test/improv_protocol_test.cpp"
	"${HC_REPO_DIR}/core/test/improv_test.cpp"
	"${HC_REPO_DIR}/core/test/update_check_test.cpp"
	"${HC_REPO_DIR}/core/test/update_check_probe_test.cpp"
	"${HC_REPO_DIR}/core/test/datapoint_test.cpp"
	"${HC_REPO_DIR}/core/test/dpt_test.cpp"
	"${HC_REPO_DIR}/core/test/dpt_corpus_test.cpp"
	"${HC_REPO_DIR}/core/test/datapoint_probe_test.cpp"
	"${HC_REPO_DIR}/core/test/astro_test.cpp"
	"${HC_REPO_DIR}/core/test/time_service_test.cpp"
	"${HC_REPO_DIR}/core/test/scheduler_test.cpp"
	"${HC_REPO_DIR}/core/test/scheduler_probe_test.cpp"
	test/file_settings_test.cpp
	test/posix_platform_test.cpp
)
target_include_directories(hc_tests SYSTEM PRIVATE "${HC_REPO_DIR}/third_party/doctest")
target_include_directories(hc_tests PRIVATE "${HC_REPO_DIR}/core/test")
target_link_libraries(hc_tests PRIVATE hc_posix)
# doctest uses exceptions only when they are enabled; tell it explicitly.
target_compile_definitions(hc_tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
# The DPT conformance corpus (spec/fixtures/dpt) is read at run time.
target_compile_definitions(hc_tests PRIVATE HC_SPEC_DIR="${HC_REPO_DIR}/spec")

include("${HC_REPO_DIR}/third_party/doctest/cmake/doctest.cmake")
doctest_discover_tests(hc_tests)

# KNXnet/IP tunnelling (issue #7, doc/knx.md): the UDP socket on BSD sockets (the socket calls are
# shared with the ESP build in platform/common), the diagnostic tool `knxtool` and the KNX tests.
target_sources(hc_posix PRIVATE src/udp_socket.cpp "${HC_REPO_DIR}/platform/common/src/bsd_udp.cpp")
target_include_directories(hc_posix PUBLIC "${HC_REPO_DIR}/platform/common/include")
add_executable(knxtool tools/knxtool.cpp)
target_link_libraries(knxtool PRIVATE hc_posix)
target_sources(hc_tests PRIVATE
	"${HC_REPO_DIR}/core/test/knx_frames_test.cpp"
	"${HC_REPO_DIR}/core/test/knx_tunnel_test.cpp"
	"${HC_REPO_DIR}/core/test/knx_adapter_test.cpp"
	"${HC_REPO_DIR}/core/test/knx_probe_test.cpp"
	test/posix_udp_test.cpp
)

# The KNX time master (issue #9, doc/knx.md "Time master").
target_sources(hc_tests PRIVATE "${HC_REPO_DIR}/core/test/knx_time_master_test.cpp")
target_sources(hc_tests PRIVATE "${HC_REPO_DIR}/core/test/transmitter_probe_test.cpp")
