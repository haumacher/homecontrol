# Copyright (c) 2026 Bernhard Haumacher
#
# Build-time generator of the build id (#36). Run as `cmake -DHC_GIT_DIR=<checkout>
# -DHC_OUTPUT=<header> -P build_id.cmake` by the custom target hc_core.cmake adds to every build:
# writes `#define HC_BUILD_ID "<git describe --always --dirty>"` into the header, touching the file
# only when the text changes so nothing recompiles when the id is unchanged.

set(_hc_id "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" describe --always --dirty
		WORKING_DIRECTORY "${HC_GIT_DIR}"
		OUTPUT_VARIABLE _hc_describe
		OUTPUT_STRIP_TRAILING_WHITESPACE
		RESULT_VARIABLE _hc_result
		ERROR_QUIET)
	if(_hc_result EQUAL 0 AND NOT _hc_describe STREQUAL "")
		set(_hc_id "${_hc_describe}")
	endif()
endif()
set(_hc_content "// Generated at build time by core/cmake/build_id.cmake; do not edit.\n#define HC_BUILD_ID \"${_hc_id}\"\n")
set(_hc_old "")
if(EXISTS "${HC_OUTPUT}")
	file(READ "${HC_OUTPUT}" _hc_old)
endif()
if(NOT _hc_old STREQUAL _hc_content)
	file(WRITE "${HC_OUTPUT}" "${_hc_content}")
endif()
