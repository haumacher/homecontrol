# Copyright (c) 2026 Bernhard Haumacher

idf_component_register(
	SRCS "main.cpp"
	INCLUDE_DIRS "."
	REQUIRES hc_core hc_platform hc_net hc_console hc_improv hc_ota hc_web hc_knx hc_image hc_storage hc_external hc_i18n hc_mqtt hc_zigbee nvs_flash esp_system log freertos)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
