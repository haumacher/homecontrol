# Copyright (c) 2026 Bernhard Haumacher
#
# The ESP-IDF project. Each target builds in its own directory:
#   idf.py -B build-esp32c6 set-target esp32c6 && idf.py -B build-esp32c6 build
# The OTA updater is a separate project in updater/ (doc/ota.md); build it into the directory of
# the same name there (updater/build-esp32c6) to get a complete flash_args.

cmake_minimum_required(VERSION 3.16)

# Keep the generated sdkconfig inside the build directory, so that build-esp32c6 and
# build-esp32s3 do not overwrite each other's configuration. Settings belong in sdkconfig.defaults.
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")

# Flash size, partition table and app version (shared with the updater).
include("${CMAKE_CURRENT_LIST_DIR}/hc_project.cmake")
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults" "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.app")
hc_add_flash_defaults()
hc_add_extra_ca_defaults()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The managed components (main/idf_component.yml) are pinned per target: the component manager
# records the target in its lock file and would rewrite a shared one on every target switch.
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${IDF_TARGET}")
project(homecontrol)

hc_check_partition_table()
hc_check_extra_ca()

# ---- The OTA layout in the flash targets ------------------------------------------------------
#
# The IDF flashes an app to the "default boot partition", which is the factory partition when there
# is one - here the updater. The application belongs in ota_0: rewrite the entries of `flash` and
# `app-flash` (and so flash_args, app-flash_args and flasher_args.json). `flash` also writes an
# otadata image that selects ota_0 (instead of a blank one, which would boot the updater first) and
# the updater from the updater's build directory of the same name.

partition_table_get_partition_info(hc_factory_offset "--partition-type app --partition-subtype factory" "offset")
partition_table_get_partition_info(hc_ota0_offset "--partition-type app --partition-subtype ota_0" "offset")
partition_table_get_partition_info(hc_otadata_offset "--partition-type data --partition-subtype ota" "offset")
partition_table_get_partition_info(hc_otadata_size "--partition-type data --partition-subtype ota" "size")
if(NOT hc_factory_offset OR NOT hc_ota0_offset OR NOT hc_otadata_offset)
	message(FATAL_ERROR "${HC_PARTITION_CSV} lacks the updater (factory), ota_0 or otadata partition")
endif()

idf_build_get_property(hc_python PYTHON)
set(hc_otadata_bin "${CMAKE_BINARY_DIR}/ota_data_ota0.bin")
add_custom_command(OUTPUT "${hc_otadata_bin}"
	COMMAND ${hc_python} "${HC_ESP_DIR}/tools/gen_otadata.py" ${hc_otadata_size} "${hc_otadata_bin}"
	DEPENDS "${HC_ESP_DIR}/tools/gen_otadata.py" "${HC_PARTITION_CSV}"
	VERBATIM)
add_custom_target(hc_otadata ALL DEPENDS "${hc_otadata_bin}")
add_dependencies(flash hc_otadata)

# The updater's build directory has the same name as this one (build-esp32c6 <-> updater/build-esp32c6).
get_filename_component(hc_build_name "${CMAKE_BINARY_DIR}" NAME)
set(hc_updater_bin "${HC_ESP_DIR}/updater/${hc_build_name}/hc_updater.bin")
file(RELATIVE_PATH hc_updater_bin_rel "${CMAKE_BINARY_DIR}" "${hc_updater_bin}")
if(NOT EXISTS "${hc_updater_bin}")
	message(STATUS "OTA updater not built yet (${hc_updater_bin}): flash_args will name it; build it with "
		"'idf.py -B ${hc_build_name} build' in platform/esp/updater (same HC_FLASH_SIZE) before flashing")
endif()

