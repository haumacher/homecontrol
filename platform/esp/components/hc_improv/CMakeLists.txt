# Copyright (c) 2026 Bernhard Haumacher
#
# The USB Serial/JTAG port shared by the console and Improv Wi-Fi serial provisioning: a reader task
# splits Improv frames from console input (hc::ImprovReader), the console reads the rest through a
# VFS device (doc/network.md, "Improv / web flasher").

idf_component_register(
	SRCS "serial_mux.cpp"
	INCLUDE_DIRS "include"
	REQUIRES hc_core hc_platform
	PRIV_REQUIRES esp_driver_usb_serial_jtag vfs esp_timer esp_hw_support log freertos)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
