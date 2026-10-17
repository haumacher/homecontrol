# Copyright (c) 2026 Bernhard Haumacher
#
# The hc::esp platform: clock, log sink, system info, time zone, NVS settings, the status LED and
# the event-loop queue other tasks post to.

idf_component_register(
	SRCS "esp_platform.cpp" "nvs_settings.cpp" "loop_queue.cpp" "esp_status_light.cpp"
	INCLUDE_DIRS "include"
	REQUIRES hc_core esp_timer esp_system log freertos nvs_flash
	PRIV_REQUIRES esp_driver_gpio espressif__led_strip)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