# Rewrites one flash target: the image at the factory offset (the application; the IDF names it with
# a generator expression) moves to ota_0, the blank otadata becomes ours, and (for `flash`) the
# updater goes to the factory offset.
function(hc_rewrite_flash_target target with_extras)
	get_target_property(images ${target} IMAGES)
	get_target_property(files ${target} FLASH_FILE)
	get_target_property(entries ${target} FLASH_ENTRY)
	set(moved FALSE)
	set(new_images)
	foreach(image IN LISTS images)
		if(image MATCHES "^${hc_factory_offset} (.*)$")
			set(image "${hc_ota0_offset} ${CMAKE_MATCH_1}")
			set(moved TRUE)
		elseif(with_extras AND image STREQUAL "${hc_otadata_offset} ota_data_initial.bin")
			set(image "${hc_otadata_offset} ota_data_ota0.bin")
		endif()
		list(APPEND new_images "${image}")
	endforeach()
	if(NOT moved)
		message(FATAL_ERROR "flash target ${target} has no image at ${hc_factory_offset} to move to ota_0 (IDF changed?)")
	endif()
	list(TRANSFORM files REPLACE "^\"${hc_factory_offset}\" : " "\"${hc_ota0_offset}\" : ")
	list(TRANSFORM entries REPLACE "\"offset\" : \"${hc_factory_offset}\"" "\"offset\" : \"${hc_ota0_offset}\"")
	if(with_extras)
		list(TRANSFORM files REPLACE "\"ota_data_initial.bin\"$" "\"ota_data_ota0.bin\"")
		list(TRANSFORM entries REPLACE "\"file\" : \"ota_data_initial.bin\"" "\"file\" : \"ota_data_ota0.bin\"")
		list(APPEND new_images "${hc_factory_offset} ${hc_updater_bin_rel}")
		list(APPEND files "\"${hc_factory_offset}\" : \"${hc_updater_bin_rel}\"")
		list(APPEND entries "\"updater\" : { \"offset\" : \"${hc_factory_offset}\", \"file\" : \"${hc_updater_bin_rel}\", \"encrypted\" : \"false\" }")
	endif()
	set_target_properties(${target} PROPERTIES IMAGES "${new_images}" FLASH_FILE "${files}" FLASH_ENTRY "${entries}")
endfunction()

# ---- The web UI image (doc/web.md) -----------------------------------------------------------
#
# web/ui/dist, gzip-compressed with its content hash (tools/gen_ui_image.py), becomes storage.bin, a
# LittleFS image of the storage partition; `flash` writes it too (flash_args, flasher_args.json).
# littlefs_create_partition_image comes with joltwallet/littlefs and installs littlefs-python into a
# venv in the build directory on first use.

get_filename_component(hc_ui_dist "${HC_ESP_DIR}/../../web/ui/dist" REALPATH)
file(GLOB_RECURSE hc_ui_files CONFIGURE_DEPENDS "${hc_ui_dist}/*")
set(hc_ui_stage "${CMAKE_BINARY_DIR}/ui_image")
add_custom_command(OUTPUT "${hc_ui_stage}/ui/VERSION"
	COMMAND ${hc_python} "${HC_ESP_DIR}/tools/gen_ui_image.py" "${hc_ui_dist}" "${hc_ui_stage}"
	DEPENDS "${HC_ESP_DIR}/tools/gen_ui_image.py" ${hc_ui_files}
	VERBATIM)
add_custom_target(hc_ui_stage DEPENDS "${hc_ui_stage}/ui/VERSION")
littlefs_create_partition_image(storage "${hc_ui_stage}" FLASH_IN_PROJECT DEPENDS hc_ui_stage)

hc_rewrite_flash_target(flash TRUE)
hc_rewrite_flash_target(app-flash FALSE)

# The IDF checks an app against every app partition and only warns while one of them fits; the
# application must fit ota_0.
partition_table_add_check_size_target(hc_app_check_size
	DEPENDS gen_project_binary
	BINARY_PATH "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin"
	PARTITION_TYPE app PARTITION_SUBTYPE ota_0)
add_dependencies(app hc_app_check_size)

# The language packs (doc/i18n.md): gen_ui_image.py takes them from web/ui/lang; a changed pack
# restages the UI image.
file(GLOB hc_ui_packs CONFIGURE_DEPENDS "${hc_ui_dist}/../lang/*.json")
add_custom_command(OUTPUT "${hc_ui_stage}/ui/VERSION" APPEND DEPENDS ${hc_ui_packs})
