# Copyright (c) 2026 Bernhard Haumacher
#
# The network bring-up: the WiFi station driver behind hc::Connectivity, the mDNS announcement and
# SNTP, WPS with its button (doc/network.md). mdns is Espressif's managed component (main/idf_component.yml).

idf_component_register(
	SRCS "wifi_driver.cpp" "net_services.cpp" "wps_button.cpp"
	INCLUDE_DIRS "include"
	REQUIRES hc_core hc_platform esp_wifi esp_netif esp_event log lwip wpa_supplicant esp_driver_gpio
	PRIV_REQUIRES espressif__mdns)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
