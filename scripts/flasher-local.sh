#!/usr/bin/env bash
# Copyright (c) 2026 Bernhard Haumacher
#
# Copies the local esp32c6 build (application and OTA updater) into web/flasher/firmware/esp32c6/
# (not versioned), so that the web flasher can be tested from this checkout:
#
#   scripts/flasher-local.sh
#   cd web/flasher && python3 -m http.server 8000      # then open http://localhost:8000/ in Chrome
#
# Build both first (doc/ota.md):
#   cd platform/esp && idf.py -B build-esp32c6 build
#   cd platform/esp/updater && idf.py -B build-esp32c6 build
# The offsets in web/flasher/manifest.json are checked against the build's flasher_args.json.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${REPO}/platform/esp/build-esp32c6"
UPDATER="${REPO}/platform/esp/updater/build-esp32c6/hc_updater.bin"
TARGET="${REPO}/web/flasher/firmware/esp32c6"
MANIFEST="${REPO}/web/flasher/manifest.json"

for file in "${BUILD}/bootloader/bootloader.bin" "${BUILD}/partition_table/partition-table.bin" \
	"${BUILD}/homecontrol.bin" "${BUILD}/ota_data_ota0.bin" "${BUILD}/flasher_args.json"; do
	if [[ ! -f "${file}" ]]; then
		echo "missing ${file}: build the esp32c6 application first" >&2
		exit 1
	fi
done
if [[ ! -f "${UPDATER}" ]]; then
	echo "missing ${UPDATER}: build the updater first (cd platform/esp/updater && idf.py -B build-esp32c6 build)" >&2
	exit 1
fi

# Every part of the manifest must sit where the build's flash layout puts it.
python3 - "${BUILD}/flasher_args.json" "${MANIFEST}" <<'PY'
import json, os, sys
flash = json.load(open(sys.argv[1]))['flash_files']
by_name = {os.path.basename(path): int(offset, 16) for offset, path in flash.items()}
parts = json.load(open(sys.argv[2]))['builds'][0]['parts']
bad = False
for part in parts:
	name = os.path.basename(part['path'])
	if by_name.get(name) != part['offset']:
		print('manifest offset of %s is %s, the build flashes it at %s' % (
			name, hex(part['offset']), hex(by_name[name]) if name in by_name else 'no offset'), file=sys.stderr)
		bad = True
missing = set(by_name) - {os.path.basename(p['path']) for p in parts}
if missing:
	print('the manifest lacks %s' % ', '.join(sorted(missing)), file=sys.stderr)
	bad = True
sys.exit(1 if bad else 0)
PY

mkdir -p "${TARGET}"
cp "${BUILD}/bootloader/bootloader.bin" "${BUILD}/partition_table/partition-table.bin" \
	"${BUILD}/homecontrol.bin" "${BUILD}/ota_data_ota0.bin" "${UPDATER}" "${TARGET}/"
echo "copied the esp32c6 build (application and updater) to ${TARGET}"
ls -l "${TARGET}"
