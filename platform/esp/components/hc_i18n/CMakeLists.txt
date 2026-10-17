# Copyright (c) 2026 Bernhard Haumacher
#
# Language packs that ship after the firmware (issue #24, doc/i18n.md): the core's LanguagePacks over an
# EspHttpFetcher that exists only while a sync runs, the packs in the LittleFS blob store, and the console
# command `lang`.

idf_component_register(
	SRCS "i18n_service.cpp"
	INCLUDE_DIRS "include"
	REQUIRES hc_core hc_platform hc_console hc_external
	PRIV_REQUIRES freertos log)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
