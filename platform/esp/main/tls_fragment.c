/*
 * Copyright (c) 2026 Bernhard Haumacher
 */
// Every TLS client of the application asks the server for records of at most 4 KB (RFC 6066 Max
// Fragment Length), as the PhoneBlock dongle does. With CONFIG_MBEDTLS_DYNAMIC_BUFFER the receive
// buffer is allocated at the size of each record, so a server that honours the extension never
// makes the client find a 16 KB block on a fragmented heap: E-Home's server (apps.haumacher.de)
// and github.com do (measured 2026-11-07), so an image E-Home deploys over the WebSocket arrives in
// 4 KB records.
//
// The receive buffer stays at 16 KB (CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN, the default): GitHub Pages
// (haumacher.github.io, the update source) and release-assets.githubusercontent.com ignore the
// extension and send full 16 KB records (16 401 bytes on the wire, measured 2026-11-07), so the
// manifest, the language packs and every other download from there need it (doc/ota.md "TLS
// records"). A server that ignores the extension changes nothing for its connection.
//
// esp-tls has no hook to adjust its mbedtls_ssl_config, so the call that starts building it,
// mbedtls_ssl_config_defaults(), is wrapped at link time (-Wl,--wrap=mbedtls_ssl_config_defaults,
// CMakeLists.txt): the dongle wraps mbedtls_ssl_setup(), which the IDF's dynamic buffers wrap
// already (components/mbedtls/port/dynamic/esp_ssl_tls.c). esp-tls sets nothing of the fragment
// length afterwards. Only client endpoints are touched; the configuration belongs to the one
// connection esp-tls builds it for.

#include "esp_log.h"
#include "mbedtls/ssl.h"

static const char* const kTag = "hc.tls";

int __real_mbedtls_ssl_config_defaults(mbedtls_ssl_config* conf, int endpoint, int transport, int preset);
int __wrap_mbedtls_ssl_config_defaults(mbedtls_ssl_config* conf, int endpoint, int transport, int preset);

int __wrap_mbedtls_ssl_config_defaults(mbedtls_ssl_config* conf, int endpoint, int transport, int preset) {
	int result = __real_mbedtls_ssl_config_defaults(conf, endpoint, transport, preset);
	if (result == 0 && endpoint == MBEDTLS_SSL_IS_CLIENT) {
		int rc = mbedtls_ssl_conf_max_frag_len(conf, MBEDTLS_SSL_MAX_FRAG_LEN_4096);
		if (rc != 0) {
			// Not fatal: the connection works with the 16 KB records as before.
			ESP_LOGW(kTag, "max fragment length not requested: -0x%04x", (unsigned) -rc);
		}
	}
	return result;
}
