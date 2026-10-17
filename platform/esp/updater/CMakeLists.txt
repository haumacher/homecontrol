# Copyright (c) 2026 Bernhard Haumacher
#
# The OTA updater: the small factory app of the asymmetric partition layout (doc/ota.md). It
# downloads the image the application asked for into the application partition (ota_0) and boots
# it. Its own build directory per target:
#   idf.py -B build-esp32c6 set-target esp32c6 && idf.py -B build-esp32c6 build
# It is updated only over USB (doc/ota.md).

cmake_minimum_required(VERSION 3.16)

set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")

# The application's defaults (flash clock, console, transmit power, rollback) first, then the
# updater's own, then the flash size and partition table fragment.
include("${CMAKE_CURRENT_LIST_DIR}/../hc_project.cmake")
set(SDKCONFIG_DEFAULTS "${HC_ESP_DIR}/sdkconfig.defaults" "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
hc_add_flash_defaults()

# Only what the updater needs: the core (for the update instruction and the decision table) and the
# NVS settings it shares with the application.
set(EXTRA_COMPONENT_DIRS "${HC_ESP_DIR}/components/hc_core" "${HC_ESP_DIR}/components/hc_settings")
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hc_updater)

hc_check_partition_table()

# The IDF checks an app against every app partition (and only warns while one of them fits);
# the updater must fit the factory partition.
partition_table_add_check_size_target(hc_updater_check_size
	DEPENDS gen_project_binary
	BINARY_PATH "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin"
	PARTITION_TYPE app PARTITION_SUBTYPE factory)
add_dependencies(app hc_updater_check_size)
