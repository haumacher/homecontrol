#!/usr/bin/env python3
# Copyright (c) 2026 Bernhard Haumacher
#
# Generates the partition tables of the asymmetric OTA layout (doc/ota.md) for every supported
# flash size from the one table of sizes below, and checks the committed CSVs against it.
#
#   platform/esp/tools/gen_partitions.py           # (re)write platform/esp/partitions-<size>.csv
#   platform/esp/tools/gen_partitions.py --check   # fail if a committed CSV differs (CI)
#
# Change a size here, regenerate, and update the table in doc/ota.md and doc/hardware.md.

import argparse
import os
import sys

KB = 1024
MB = 1024 * KB

# Fixed head of every layout. NVS stays where the single-app table had it (0x9000, 24 KB), so the
# stored settings survive the switch to the OTA layout; phy_init follows it as before.
NVS = (0x9000, 0x6000)
PHY_INIT = (0xF000, 0x1000)
OTADATA_SIZE = 0x2000
APP_ALIGN = 0x10000   # app partitions start on 64 KB boundaries
DATA_ALIGN = 0x1000

# The updater (factory) partition, the same on every flash size because it holds the same image.
# Measured updater image (platform/esp/updater, esp32c6, HTTPS with the certificate bundle's common
# subset, -Os): 794,592 bytes (0xC1FE0) on 2026-10-17; plus about 20 % and rounded up to 64 KB:
# 960 KB (24 % headroom). The build refuses an updater that outgrows it (doc/ota.md).
UPDATER_SIZE = 0xF0000

# The Zigbee coordinator's partitions at the end of the flash (issue #15, doc/zigbee.md), the same
# on every flash size, taken from the storage partition (whose offset stays):
# - zb_storage: the Zigbee stack's NVRAM (network key, PAN id, channel, address and binding tables).
#   The esp-zigbee-lib finds it by this label (data/fat, 16 KB as in Espressif's examples).
# - zb_fct: the stack's factory configuration zone (label fixed by the library; 1 KB used, one 4 KB
#   sector reserved).
# - zb_db: an NVS partition of its own for the controller's device database (zigbee-devices.json,
#   about 300 bytes per device): an NVS blob update needs room for the old and the new copy, and
#   the database must never crowd out the settings in the main NVS partition. 24 KB = 6 pages: a
#   database of up to about 9 KB (some 30 devices) can be rewritten.
# The application builds without Zigbee have them too (the updater ignores them).
ZIGBEE = [
	('zb_storage', 'data', 'fat', 0x4000),
	('zb_fct', 'data', 'fat', 0x1000),
	('zb_db', 'data', 'nvs', 0x6000),
]
ZIGBEE_SIZE = sum(size for (_name, _type, _subtype, size) in ZIGBEE)

# The core dump partition (issue #4, doc/diagnostics.md) between the storage and the Zigbee
# partitions, the same on every flash size, taken from the storage partition (whose offset stays):
# the panic handler writes an ELF core dump of every task (the used part of each stack and its
# TCB) there; GET /api/coredump and `esp-coredump info_corefile` read it. 64 KB as in Espressif's
# examples (`coredump status` prints the size of a stored dump; a dump that does not fit is not
# written, and the next boot logs that none is stored).
COREDUMP = ('coredump', 'data', 'coredump', 0x10000)

# The partitions at the end of the flash, in this order: the core dump, then the Zigbee three.
TAIL = [COREDUMP] + ZIGBEE
TAIL_SIZE = sum(size for (_name, _type, _subtype, size) in TAIL)

# Per flash size: the application (ota_0) partition, or None for "everything that leaves the
# storage its minimum". The 4 MB application stays at the 2752 KB it had before the core dump
# partition (the measured image of 2026-10-17 takes 2,076,352 bytes, 74 %); the storage gave the
# 64 KB (268 KB -> 204 KB: the UI image of about 50 KB gzipped and the state blobs, doc/persistence.md).
LAYOUTS = {
	'4MB': dict(flash=4 * MB, app=0x2B0000, storage_min=192 * KB),
	'8MB': dict(flash=8 * MB, app=4 * MB, storage_min=1 * MB),
	'16MB': dict(flash=16 * MB, app=6 * MB, storage_min=4 * MB),
}


