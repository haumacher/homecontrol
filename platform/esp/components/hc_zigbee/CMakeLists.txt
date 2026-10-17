# Copyright (c) 2026 Bernhard Haumacher
#
# The Zigbee coordinator on the device (issue #15, doc/zigbee.md): the esp-zigbee-lib (ZBOSS) task,
# the core's ZigbeeAdapter fed through the LoopQueue, the device database in the zb_db NVS
# partition, and the body of the console command `zigbee`. On a chip without an 802.15.4 radio (or
# with HC_ZIGBEE_ENABLED off) the same API is a stub that says Zigbee is not available.
#
# The requirements depend on the target only: the IDF expands them before the configuration
# exists (CONFIG_ values are empty then). The SDK is a managed component of main for exactly
# these targets (main/idf_component.yml); the sources follow HC_ZIGBEE_ENABLED.

set(hc_zigbee_targets esp32c6 esp32h2)
set(hc_zigbee_priv)
if(IDF_TARGET IN_LIST hc_zigbee_targets)
	set(hc_zigbee_priv espressif__esp-zigbee-lib espressif__esp-zboss-lib nvs_flash esp_partition esp_coex esp_wifi
		freertos log)
endif()

if(CONFIG_HC_ZIGBEE_ENABLED)
	if(NOT CONFIG_ZB_ENABLED OR NOT CONFIG_ZB_ZCZR OR NOT CONFIG_ZB_RADIO_NATIVE)
		message(FATAL_ERROR "HC_ZIGBEE_ENABLED needs the esp-zigbee-lib as coordinator on the native radio: "
			"CONFIG_ZB_ENABLED, CONFIG_ZB_ZCZR and CONFIG_ZB_RADIO_NATIVE (sdkconfig.defaults.app.esp32c6); "
			"delete the build directory's sdkconfig after changing the defaults")
	endif()
	set(hc_zigbee_srcs "zigbee_service.cpp" "zigbee_console.cpp" "nvs_blob_store.cpp")
else()
	set(hc_zigbee_srcs "zigbee_stub.cpp")
endif()

idf_component_register(
	SRCS ${hc_zigbee_srcs}
	INCLUDE_DIRS "include"
	REQUIRES hc_core hc_platform
	PRIV_REQUIRES ${hc_zigbee_priv})

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
