# Copyright (c) 2026 Bernhard Haumacher
#
# The line console on the USB Serial/JTAG port (linenoise and the IDF command registry, reading the
# input device of hc_improv, which shares the port with Improv). The command table is in
# console.cpp; issue #4 adds its commands there.

idf_component_register(
	SRCS "console.cpp"
	INCLUDE_DIRS "include"
	REQUIRES hc_core hc_platform hc_net hc_ota
	PRIV_REQUIRES console esp_system log freertos)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
