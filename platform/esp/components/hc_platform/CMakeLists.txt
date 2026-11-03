# Copyright (c) 2026 Bernhard Haumacher
#
# The hc::esp platform: clock, log sink, system info, time zone, the status LED, the event-loop
# queue other tasks post to, the System page's facts (tasks, temperature, chip, NVS statistics; issue #42),
# the diagnostics (reset reason, boot record, core dump, task watchdog;
# doc/diagnostics.md), and the console output path (the USB Serial/JTAG driver, the queue and its
# writer task; issue #46), and the worker that runs a job off the loop in a task of its own (the
# image's store, issue #68). The NVS settings live in hc_settings (shared with the OTA updater)
# and come along as a public requirement.

idf_component_register(
	SRCS "esp_platform.cpp" "loop_queue.cpp" "esp_status_light.cpp" "esp_diagnostics.cpp" "esp_system_info.cpp"
		"console_output.cpp" "esp_worker.cpp"
	INCLUDE_DIRS "include"
	REQUIRES hc_core hc_settings esp_timer esp_system log freertos nvs_flash
	PRIV_REQUIRES esp_driver_gpio espressif__led_strip espcoredump esp_partition spi_flash esp_driver_tsens app_update
		esp_rom esp_driver_usb_serial_jtag vfs)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
