# Copyright (c) 2026 Bernhard Haumacher
#
# Settings of the application only (the updater does not build these components, and the IDF warns
# about defaults for symbols a build does not know). Shared settings are in sdkconfig.defaults.

# The web server (doc/web.md): WebSocket support for /ws; request headers up to 1.25 KB (the default
# 512 B is exceeded by a browser's Cookie and User-Agent lines alone; a signed image's
# X-Image-Signature adds 147 bytes, issue #35; one scratch buffer per server); sockets for the
# server's 10 connections next to mDNS, SNTP, the update check and the server's own control socket.
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_MAX_REQ_HDR_LEN=1280

# Diagnostics (issue #4, doc/diagnostics.md).
# The panic handler writes an ELF core dump to the `coredump` partition (partitions-<size>.csv); the
# application reports its summary at the next boot, `coredump status|clear` on the console, the raw
# dump for `esp-coredump info_corefile` over GET /api/coredump. A new crash overwrites an old dump.
CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y
CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y
CONFIG_ESP_COREDUMP_CHECKSUM_CRC32=y
# The task watchdog watches the event-loop task (fed every loop turn) next to the idle task: a loop
# stuck for 10 s panics, which leaves a core dump and the reason "task watchdog" for the next boot.
CONFIG_ESP_TASK_WDT_EN=y
CONFIG_ESP_TASK_WDT_INIT=y
CONFIG_ESP_TASK_WDT_TIMEOUT_S=10
CONFIG_ESP_TASK_WDT_PANIC=y
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y
# uxTaskGetSystemState() for the console's task list (name, priority, stack high-water mark).
CONFIG_FREERTOS_USE_TRACE_FACILITY=y

# The E-Home connection (issue #41, doc/ehome.md): the event loop sends on the WebSocket while the client
# task waits for the loop to take a received message, so sending must not need the client's main lock.
CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=y
# TLS memory for the E-Home connection next to the other TLS clients (OTA check, HTTP fetcher, MQTT): the
# heap's largest free block (about 24 KB on a busy C6) rules out the static 16 KB + 4 KB record buffers per
# connection. Dynamic buffers allocate each record buffer at the size of the record and free it after use;
# the configuration's certificate data is freed after the handshake; the peer's certificate is not kept
# after the handshake (about 4 KB; IDF 5.3's counterpart of a "free peer certificate" option).
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y
CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA=y
CONFIG_MBEDTLS_DYNAMIC_FREE_CA_CERT=y
# CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE is not set

# The System page's CPU share per task (issue #42): FreeRTOS run-time statistics on esp_timer's
# microseconds (one esp_timer_get_time() per context switch, no extra timer), counted in 64 bits:
# the IDF's default 32-bit counter wraps after 71 minutes and the shares become nonsense.
CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y
CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER=y
CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64=y

# The system event task runs the WiFi and IP handlers of hc_net, which log through stdio into the
# console ring (doc/diagnostics.md "Console output"); the IDF's 2304 bytes left about 1 KB in steady
# state and overflowed once at a boot with a USB host attached (a stack protection fault in sys_evt
# during the WiFi start, 2026-10-18, issue #48). 1.3 KB more RAM buys the margin.
CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=3584
