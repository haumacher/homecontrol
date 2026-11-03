# Copyright (c) 2026 Bernhard Haumacher
#
# Settings of the application only (the updater does not build these components, and the IDF warns
# about defaults for symbols a build does not know). Shared settings are in sdkconfig.defaults.

# The web server (doc/web.md): WebSocket support for /ws; request headers up to 1.25 KB (the default
# 512 B is exceeded by a browser's Cookie and User-Agent lines alone; a signed image's
# X-Image-Signature adds 147 bytes, issue #35; one scratch buffer per server). The server's 7
# connections (WebServer::kMaxSockets) and its listen and control sockets take 9 of lwIP's 16
# (CONFIG_LWIP_MAX_SOCKETS in sdkconfig.defaults), next to mDNS, SNTP, the KNX tunnel, the E-Home
# connection, MQTT and the fetchers.
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

# Memory (issue #45; the application only: the updater keeps the IDF's WiFi defaults, its download
# has the RAM to itself and its throughput sets the update's duration, doc/ota.md).
#
# WiFi code in IRAM: the receive path and the driver's hot functions stay there (RX_IRAM_OPT more
# than 17 KB, IRAM_OPT more than 10 KB); the modem-sleep functions (about 7.3 KB: they only lower the
# current in modem sleep, which matters on batteries, not on mains) and the extra set (more than
# 5 KB) run from flash. Measure the OTA throughput against the build before (issue #45).
# CONFIG_ESP_WIFI_SLP_IRAM_OPT is not set
# CONFIG_ESP_WIFI_EXTRA_IRAM_OPT is not set
CONFIG_ESP_WIFI_IRAM_OPT=y
CONFIG_ESP_WIFI_RX_IRAM_OPT=y
# WiFi buffers: 6 static receive buffers of about 1.6 KB (10 by default: 6.4 KB), as many as the
# AMPDU receive window (CONFIG_ESP_WIFI_RX_BA_WIN 6, the IDF's advice); at most 16 dynamic receive
# and 16 dynamic transmit buffers (32 each by default), which caps a burst at about 26 KB of heap
# instead of 51 KB: a page load over every web server socket with lwIP's 5760-byte TCP windows next
# to a TLS session could exhaust the heap before.
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=6
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=16
CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=16

# Task stacks of the IDF (issue #45): the size, what the C6 used of it (2026-11-03: firmware 5ceb29f+
# on IDF 5.5.6, 45 min up, E-Home over wss, the KNX tunnel up, one Zigbee device; the System page's
# stack_free subtracted from the size), and why the margin. The application's own tasks have theirs
# next to their constants. sys_evt stays at 3584 (above; issue #48 watches it).
# esp_timer: 3584 -> 2560. It runs the esp_timer callbacks (the WiFi driver's and mDNS's timers;
# the application has none): 3588 B reported free, no measurable use; a callback that logs needs
# the ROM printf's frame of 1.25 KB, reserved at once and invisible to the high-water mark (#48).
CONFIG_ESP_TIMER_TASK_STACK_SIZE=2560
# tiT (lwIP): 3072 -> 2560, 688 B used: packets, DHCP, DNS, SNTP and the netif callbacks, all run
# in the measurement; the margin covers a log line's ROM printf frame (1.25 KB, invisible to the
# high-water mark, #48).
CONFIG_LWIP_TCPIP_TASK_STACK_SIZE=2560
# mdns: 4096 -> 3584, 1712 B used (2120 B on 2026-10-18): answering queries and the announcements
# were measured, a name conflict (probing anew under another name) was not; 1.4 KB left over the
# older figure.
CONFIG_MDNS_TASK_STACK_SIZE=3584
# Tmr Svc (FreeRTOS software timers): kept at 2048, 260 B used: its callbacks are the IDF's own
# and short, but one that logs needs the ROM printf's frame of 1.25 KB (#48).
CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=2048
