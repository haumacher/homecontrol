# Copyright (c) 2026 Bernhard Haumacher
#
# The single source list of the portable core, shared by the host build (core/CMakeLists.txt) and
# the ESP-IDF component (platform/esp/components/hc_core/CMakeLists.txt). Defines:
#   HC_CORE_SOURCES      absolute paths of the core's sources
#   HC_CORE_INCLUDE_DIR  the public include directory
#   hc_core_add_build_id(<target>)  adds the build-time generated build id (git describe) to the target

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
	# The web server base (issue #5, doc/web.md)
	"${HC_CORE_DIR}/src/auth.cpp"
	"${HC_CORE_DIR}/src/demo.cpp"
	"${HC_CORE_DIR}/src/json.cpp"
	"${HC_CORE_DIR}/src/live_feed.cpp"
	"${HC_CORE_DIR}/src/monitor_feed.cpp"
	"${HC_CORE_DIR}/src/log_ring.cpp"
	"${HC_CORE_DIR}/src/output_ring.cpp"
	"${HC_CORE_DIR}/src/sha.cpp"
	"${HC_CORE_DIR}/src/value_json.cpp"
	"${HC_CORE_DIR}/src/web_api.cpp"
	"${HC_CORE_DIR}/src/web_api_system.cpp"
	"${HC_CORE_DIR}/src/web_api_blocks.cpp"
	"${HC_CORE_DIR}/src/websocket.cpp"
	"${HC_CORE_DIR}/src/astro.cpp"
	"${HC_CORE_DIR}/src/time_service.cpp"
	"${HC_CORE_DIR}/src/scheduler.cpp"
)

# The build id (`git describe --always --dirty`) is generated at build time (#36): the custom target
# hc_build_id runs core/cmake/build_id.cmake on every build and rewrites <binary dir>/hc_generated/
# hc_build_id.h only when the id changed, so an incremental build after a commit carries the new id
# without a reconfigure and an unchanged id recompiles nothing. Call for the core's library target.
function(hc_core_add_build_id target)
	set(_hc_gen_dir "${CMAKE_BINARY_DIR}/hc_generated")
	file(MAKE_DIRECTORY "${_hc_gen_dir}")
	add_custom_target(hc_build_id ALL
		BYPRODUCTS "${_hc_gen_dir}/hc_build_id.h"
		COMMAND "${CMAKE_COMMAND}" "-DHC_GIT_DIR=${HC_CORE_DIR}" "-DHC_OUTPUT=${_hc_gen_dir}/hc_build_id.h"
			-P "${HC_CORE_DIR}/cmake/build_id.cmake"
		COMMENT "Build id (git describe)"
		VERBATIM)
	add_dependencies(${target} hc_build_id)
	target_include_directories(${target} PRIVATE "${_hc_gen_dir}")
endfunction()

# KNXnet/IP tunnelling and the KNX adapter (issue #7, doc/knx.md).
list(APPEND HC_CORE_SOURCES
	"${HC_CORE_DIR}/src/ipv4.cpp"
	"${HC_CORE_DIR}/src/knx/address.cpp"
	"${HC_CORE_DIR}/src/knx/cemi.cpp"
	"${HC_CORE_DIR}/src/knx/knxnetip.cpp"
	"${HC_CORE_DIR}/src/knx/knx_settings.cpp"
	"${HC_CORE_DIR}/src/knx/adapter.cpp"
	"${HC_CORE_DIR}/src/knx/group_value.cpp"
	"${HC_CORE_DIR}/src/knx/telegram_log.cpp"
	"${HC_CORE_DIR}/src/knx/tunnel.cpp"
)

