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
	"${HC_CORE_DIR}/src/sha.cpp"
	"${HC_CORE_DIR}/src/value_json.cpp"
	"${HC_CORE_DIR}/src/web_api.cpp"
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
)
# The rule interpreter (doc/rules.md).
list(APPEND HC_CORE_SOURCES
	"${HC_CORE_DIR}/src/rules/value.cpp"
	"${HC_CORE_DIR}/src/rules/loader.cpp"
	"${HC_CORE_DIR}/src/rules/parameter_store.cpp"
	"${HC_CORE_DIR}/src/rules/printer.cpp"
	"${HC_CORE_DIR}/src/rules/engine.cpp"
)
# The runtime image: format, loader, storage, the service that applies it (issue #8, doc/image.md).
list(APPEND HC_CORE_SOURCES
	"${HC_CORE_DIR}/src/json_document.cpp"
	"${HC_CORE_DIR}/src/image/loader.cpp"
	"${HC_CORE_DIR}/src/image/image_store.cpp"
	"${HC_CORE_DIR}/src/image/image_service.cpp"
	"${HC_CORE_DIR}/src/image/model_view.cpp"
	"${HC_CORE_DIR}/src/image/rules_clock.cpp"
	"${HC_CORE_DIR}/src/image/memory_blob_store.cpp"
)
# External values: HTTP JSON sources as datapoints, Open-Meteo as the first (issue #17, doc/external.md).
list(APPEND HC_CORE_SOURCES "${HC_CORE_DIR}/src/external_values.cpp")
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
# The MQTT client: Home Assistant discovery, commands, history export (issue #18, doc/mqtt.md).
list(APPEND HC_CORE_SOURCES
	"${HC_CORE_DIR}/src/mqtt.cpp"
	"${HC_CORE_DIR}/src/mqtt_codec.cpp"
)
