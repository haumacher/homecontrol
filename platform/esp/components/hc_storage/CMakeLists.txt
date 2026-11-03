# Copyright (c) 2026 Bernhard Haumacher
#
# The LittleFS `storage` partition (doc/persistence.md): its one mount at /storage, shared by the web
# server (which removes the UI files an older firmware left in /storage/ui) and the BlobStore (/storage/state), and hc::esp::LittleFsBlobStore
# with the `storage` console command. joltwallet/littlefs is a managed component
# (main/idf_component.yml).

idf_component_register(
	SRCS "storage.cpp" "littlefs_blob_store.cpp"
	INCLUDE_DIRS "include"
	REQUIRES hc_core hc_console
	PRIV_REQUIRES joltwallet__littlefs vfs log)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
