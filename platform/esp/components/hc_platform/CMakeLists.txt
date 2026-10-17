# Copyright (c) 2026 Bernhard Haumacher
#
# The hc::esp platform: clock, log sink, system info, time zone, the status LED and the event-loop
# queue other tasks post to. The NVS settings live in hc_settings (shared with the OTA updater)
# and come along as a public requirement.

idf_component_register(
	SRCS "esp_platform.cpp" "loop_queue.cpp" "esp_status_light.cpp"
	INCLUDE_DIRS "include"
	REQUIRES hc_core hc_settings esp_timer esp_system log freertos nvs_flash
	PRIV_REQUIRES esp_driver_gpio espressif__led_strip)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