def align_up(value, alignment):
	return (value + alignment - 1) // alignment * alignment


def align_down(value, alignment):
	return value // alignment * alignment


def layout(name):
	spec = LAYOUTS[name]
	flash = spec['flash']
	updater_offset = align_up(PHY_INIT[0] + PHY_INIT[1], APP_ALIGN)
	app_offset = updater_offset + UPDATER_SIZE
	if app_offset % APP_ALIGN:
		raise SystemExit('UPDATER_SIZE must be a multiple of 64 KB')
	app_size = spec['app']
	if app_size is None:
		# Largest 64 KB multiple that leaves otadata and the minimal storage.
		app_size = align_down(flash - app_offset - OTADATA_SIZE - spec['storage_min'] - TAIL_SIZE, APP_ALIGN)
	otadata_offset = align_up(app_offset + app_size, DATA_ALIGN)
	storage_offset = otadata_offset + OTADATA_SIZE
	storage_size = flash - storage_offset - TAIL_SIZE
	if storage_size < spec['storage_min']:
		raise SystemExit('%s: storage %d KB below its minimum %d KB' % (name, storage_size // KB,
																	   spec['storage_min'] // KB))
	rows = [
		('nvs', 'data', 'nvs', NVS[0], NVS[1]),
		('phy_init', 'data', 'phy', PHY_INIT[0], PHY_INIT[1]),
		('updater', 'app', 'factory', updater_offset, UPDATER_SIZE),
		('app', 'app', 'ota_0', app_offset, app_size),
		('otadata', 'data', 'ota', otadata_offset, OTADATA_SIZE),
		('storage', 'data', 'littlefs', storage_offset, storage_size),
	]
	offset = storage_offset + storage_size
	for (part, ptype, subtype, size) in TAIL:
		rows.append((part, ptype, subtype, offset, size))
		offset += size
	if offset != flash:
		raise SystemExit('%s: the partitions end at 0x%x, not at the flash size' % (name, offset))
	return rows


def render(name):
	rows = layout(name)
	lines = [
		'# Copyright (c) 2026 Bernhard Haumacher',
		'#',
		'# GENERATED by platform/esp/tools/gen_partitions.py - do not edit; change the sizes there.',
		'# Asymmetric OTA layout for %s flash (doc/ota.md): a small updater (factory) and one large' % name,
		'# application partition (ota_0); storage is the LittleFS partition (issue #5); coredump holds the',
		'# core dump of the last crash (issue #4, doc/diagnostics.md); zb_storage, zb_fct and zb_db belong',
		'# to the Zigbee coordinator (issue #15, doc/zigbee.md).',
		'#',
		'# Name,     Type, SubType,  Offset,   Size,     Flags',
	]
	for (part, ptype, subtype, offset, size) in rows:
		lines.append('%-10s %-5s %-9s 0x%06x, 0x%06x,' % (part + ',', ptype + ',', subtype + ',', offset, size))
	return '\n'.join(lines) + '\n'


def describe(name):
	out = []
	for (part, _ptype, _subtype, offset, size) in layout(name):
		out.append('  %-9s 0x%06x  0x%06x  %8.1f KB' % (part, offset, size, size / KB))
	return '\n'.join(out)


def main():
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument('--check', action='store_true', help='compare instead of writing')
	parser.add_argument('--print', action='store_true', help='print the layouts')
	args = parser.parse_args()
	root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
	failed = False
	for name in LAYOUTS:
		path = os.path.join(root, 'partitions-%s.csv' % name)
		text = render(name)
		if args.print:
			print('%s:\n%s' % (name, describe(name)))
		if args.check:
			try:
				with open(path) as f:
					current = f.read()
			except OSError:
				current = None
			if current != text:
				print('%s is out of date: run platform/esp/tools/gen_partitions.py' % path, file=sys.stderr)
				failed = True
		elif not args.print:
			with open(path, 'w') as f:
				f.write(text)
			print('wrote %s' % path)
	return 1 if failed else 0


if __name__ == '__main__':
	sys.exit(main())
