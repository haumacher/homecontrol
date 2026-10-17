# Copyright (c) 2026 Bernhard Haumacher
#
# The single source list of the portable core, shared by the host build (core/CMakeLists.txt) and
# the ESP-IDF component (platform/esp/components/hc_core/CMakeLists.txt). Defines:
#   HC_CORE_SOURCES      absolute paths of the core's sources
#   HC_CORE_INCLUDE_DIR  the public include directory
#   HC_CORE_BUILD_ID     `git describe --always --dirty` of the checkout, or "unknown"

set(HC_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(HC_CORE_INCLUDE_DIR "${HC_CORE_DIR}/include")
set(HC_CORE_SOURCES
	"${HC_CORE_DIR}/src/backoff.cpp"
	"${HC_CORE_DIR}/src/connectivity.cpp"
	"${HC_CORE_DIR}/src/controller.cpp"
	"${HC_CORE_DIR}/src/datapoint.cpp"
	"${HC_CORE_DIR}/src/dpt.cpp"
	"${HC_CORE_DIR}/src/event_loop.cpp"
	"${HC_CORE_DIR}/src/hold_button.cpp"
	"${HC_CORE_DIR}/src/improv.cpp"
	"${HC_CORE_DIR}/src/improv_protocol.cpp"
	"${HC_CORE_DIR}/src/log.cpp"
	"${HC_CORE_DIR}/src/network_settings.cpp"
	"${HC_CORE_DIR}/src/settings.cpp"
	"${HC_CORE_DIR}/src/status_led.cpp"
	"${HC_CORE_DIR}/src/tz.cpp"
	"${HC_CORE_DIR}/src/version.cpp"
	"${HC_CORE_DIR}/src/update_check.cpp"
)

# The build id is taken at configure time; reconfigure to refresh it after a commit.
set(HC_CORE_BUILD_ID "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" describe --always --dirty
		WORKING_DIRECTORY "${HC_CORE_DIR}"
		OUTPUT_VARIABLE _hc_git_describe
		OUTPUT_STRIP_TRAILING_WHITESPACE
		RESULT_VARIABLE _hc_git_result
		ERROR_QUIET)
	if(_hc_git_result EQUAL 0 AND NOT _hc_git_describe STREQUAL "")
		set(HC_CORE_BUILD_ID "${_hc_git_describe}")
	endif()
endif()
