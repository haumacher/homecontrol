#
# Copyright (c) 2026 Bernhard Haumacher
#
# The test discovery of doctest_discover_tests (third_party/doctest/cmake/doctest.cmake), replacing
# the vendored doctestAddTests.cmake: one CTest entry per doctest test case. The vendored script
# splits the output of --list-test-cases with CMake's list semantics, so a test case whose name
# contains ';' fell apart into fragments that matched no test case, and ctest reported them as
# passed without running anything. Here a ';' is masked before the output is split, the CTest name
# keeps the test case's name, and the --test-case filter matches the ';' with the wildcard '?'
# (doctest's filters know only '*' and '?'; ',' separates filters and is escaped). '[', ']' and '\'
# are masked as well: CMake's list splitting does not split inside brackets and honours '\;'.
#
# Input (set by doctest_discover_tests): TEST_EXECUTABLE, TEST_EXECUTOR, TEST_WORKING_DIR,
# TEST_SPEC, TEST_EXTRA_ARGS, TEST_PROPERTIES, TEST_PREFIX, TEST_SUFFIX, TEST_LIST, CTEST_FILE.
# ADD_LABELS and JUNIT_OUTPUT_DIR are not supported (this project uses neither).

set(semicolon "<doctest-semicolon>")
set(open "<doctest-open>")
set(close "<doctest-close>")
set(backslash "<doctest-backslash>")
set(script "")
set(tests "")

# Quotes one argument as a bracket argument: never list-expanded, no escapes needed.
function(quote VAR VALUE)
	set(${VAR} "[==[${VALUE}]==]" PARENT_SCOPE)
endfunction()

if(NOT EXISTS "${TEST_EXECUTABLE}")
	message(FATAL_ERROR "Specified test executable '${TEST_EXECUTABLE}' does not exist")
endif()

set(spec "")
if("${TEST_SPEC}" MATCHES .)
	set(spec "--test-case=${TEST_SPEC}")
endif()

execute_process(
	COMMAND ${TEST_EXECUTOR} "${TEST_EXECUTABLE}" ${spec} --list-test-cases
	OUTPUT_VARIABLE output
	RESULT_VARIABLE result
	WORKING_DIRECTORY "${TEST_WORKING_DIR}"
)
if(NOT ${result} EQUAL 0)
	message(FATAL_ERROR "Error running test executable '${TEST_EXECUTABLE}':\n  Result: ${result}\n  Output: ${output}\n")
endif()

# Mask the characters CMake's list handling would interpret before splitting into lines.
string(REPLACE "\\" "${backslash}" output "${output}")
string(REPLACE ";" "${semicolon}" output "${output}")
string(REPLACE "[" "${open}" output "${output}")
string(REPLACE "]" "${close}" output "${output}")
string(REPLACE "\n" ";" lines "${output}")

foreach(line IN LISTS lines)
	string(REPLACE "${open}" "[" line "${line}")
	string(REPLACE "${close}" "]" line "${line}")
	string(REPLACE "${backslash}" "\\" line "${line}")
	if("${line}" STREQUAL "" OR "${line}" MATCHES "^=+$" OR "${line}" MATCHES "^\\[doctest\\] ")
		continue()
	endif()
	string(REPLACE "${semicolon}" ";" name "${TEST_PREFIX}${line}${TEST_SUFFIX}")
	# The filter: ',' separates doctest filters, '?' stands for the ';'.
	string(REPLACE "," "\\," filter "${line}")
	string(REPLACE "${semicolon}" "?" filter "${filter}")

	quote(qname "${name}")
	quote(qexe "${TEST_EXECUTABLE}")
	quote(qfilter "--test-case=${filter}")
	quote(qdir "${TEST_WORKING_DIR}")
	set(command "")
	foreach(arg IN LISTS TEST_EXECUTOR)
		quote(qarg "${arg}")
		string(APPEND command " ${qarg}")
	endforeach()
	string(APPEND command " ${qexe} ${qfilter}")
	foreach(arg IN LISTS TEST_EXTRA_ARGS)
		quote(qarg "${arg}")
		string(APPEND command " ${qarg}")
	endforeach()
	set(props "")
	foreach(prop IN LISTS TEST_PROPERTIES)
		quote(qprop "${prop}")
		string(APPEND props " ${qprop}")
	endforeach()
	# add_test's old signature (as the vendored script): add_test(<name> <command> <arg>...).
	string(APPEND script "add_test(${qname}${command})\n")
	string(APPEND script "set_tests_properties(${qname} PROPERTIES WORKING_DIRECTORY ${qdir}${props})\n")
	string(REPLACE ";" "\;" listed "${name}")
	list(APPEND tests "${listed}")
endforeach()

# The list of all discovered tests, for scripts that set properties on them.
set(qlist "")
foreach(test IN LISTS tests)
	quote(qtest "${test}")
	string(APPEND qlist " ${qtest}")
endforeach()
string(APPEND script "set(${TEST_LIST}${qlist})\n")

file(WRITE "${CTEST_FILE}" "${script}")
