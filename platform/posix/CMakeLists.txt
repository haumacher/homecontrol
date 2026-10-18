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

get_filename_component(HC_REPO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." REALPATH)

# The core's rules apply to everything built here: no exceptions, no RTTI, warnings are errors.
add_compile_options(-Wall -Wextra -Werror -fno-exceptions -fno-rtti)

add_subdirectory("${HC_REPO_DIR}/core" core)

add_library(hc_posix STATIC src/platform.cpp src/file_settings.cpp)
target_include_directories(hc_posix PUBLIC include)
target_link_libraries(hc_posix PUBLIC hc_core)

# The web server (doc/web.md): cpp-httplib (third_party/cpp-httplib, MIT, v0.61.0) for HTTP, the core's
# WebSocket codec on its own port. A separate library, so the test runner does not link it.
find_package(Threads REQUIRED)
add_library(hc_posix_web STATIC src/web_server.cpp)
target_link_libraries(hc_posix_web PUBLIC hc_posix Threads::Threads)
target_include_directories(hc_posix_web SYSTEM PRIVATE "${HC_REPO_DIR}/third_party/cpp-httplib")
target_compile_definitions(hc_posix_web PRIVATE CPPHTTPLIB_NO_EXCEPTIONS)

add_executable(homecontrol src/main.cpp)
target_link_libraries(homecontrol PRIVATE hc_posix hc_posix_web)
# The UI files served by default (homecontrol --www <dir> overrides).
target_compile_definitions(homecontrol PRIVATE HC_WEB_UI_DIR="${HC_REPO_DIR}/web/ui/dist")

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
	"${HC_REPO_DIR}/core/test/update_upload_test.cpp"
	"${HC_REPO_DIR}/core/test/update_upload_probe_test.cpp"
	"${HC_REPO_DIR}/core/test/update_check_probe_test.cpp"
	"${HC_REPO_DIR}/core/test/datapoint_test.cpp"
	"${HC_REPO_DIR}/core/test/dpt_test.cpp"
	"${HC_REPO_DIR}/core/test/dpt_corpus_test.cpp"
	"${HC_REPO_DIR}/core/test/datapoint_probe_test.cpp"
	"${HC_REPO_DIR}/core/test/sha_test.cpp"
	"${HC_REPO_DIR}/core/test/json_test.cpp"
	"${HC_REPO_DIR}/core/test/websocket_test.cpp"
	"${HC_REPO_DIR}/core/test/log_ring_test.cpp"
	"${HC_REPO_DIR}/core/test/auth_test.cpp"
	"${HC_REPO_DIR}/core/test/web_api_test.cpp"
	"${HC_REPO_DIR}/core/test/web_system_test.cpp"
	"${HC_REPO_DIR}/core/test/web_upload_test.cpp"
	"${HC_REPO_DIR}/core/test/web_system_probe_test.cpp"
	"${HC_REPO_DIR}/core/test/web_probe_test.cpp"
	"${HC_REPO_DIR}/core/test/live_feed_test.cpp"
	"${HC_REPO_DIR}/core/test/monitor_feed_test.cpp"
	"${HC_REPO_DIR}/core/test/monitor_probe_test.cpp"
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
# Our discovery script in place of the vendored doctestAddTests.cmake, which loses test cases whose
# name contains ';' (CMake list splitting) while ctest stays green.
set(_DOCTEST_DISCOVER_TESTS_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/test/doctest_add_tests.cmake")
doctest_discover_tests(hc_tests)
# The guard of the test gate: every test case the runner lists is a CTest entry. The tests file is
# the one doctest_discover_tests generates (named after the hash of TEST_SPEC and EXTRA_ARGS).
string(SHA1 hc_discover_hash " ")
string(SUBSTRING "${hc_discover_hash}" 0 7 hc_discover_hash)
add_test(NAME TestDiscovery.EveryTestCaseIsACTestEntry
	COMMAND "${CMAKE_COMMAND}" "-DHC_TEST_EXE=$<TARGET_FILE:hc_tests>"
		"-DHC_CTEST_FILE=${CMAKE_CURRENT_BINARY_DIR}/hc_tests_tests-${hc_discover_hash}.cmake"
		-P "${CMAKE_CURRENT_LIST_DIR}/test/check_test_discovery.cmake")
# The build id follows the checkout at build time, not at configure time (#36): the controller's
# --version must report what `git describe --always --dirty` says now.
add_test(NAME BuildId.FollowsGitDescribe
	COMMAND "${CMAKE_COMMAND}" "-DHC_GIT_DIR=${HC_REPO_DIR}" "-DHC_PROGRAM=$<TARGET_FILE:homecontrol>"
		-P "${HC_REPO_DIR}/platform/posix/test/check_build_id.cmake")

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
target_sources(hc_tests PRIVATE "${HC_REPO_DIR}/core/test/function_roles_probe_test.cpp") # function roles (#30)
target_sources(hc_tests PRIVATE "${HC_REPO_DIR}/core/test/schedule_output_probe_test.cpp") # schedule outputs are not rule outputs (#33)
# State persistence and short-term history (issue #21).
target_sources(hc_tests PRIVATE
	"${HC_REPO_DIR}/core/test/value_store_test.cpp"
	"${HC_REPO_DIR}/core/test/history_test.cpp"
	"${HC_REPO_DIR}/core/test/restore_probe_test.cpp"
)
# The rule interpreter's tests (the corpus spec/fixtures/rules is read at run time).
target_sources(hc_tests PRIVATE
	"${HC_REPO_DIR}/core/test/rules_corpus_test.cpp"
	"${HC_REPO_DIR}/core/test/rules_unit_test.cpp"
	"${HC_REPO_DIR}/core/test/allocation_counter.cpp"
	"${HC_REPO_DIR}/core/test/rules_probe_test.cpp"
)
# The runtime image (issue #8, spec/runtime-image.md; the fixtures spec/fixtures/images are read at run time).
target_sources(hc_tests PRIVATE "${HC_REPO_DIR}/core/test/image_test.cpp")
# The BlobStore on files (issue #21, doc/persistence.md).
target_sources(hc_posix PRIVATE src/file_blob_store.cpp)
target_sources(hc_tests PRIVATE test/file_blob_store_test.cpp)
target_sources(hc_tests PRIVATE test/blob_store_probe_test.cpp)
# External values (issue #17, doc/external.md): the core's tests, and the HTTP fetcher on cpp-httplib in
# the web library (it shares the vendored header and the LoopExecutor). HTTPS needs OpenSSL's development
# files; without them the fetcher speaks plain HTTP and refuses https:// with that reason.
target_sources(hc_tests PRIVATE "${HC_REPO_DIR}/core/test/external_values_test.cpp")
target_sources(hc_tests PRIVATE "${HC_REPO_DIR}/core/test/external_probe_test.cpp")
target_sources(hc_posix_web PRIVATE src/httplib_fetcher.cpp)
find_package(OpenSSL 3.0 QUIET)
if(OPENSSL_FOUND)
	target_compile_definitions(hc_posix_web PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
	target_link_libraries(hc_posix_web PUBLIC OpenSSL::SSL OpenSSL::Crypto)
	message(STATUS "External values: HTTPS through OpenSSL ${OPENSSL_VERSION}")
else()
	message(STATUS "External values: OpenSSL development files not found: the POSIX fetcher speaks HTTP only")
endif()
# The Zigbee coordinator's core (issue #15, doc/zigbee.md): database, mapping, adapter under the fake clock.
target_sources(hc_tests PRIVATE
	"${HC_REPO_DIR}/core/test/zigbee_database_test.cpp"
	"${HC_REPO_DIR}/core/test/zigbee_mapping_test.cpp"
	"${HC_REPO_DIR}/core/test/zigbee_adapter_test.cpp"
	"${HC_REPO_DIR}/core/test/zigbee_probe_test.cpp"
)
# Diagnostics (issue #4, doc/diagnostics.md): the reset reason, the boot facts, log levels per tag, the ring's tail;
# the POSIX diagnostics (PID, start time, the signal that stopped the previous run).
target_sources(hc_tests PRIVATE "${HC_REPO_DIR}/core/test/diagnostics_test.cpp")
target_sources(hc_tests PRIVATE "${HC_REPO_DIR}/core/test/diagnostics_probe_test.cpp")
target_sources(hc_tests PRIVATE "${HC_REPO_DIR}/core/test/coredump_api_test.cpp")
target_sources(hc_posix PRIVATE src/posix_diagnostics.cpp)
target_sources(hc_tests PRIVATE test/posix_diagnostics_test.cpp)
# Dynamic datapoints in the image-loaded model: providers, the Zigbee bindings of the image (issue #29).
target_sources(hc_tests PRIVATE "${HC_REPO_DIR}/core/test/image_dynamic_test.cpp")
# Internationalization (issue #24, doc/i18n.md): the language pack decision, the ui.lang setting, the pack key sets.
target_sources(hc_tests PRIVATE "${HC_REPO_DIR}/core/test/language_packs_test.cpp")
target_sources(hc_tests PRIVATE "${HC_REPO_DIR}/core/test/language_packs_probe_test.cpp")
target_sources(hc_tests PRIVATE "${HC_REPO_DIR}/core/test/wifi_power_save_test.cpp")
target_compile_definitions(hc_tests PRIVATE HC_WEB_DIR="${HC_REPO_DIR}/web")
# The committed language packs (web/ui/lang/<code>.json) are copied next to the base language
# (web/ui/dist/lang/en.json), where the POSIX server serves them; the copies are not versioned. The ESP
# build packs them into storage.bin itself (tools/gen_ui_image.py).
file(GLOB hc_lang_packs CONFIGURE_DEPENDS "${HC_REPO_DIR}/web/ui/lang/*.json")
set(hc_lang_copies "")
foreach(pack ${hc_lang_packs})
	get_filename_component(pack_name "${pack}" NAME)
	set(copy "${HC_REPO_DIR}/web/ui/dist/lang/${pack_name}")
	add_custom_command(OUTPUT "${copy}" COMMAND ${CMAKE_COMMAND} -E copy_if_different "${pack}" "${copy}"
		DEPENDS "${pack}" VERBATIM)
	list(APPEND hc_lang_copies "${copy}")
endforeach()
add_custom_target(hc_lang_packs ALL DEPENDS ${hc_lang_copies})

# The E-Home connection (issue #41, doc/ehome.md): the enrollment and the socket against a fake E-Home; the
# POSIX WebSocket client (plain ws://, in the web library for the LoopExecutor) and its test against an
# in-process server speaking the core's codec.
target_sources(hc_tests PRIVATE "${HC_REPO_DIR}/core/test/ehome_link_test.cpp")
target_sources(hc_posix_web PRIVATE src/websocket_client.cpp)
# The MQTT client's core tests (issue #18, doc/mqtt.md).
target_sources(hc_tests PRIVATE
	"${HC_REPO_DIR}/core/test/mqtt_codec_test.cpp"
	"${HC_REPO_DIR}/core/test/mqtt_test.cpp"
	"${HC_REPO_DIR}/core/test/mqtt_probe_test.cpp"
)
# The POSIX MQTT client (a small MQTT 3.1.1 client over TCP, in the web library for the LoopExecutor) and
# its test against an in-process broker speaking the core's codec.
target_sources(hc_posix_web PRIVATE src/mqtt_client.cpp)
add_executable(hc_posix_mqtt_tests "${HC_REPO_DIR}/core/test/test_main.cpp" test/posix_mqtt_client_test.cpp
	test/posix_websocket_client_test.cpp)
target_include_directories(hc_posix_mqtt_tests SYSTEM PRIVATE "${HC_REPO_DIR}/third_party/doctest")
target_include_directories(hc_posix_mqtt_tests PRIVATE "${HC_REPO_DIR}/core/test")
target_link_libraries(hc_posix_mqtt_tests PRIVATE hc_posix_web)
target_compile_definitions(hc_posix_mqtt_tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
doctest_discover_tests(hc_posix_mqtt_tests)
# The System page's facts of the host (issue #42): threads, resident memory, thermal zone, CPU.
target_sources(hc_posix PRIVATE src/posix_system_info.cpp)
target_sources(hc_tests PRIVATE "${HC_REPO_DIR}/core/test/web_diagnostics_test.cpp")
target_sources(hc_tests PRIVATE "${HC_REPO_DIR}/core/test/web_diagnostics_probe_test.cpp")
target_sources(hc_tests PRIVATE "${HC_REPO_DIR}/core/test/memory_probe_test.cpp")
target_sources(hc_tests PRIVATE test/posix_system_info_test.cpp)
