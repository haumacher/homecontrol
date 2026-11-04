# Copyright (c) 2026 Bernhard Haumacher
#
# The board's status LED (hc::esp::EspStatusLight, menuconfig "HomeControl status LED"): none, a
# plain GPIO LED or one WS2812 pixel over RMT (espressif/led_strip, listed in the manifest of each
# project that builds it). Shared by the application (through hc_platform) and the OTA updater
# (doc/ota.md "The updater").

idf_component_register(
	SRCS "esp_status_light.cpp"
	INCLUDE_DIRS "include"
	REQUIRES hc_core
	PRIV_REQUIRES esp_driver_gpio espressif__led_strip log)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
