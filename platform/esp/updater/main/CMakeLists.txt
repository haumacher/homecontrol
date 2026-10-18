# Copyright (c) 2026 Bernhard Haumacher

idf_component_register(
	SRCS "updater.cpp"
	INCLUDE_DIRS "."
	REQUIRES hc_core hc_settings nvs_flash esp_wifi esp_netif esp_event esp_http_client esp_http_server esp_timer app_update
		esp_partition bootloader_support esp_app_format mbedtls esp_system log freertos)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
