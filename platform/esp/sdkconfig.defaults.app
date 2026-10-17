# Copyright (c) 2026 Bernhard Haumacher
#
# Settings of the application only (the updater does not build these components, and the IDF warns
# about defaults for symbols a build does not know). Shared settings are in sdkconfig.defaults.

# The web server (doc/web.md): WebSocket support for /ws; request headers up to 1 KB (the default
# 512 B is exceeded by a browser's Cookie and User-Agent lines alone); sockets for the server's 10
# connections next to mDNS, SNTP, the update check and the server's own control socket.
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024
