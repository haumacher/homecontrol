# Copyright (c) 2026 Bernhard Haumacher

idf_component_register(
	SRCS "main.cpp" "nothrow_new.cpp" "tls_fragment.c"
	INCLUDE_DIRS "."
	REQUIRES mbedtls hc_core hc_platform hc_net hc_console hc_improv hc_ota hc_web hc_knx hc_image hc_storage hc_external hc_i18n hc_mqtt hc_zigbee hc_ehome nvs_flash esp_system log freertos
	# nothrow_new.cpp replaces global allocation functions and tls_fragment.c wraps mbedtls_ssl_config_defaults:
	# nothing references them by name.
	WHOLE_ARCHIVE)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)

# Every TLS client asks for 4 KB records (tls_fragment.c): esp-tls has no hook into its
# mbedtls_ssl_config, so mbedtls_ssl_config_defaults() is wrapped (mbedtls_ssl_setup() is wrapped by the
# IDF's dynamic buffers already).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mbedtls_ssl_config_defaults")
