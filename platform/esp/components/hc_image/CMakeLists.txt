# Copyright (c) 2026 Bernhard Haumacher
#
# The runtime image on the device (issue #8, doc/image.md): the console command `image` with the
# download of `image load <url>` (esp_http_client in a task of its own, handed to the event loop),
# the image staging area behind the updater's image (homecontrol #66, doc/ota.md), and the heap probe of
# the load's phases (homecontrol #70, doc/image.md "Memory of a swap").

idf_component_register(
	SRCS "image_console.cpp" "esp_image_staging.cpp" "esp_load_probe.cpp"
	INCLUDE_DIRS "include"
	REQUIRES hc_core hc_platform esp_partition
	PRIV_REQUIRES mbedtls freertos log bootloader_support esp_mm esp_timer spi_flash heap)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
