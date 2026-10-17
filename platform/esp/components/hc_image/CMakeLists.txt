# Copyright (c) 2026 Bernhard Haumacher
#
# The runtime image on the device (issue #8, doc/image.md): the console command `image` with the
# download of `image load <url>` (esp_http_client in a task of its own, handed to the event loop).

idf_component_register(
	SRCS "image_console.cpp"
	INCLUDE_DIRS "include"
	REQUIRES hc_core hc_platform
	PRIV_REQUIRES esp_http_client mbedtls freertos log)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
