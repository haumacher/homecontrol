#!/usr/bin/env bash
# Copyright (c) 2026 Bernhard Haumacher
#
# Copies the local esp32c6 build into web/flasher/firmware/esp32c6/ (not versioned), so that the
# web flasher can be tested from this checkout:
#
#   scripts/flasher-local.sh
#   cd web/flasher && python3 -m http.server 8000      # then open http://localhost:8000/ in Chrome
#
# Build first: cd platform/esp && idf.py -B build-esp32c6 build

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${REPO}/platform/esp/build-esp32c6"
TARGET="${REPO}/web/flasher/firmware/esp32c6"

for file in bootloader/bootloader.bin partition_table/partition-table.bin homecontrol.bin; do
	if [[ ! -f "${BUILD}/${file}" ]]; then
		echo "missing ${BUILD}/${file}: build the esp32c6 firmware first" >&2
		exit 1
	fi
done

mkdir -p "${TARGET}"
cp "${BUILD}/bootloader/bootloader.bin" "${BUILD}/partition_table/partition-table.bin" \
	"${BUILD}/homecontrol.bin" "${TARGET}/"
echo "copied the esp32c6 build to ${TARGET}"
ls -l "${TARGET}"
