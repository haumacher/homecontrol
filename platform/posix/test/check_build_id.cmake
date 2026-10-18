# Copyright (c) 2026 Bernhard Haumacher
#
# ctest BuildId.FollowsGitDescribe (#36): `homecontrol --version` names the build id git describes
# for the checkout right now. Run with -DHC_GIT_DIR=<checkout> -DHC_PROGRAM=<homecontrol binary>.

find_package(Git REQUIRED)
execute_process(
	COMMAND "${GIT_EXECUTABLE}" describe --always --dirty
	WORKING_DIRECTORY "${HC_GIT_DIR}"
	OUTPUT_VARIABLE expected
	OUTPUT_STRIP_TRAILING_WHITESPACE
	RESULT_VARIABLE git_result)
if(NOT git_result EQUAL 0)
	message(FATAL_ERROR "git describe failed in ${HC_GIT_DIR}")
endif()
execute_process(
	COMMAND "${HC_PROGRAM}" --version
	OUTPUT_VARIABLE actual
	OUTPUT_STRIP_TRAILING_WHITESPACE
	RESULT_VARIABLE run_result)
if(NOT run_result EQUAL 0)
	message(FATAL_ERROR "${HC_PROGRAM} --version failed: ${run_result}")
endif()
if(NOT actual MATCHES "\\(build ${expected}\\)$")
	message(FATAL_ERROR "build id mismatch: '${actual}' does not end in '(build ${expected})'")
endif()
message(STATUS "${actual}")
