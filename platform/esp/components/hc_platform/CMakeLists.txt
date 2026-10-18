# Copyright (c) 2026 Bernhard Haumacher
#
# The hc::esp platform: clock, log sink, system info, time zone, the status LED, the event-loop
# queue other tasks post to, the System page's facts (tasks, temperature, chip, NVS statistics; issue #42),
# and the diagnostics (reset reason, boot record, core dump, task watchdog;
# doc/diagnostics.md). The NVS settings live in hc_settings (shared with the OTA updater)
# and come along as a public requirement.

idf_component_register(
	SRCS "esp_platform.cpp" "loop_queue.cpp" "esp_status_light.cpp" "esp_diagnostics.cpp" "esp_system_info.cpp"
	INCLUDE_DIRS "include"
	REQUIRES hc_core hc_settings esp_timer esp_system log freertos nvs_flash
	PRIV_REQUIRES esp_driver_gpio espressif__led_strip espcoredump esp_partition spi_flash esp_driver_tsens app_update
		esp_rom)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
