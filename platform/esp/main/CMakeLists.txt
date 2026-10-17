# Copyright (c) 2026 Bernhard Haumacher

idf_component_register(
	SRCS "main.cpp"
	INCLUDE_DIRS "."
	REQUIRES hc_core hc_platform hc_net hc_console hc_improv hc_ota nvs_flash esp_system log freertos)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
