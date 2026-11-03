# Copyright (c) 2026 Bernhard Haumacher
#
# The runtime image on the device (issue #8, doc/image.md): the console command `image` with the
# download of `image load <url>` (esp_http_client in a task of its own, handed to the event loop),
# and the image staging area behind the updater's image (homecontrol #66, doc/ota.md).

idf_component_register(
	SRCS "image_console.cpp" "esp_image_staging.cpp"
	INCLUDE_DIRS "include"
	REQUIRES hc_core hc_platform esp_partition
	PRIV_REQUIRES esp_http_client mbedtls freertos log bootloader_support esp_mm esp_timer spi_flash)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
