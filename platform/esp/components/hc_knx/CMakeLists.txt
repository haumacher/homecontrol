# Copyright (c) 2026 Bernhard Haumacher
#
# KNXnet/IP tunnelling on the device (issue #7, doc/knx.md): hc::UdpSocket over lwIP's BSD sockets
# (the socket calls are platform/common/src/bsd_udp.cpp, shared with the POSIX build) with a reader
# task that posts through the LoopQueue, the KnxService that owns the core's tunnel, adapter and
# telegram log, and the body of the console command `knx`.

idf_component_register(
	SRCS "esp_udp_socket.cpp" "knx_service.cpp" "knx_console.cpp" "../../../common/src/bsd_udp.cpp"
	INCLUDE_DIRS "include" "../../../common/include"
	REQUIRES hc_core hc_platform
	PRIV_REQUIRES lwip freertos log)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
