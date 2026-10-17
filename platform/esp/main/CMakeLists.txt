# Copyright (c) 2026 Bernhard Haumacher

idf_component_register(
	SRCS "main.cpp" "esp_platform.cpp"
	INCLUDE_DIRS "."
	REQUIRES hc_core esp_timer esp_system log freertos)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