# The KNX time master (issue #9, doc/knx.md "Time master").
list(APPEND HC_CORE_SOURCES "${HC_CORE_DIR}/src/knx/time_master.cpp")
# State persistence and short-term history (issue #21, doc/persistence.md).
list(APPEND HC_CORE_SOURCES
	"${HC_CORE_DIR}/src/value_store.cpp"
	"${HC_CORE_DIR}/src/history.cpp"
	"${HC_CORE_DIR}/src/history_archive.cpp"
)
# The rule interpreter (doc/rules.md).
list(APPEND HC_CORE_SOURCES
	"${HC_CORE_DIR}/src/rules/value.cpp"
	"${HC_CORE_DIR}/src/rules/loader.cpp"
	"${HC_CORE_DIR}/src/rules/parameter_store.cpp"
	"${HC_CORE_DIR}/src/rules/printer.cpp"
	"${HC_CORE_DIR}/src/rules/engine.cpp"
)
# Blocks: the block types, the loader of the image's `blocks` and the runtime (issue #60,
# spec/blocks.md, doc/blocks.md). The blocks' arithmetic must give the same bits as E-Home's Java
# simulation (spec/blocks.md 13): no fused multiply-add.
set(HC_CORE_BLOCK_SOURCES
	"${HC_CORE_DIR}/src/blocks/block_type.cpp"
	"${HC_CORE_DIR}/src/blocks/block_set.cpp"
	"${HC_CORE_DIR}/src/blocks/block_runtime.cpp"
	"${HC_CORE_DIR}/src/blocks/room_controller.cpp"
)
list(APPEND HC_CORE_SOURCES ${HC_CORE_BLOCK_SOURCES})
# (Not in the ESP-IDF's script-mode pass that only collects the component's requirements.)
if(NOT CMAKE_SCRIPT_MODE_FILE)
	set_source_files_properties(${HC_CORE_BLOCK_SOURCES} PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
# The runtime image: format, loader, storage, the service that applies it (issue #8, doc/image.md).
list(APPEND HC_CORE_SOURCES
	"${HC_CORE_DIR}/src/json_document.cpp"
	"${HC_CORE_DIR}/src/image/loader.cpp"
	"${HC_CORE_DIR}/src/image/image_body.cpp"
	"${HC_CORE_DIR}/src/image/image_store.cpp"
	"${HC_CORE_DIR}/src/image/image_service.cpp"
	"${HC_CORE_DIR}/src/image/model_view.cpp"
	"${HC_CORE_DIR}/src/image/rules_clock.cpp"
	"${HC_CORE_DIR}/src/image/memory_blob_store.cpp"
)
# External values: HTTP JSON sources as datapoints, Open-Meteo as the first (issue #17, doc/external.md).
list(APPEND HC_CORE_SOURCES "${HC_CORE_DIR}/src/external_values.cpp")
list(APPEND HC_CORE_SOURCES "${HC_CORE_DIR}/src/fetch_body.cpp")
# The Zigbee coordinator's portable half: device database, cluster mapping, adapter (issue #15, doc/zigbee.md).
list(APPEND HC_CORE_SOURCES
	"${HC_CORE_DIR}/src/zigbee/zcl.cpp"
	"${HC_CORE_DIR}/src/zigbee/device_database.cpp"
	"${HC_CORE_DIR}/src/zigbee/cluster_mapping.cpp"
	"${HC_CORE_DIR}/src/zigbee/adapter.cpp"
)
# Diagnostics: the reset reason and the boot facts of the platform in the status (issue #4, doc/diagnostics.md).
list(APPEND HC_CORE_SOURCES "${HC_CORE_DIR}/src/diagnostics.cpp")
# Language packs that ship after the firmware (issue #24, doc/i18n.md).
list(APPEND HC_CORE_SOURCES "${HC_CORE_DIR}/src/language_packs.cpp")
# The E-Home connection: the enrollment and the outbound socket (issue #41, doc/ehome.md).
list(APPEND HC_CORE_SOURCES "${HC_CORE_DIR}/src/ehome_link.cpp")
# The MQTT client: Home Assistant discovery, commands, history export (issue #18, doc/mqtt.md).
list(APPEND HC_CORE_SOURCES
	"${HC_CORE_DIR}/src/mqtt.cpp"
	"${HC_CORE_DIR}/src/mqtt_codec.cpp"
)
# The System page: tasks, memory, temperature, the loop, the link, chip and storage facts (issue #42).
list(APPEND HC_CORE_SOURCES "${HC_CORE_DIR}/src/web_api_diagnostics.cpp")
# Ed25519 signatures: the installation key of signed runtime images and the release key of signed
# update manifests (issue #35, spec/api.md "Signed runtime images", doc/ota.md "Manifest signature").
# Ed25519 itself is the vendored Monocypher 4.0.2 (third_party/monocypher, BSD-2-Clause or CC0,
# https://github.com/LoupVaillant/Monocypher, tag 4.0.2, unmodified src/monocypher.c and
# src/optional/monocypher-ed25519.c), compiled as C; only the core's signature.cpp includes it.
list(APPEND HC_CORE_SOURCES
	"${HC_CORE_DIR}/src/signature.cpp"
	"${HC_CORE_DIR}/src/release_key.cpp"
	"${HC_CORE_DIR}/../third_party/monocypher/monocypher.c"
	"${HC_CORE_DIR}/../third_party/monocypher/optional/monocypher-ed25519.c"
)
set(HC_CORE_PRIVATE_INCLUDE_DIRS
	"${HC_CORE_DIR}/../third_party/monocypher"
	"${HC_CORE_DIR}/../third_party/monocypher/optional"
)

# The release public key the firmware trusts for update manifests (doc/ota.md "Manifest signature"):
# the file HC_RELEASE_KEY names (64 hex digits, white space around them ignored; default the
# committed platform/esp/release-key.pub). A development build points it at the key
# scripts/update-server.py generated (`idf.py -DHC_RELEASE_KEY=<file> ...`). Written at configure
# time into <binary dir>/hc_generated/hc_release_key.h; a change of the file reconfigures.
set(HC_RELEASE_KEY "${HC_CORE_DIR}/../platform/esp/release-key.pub" CACHE FILEPATH
	"The Ed25519 release public key (64 hex digits) the firmware trusts for update manifests")
function(hc_core_add_release_key target)
	get_filename_component(_hc_key_file "${HC_RELEASE_KEY}" ABSOLUTE)
	if(NOT EXISTS "${_hc_key_file}")
		message(FATAL_ERROR "HC_RELEASE_KEY: ${_hc_key_file} does not exist")
	endif()
	file(READ "${_hc_key_file}" _hc_key)
	string(STRIP "${_hc_key}" _hc_key)
	if(NOT _hc_key MATCHES "^[0-9a-fA-F]+$")
		message(FATAL_ERROR "HC_RELEASE_KEY: ${_hc_key_file} does not hold 64 hex digits")
	endif()
	string(LENGTH "${_hc_key}" _hc_key_length)
	if(NOT _hc_key_length EQUAL 64)
		message(FATAL_ERROR "HC_RELEASE_KEY: ${_hc_key_file} does not hold 64 hex digits")
	endif()
	string(TOLOWER "${_hc_key}" _hc_key)
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_hc_key_file}")
	set(_hc_gen_dir "${CMAKE_BINARY_DIR}/hc_generated")
	file(MAKE_DIRECTORY "${_hc_gen_dir}")
	set(_hc_content "// Generated at configure time by core/hc_core.cmake from ${_hc_key_file}; do not edit.\n#define HC_RELEASE_KEY_HEX \"${_hc_key}\"\n")
	set(_hc_old "")
	if(EXISTS "${_hc_gen_dir}/hc_release_key.h")
		file(READ "${_hc_gen_dir}/hc_release_key.h" _hc_old)
	endif()
	if(NOT _hc_old STREQUAL _hc_content)
		file(WRITE "${_hc_gen_dir}/hc_release_key.h" "${_hc_content}")
	endif()
	target_include_directories(${target} PRIVATE "${_hc_gen_dir}")
	message(STATUS "Release key: ${_hc_key_file}")
endfunction()
