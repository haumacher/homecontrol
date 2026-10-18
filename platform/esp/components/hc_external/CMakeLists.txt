# Copyright (c) 2026 Bernhard Haumacher
#
# External values on the device (issue #17, doc/external.md): hc::HttpFetcher on esp_http_client with the
# IDF certificate bundle (a short-lived worker task per fetch, the result posted through the LoopQueue),
# the ExternalService that owns it with the core's ExternalValues, and the console command `external`.

idf_component_register(
	SRCS "esp_http_fetcher.cpp" "external_service.cpp" "on_demand_fetcher.cpp"
	INCLUDE_DIRS "include"
	REQUIRES hc_core hc_platform hc_console
	PRIV_REQUIRES esp_http_client mbedtls freertos log)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
