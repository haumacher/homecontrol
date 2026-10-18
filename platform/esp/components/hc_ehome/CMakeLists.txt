# Copyright (c) 2026 Bernhard Haumacher
#
# The E-Home connection on the device (issue #41, doc/ehome.md): hc::WebSocketClient on the managed component
# esp_websocket_client (wss:// with the IDF certificate bundle), its events handed to the event loop through the
# LoopQueue, and the EhomeService that owns it with the enrollment's on-demand HTTP client and the core's EhomeLink.

idf_component_register(
	SRCS "esp_ws_client.cpp" "ehome_service.cpp"
	INCLUDE_DIRS "include"
	REQUIRES hc_core hc_platform hc_external hc_web
	PRIV_REQUIRES espressif__esp_websocket_client mbedtls esp-tls freertos log)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
