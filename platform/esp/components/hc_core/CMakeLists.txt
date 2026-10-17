# Copyright (c) 2026 Bernhard Haumacher
#
# The portable core as an ESP-IDF component. The source list lives in core/hc_core.cmake and is
# shared with the host build; nothing is listed here.

include("${CMAKE_CURRENT_LIST_DIR}/../../../../core/hc_core.cmake")

idf_component_register(
	SRCS ${HC_CORE_SOURCES}
	INCLUDE_DIRS "${HC_CORE_INCLUDE_DIR}")

# The same strictness as on the host (the IDF adds its own warning set on top).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(${COMPONENT_LIB} PRIVATE HC_BUILD_ID="${HC_CORE_BUILD_ID}")
