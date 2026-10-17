# Copyright (c) 2026 Bernhard Haumacher
#
# The NVS settings (hc::esp::NvsSettings, namespace "hc"): shared by the application (through
# hc_platform) and the OTA updater (platform/esp/updater), which reads the WiFi credentials and the
# update instruction from the same namespace.

idf_component_register(
	SRCS "nvs_settings.cpp"
	INCLUDE_DIRS "include"
	REQUIRES hc_core nvs_flash
	PRIV_REQUIRES log)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
