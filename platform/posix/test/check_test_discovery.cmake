#
# Copyright (c) 2026 Bernhard Haumacher
#
# Guard of the test gate: every test case the doctest runner lists must be a CTest entry. Compares
# the runner's --list-test-cases with the add_test calls of the CTest file the discovery generated
# (platform/posix/test/doctest_add_tests.cmake); a name the discovery mangles (as the vendored
# script did with ';') makes the counts or the names differ.
#
# Usage: cmake -DHC_TEST_EXE=<runner> -DHC_CTEST_FILE=<generated tests file> -P check_test_discovery.cmake

# Masks the characters CMake's list handling interprets (';', '[', ']', '\') and splits into lines.
function(split_lines VAR TEXT)
	string(REPLACE "\\" "<bs>" TEXT "${TEXT}")
	string(REPLACE ";" "<sc>" TEXT "${TEXT}")
	string(REPLACE "[" "<o>" TEXT "${TEXT}")
	string(REPLACE "]" "<c>" TEXT "${TEXT}")
	string(REPLACE "\n" ";" TEXT "${TEXT}")
	set(${VAR} "${TEXT}" PARENT_SCOPE)
endfunction()

execute_process(COMMAND "${HC_TEST_EXE}" --list-test-cases --no-version
	OUTPUT_VARIABLE output RESULT_VARIABLE result)
if(NOT result EQUAL 0)
	message(FATAL_ERROR "${HC_TEST_EXE} --list-test-cases failed: ${result}")
endif()
split_lines(lines "${output}")
set(listed "")
foreach(line IN LISTS lines)
	if("${line}" STREQUAL "" OR "${line}" MATCHES "^=+$" OR "${line}" MATCHES "^<o>doctest<c> ")
		continue()
	endif()
	list(APPEND listed "${line}")
endforeach()

if(NOT EXISTS "${HC_CTEST_FILE}")
	message(FATAL_ERROR "the discovered tests file ${HC_CTEST_FILE} does not exist")
endif()
file(READ "${HC_CTEST_FILE}" content)
split_lines(lines "${content}")
set(registered "")
foreach(line IN LISTS lines)
	if(NOT line MATCHES "^add_test\\(")
		continue()
	endif()
	# The test name is the first bracket argument: add_test([==[<name>]==] [==[<command>]==] ...).
	if(NOT line MATCHES "^add_test\\(<o>==<o>(.*)<c>==<c> <o>==<o>")
		message(FATAL_ERROR "unexpected line in ${HC_CTEST_FILE}: ${line}")
	endif()
	string(REGEX REPLACE "<c>==<c> <o>==<o>.*" "" name "${CMAKE_MATCH_1}")
	list(APPEND registered "${name}")
endforeach()

list(LENGTH listed nlisted)
list(LENGTH registered nregistered)
set(missing ${listed})
list(REMOVE_ITEM missing ${registered})
if(NOT nlisted EQUAL nregistered OR missing)
	string(REPLACE ";" "\n  " missing "${missing}")
	message(FATAL_ERROR "the runner lists ${nlisted} test cases, CTest has ${nregistered}; not registered:\n  ${missing}")
endif()
message(STATUS "all ${nlisted} test cases are CTest entries")
