# Copyright (c) 2026 Bernhard Haumacher
#
# The web server (doc/web.md): esp_http_server with the core's JSON API (hc::WebApi), the live feed
# over WebSocket (hc::LiveFeed), the session and token authentication (hc::Auth) and the UI files
# from the LittleFS image in the storage partition, mounted by hc_storage.

idf_component_register(
	SRCS "web_server.cpp"
	INCLUDE_DIRS "include"
	REQUIRES hc_core hc_platform hc_net hc_ota hc_console esp_http_server
	PRIV_REQUIRES hc_storage app_update mbedtls esp_hw_support esp_system lwip log freertos)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
