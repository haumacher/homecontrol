# Copyright (c) 2026 Bernhard Haumacher
#
# The web server (doc/web.md): esp_http_server with the core's JSON API (hc::WebApi), the live feed
# over WebSocket (hc::LiveFeed), the session and token authentication (hc::Auth) and the UI files,
# compiled into the firmware: tools/gen_ui_files.py turns web/ui/dist and the language packs of
# web/ui/lang into ui_files.cpp in the build directory (ui_files.h).

set(hc_ui_files_cpp "${CMAKE_CURRENT_BINARY_DIR}/ui_files.cpp")

idf_component_register(
	SRCS "web_server.cpp" "${hc_ui_files_cpp}"
	INCLUDE_DIRS "include"
	PRIV_INCLUDE_DIRS "."
	REQUIRES hc_core hc_platform hc_net hc_ota hc_console esp_http_server
	PRIV_REQUIRES hc_storage app_update mbedtls esp_hw_support esp_system lwip log freertos)

get_filename_component(hc_ui_dist "${COMPONENT_DIR}/../../../../web/ui/dist" REALPATH)
get_filename_component(hc_ui_packs_dir "${hc_ui_dist}/../lang" REALPATH)
set(hc_ui_tool "${COMPONENT_DIR}/../../tools/gen_ui_files.py")
file(GLOB_RECURSE hc_ui_sources CONFIGURE_DEPENDS "${hc_ui_dist}/*")
file(GLOB hc_ui_packs CONFIGURE_DEPENDS "${hc_ui_packs_dir}/*.json")
idf_build_get_property(hc_python PYTHON)
add_custom_command(OUTPUT "${hc_ui_files_cpp}"
	COMMAND ${hc_python} "${hc_ui_tool}" "${hc_ui_dist}" "${hc_ui_files_cpp}" "${hc_ui_packs_dir}"
	DEPENDS "${hc_ui_tool}" ${hc_ui_sources} ${hc_ui_packs}
	VERBATIM)
add_custom_target(hc_ui_files DEPENDS "${hc_ui_files_cpp}")
add_dependencies(${COMPONENT_LIB} hc_ui_files)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
