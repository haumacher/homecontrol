# Copyright (c) 2026 Bernhard Haumacher
#
# The application's side of the OTA update (doc/ota.md): the periodic check of the update source,
# the hand-over to the updater, and the confirmation of a new image after a successful start.

idf_component_register(
	SRCS "ota_service.cpp"
	INCLUDE_DIRS "include"
	REQUIRES hc_core hc_platform
	PRIV_REQUIRES app_update esp_partition esp_http_client mbedtls esp_system log freertos)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
