# Copyright (c) 2026 Bernhard Haumacher
#
# The hc::esp platform: clock, log sink, system info, time zone, the event-loop queue other tasks
# post to, the System page's facts (tasks, temperature, chip, NVS statistics; issue #42), the
# diagnostics (reset reason, boot record, core dump, task watchdog; doc/diagnostics.md), the console
# output path (the USB Serial/JTAG driver, the queue and its writer task; issue #46), and the worker
# that runs a job off the loop in a task of its own (the image's store, issue #68), and the planned
# restart that closes the connections first (homecontrol #103), and the heap watch: the heap's lowest
# point and the failed allocations where they happen (homecontrol #110). The NVS settings
# live in hc_settings and the status LED in hc_status_light (both shared with the OTA updater); they
# come along as public requirements.

idf_component_register(
	SRCS "esp_platform.cpp" "loop_queue.cpp" "esp_diagnostics.cpp" "esp_system_info.cpp"
		"console_output.cpp" "esp_worker.cpp" "planned_restart.cpp" "heap_watch.cpp" "esp_memory_watch.cpp"
	INCLUDE_DIRS "include"
	REQUIRES hc_core hc_settings hc_status_light esp_timer esp_system log freertos nvs_flash
	PRIV_REQUIRES espcoredump esp_partition spi_flash esp_driver_tsens app_update
		esp_rom esp_driver_usb_serial_jtag vfs)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
