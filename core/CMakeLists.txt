# Copyright (c) 2026 Bernhard Haumacher
#
# The portable core as a static library for the host build. The ESP-IDF build compiles the same
# source list (hc_core.cmake) as the component platform/esp/components/hc_core.

include("${CMAKE_CURRENT_LIST_DIR}/hc_core.cmake")

add_library(hc_core STATIC ${HC_CORE_SOURCES})
target_include_directories(hc_core PUBLIC "${HC_CORE_INCLUDE_DIR}")
target_compile_features(hc_core PUBLIC cxx_std_17)
target_compile_definitions(hc_core PRIVATE HC_BUILD_ID="${HC_CORE_BUILD_ID}")
