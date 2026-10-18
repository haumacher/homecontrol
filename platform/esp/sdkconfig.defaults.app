# Copyright (c) 2026 Bernhard Haumacher
#
# Settings of the application only (the updater does not build these components, and the IDF warns
# about defaults for symbols a build does not know). Shared settings are in sdkconfig.defaults.

# The web server (doc/web.md): WebSocket support for /ws; request headers up to 1 KB (the default
# 512 B is exceeded by a browser's Cookie and User-Agent lines alone); sockets for the server's 10
# connections next to mDNS, SNTP, the update check and the server's own control socket.
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024

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
# The System page's CPU share per task (issue #42): FreeRTOS run-time statistics on esp_timer's
# microseconds (one esp_timer_get_time() per context switch, no extra timer), counted in 64 bits:
# the IDF's default 32-bit counter wraps after 71 minutes and the shares become nonsense.
CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y
CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER=y
CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64=y
