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
# 960 KB until issue #87 (the staging area for received images lay behind the updater's image);
# 896 KB since: the updater of 2026-11-16 is 876,064 bytes (0xD5E20), 41 KB below it. CI refuses an
# updater larger than the partition less UPDATER_MARGIN (--check-updater, doc/ota.md).
UPDATER_SIZE = 0xE0000
UPDATER_MARGIN = 16 * KB

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

# The runtime image's slot (issue #87, spec/runtime-image.md 7): the partition `image`, type data,
# subtype 0x40 (found by its name), directly behind the application, mapped through the cache from
# its first byte, so it starts on a 64 KB boundary (the MMU's page).
IMAGE_SUBTYPE = '0x40'
IMAGE_ALIGN = 0x10000

# Per flash size: the application (ota_0) partition, or None for "everything that leaves the
# storage its minimum", and the image slot behind it.
# - 4 MB: the application gave the slot its 128 KB (2752 KB -> 2624 KB) and took the 64 KB the
#   updater gave back (2688 KB at 0xF0000; the firmware of 2026-11-17 takes 2,475,392 bytes); the
#   storage stays 204 KB (the history rings and archives up to 64 + 64 KB, the language pack, the
#   values, the overrides; doc/persistence.md).
# - 8 and 16 MB: the storage gave the slot its 512 KB; the application took the updater's 64 KB.
LAYOUTS = {
	'4MB': dict(flash=4 * MB, app=0x2A0000, image=0x20000, storage_min=192 * KB),
	'8MB': dict(flash=8 * MB, app=4 * MB + 64 * KB, image=0x80000, storage_min=1 * MB),
	'16MB': dict(flash=16 * MB, app=6 * MB + 64 * KB, image=0x80000, storage_min=4 * MB),
}

# What spec/runtime-image.md 7 fixes per flash size (offset, size), checked by --check: the
# application, the image slot, otadata and the storage.
EXPECTED = {
	'4MB': dict(updater=(0x10000, 0xE0000), app=(0xF0000, 0x2A0000), image=(0x390000, 0x20000),
				otadata=(0x3B0000, 0x2000), storage=(0x3B2000, 0x33000)),
	'8MB': dict(updater=(0x10000, 0xE0000), app=(0xF0000, 0x410000), image=(0x500000, 0x80000),
				otadata=(0x580000, 0x2000), storage=(0x582000, 2444 * KB)),
	'16MB': dict(updater=(0x10000, 0xE0000), app=(0xF0000, 0x610000), image=(0x700000, 0x80000),
				 otadata=(0x780000, 0x2000), storage=(0x782000, 8588 * KB)),
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
		app_size = align_down(flash - app_offset - spec['image'] - OTADATA_SIZE - spec['storage_min'] - TAIL_SIZE,
							  APP_ALIGN)
	image_offset = align_up(app_offset + app_size, IMAGE_ALIGN)
	image_size = spec['image']
	otadata_offset = align_up(image_offset + image_size, DATA_ALIGN)
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
		('image', 'data', IMAGE_SUBTYPE, image_offset, image_size),
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
		'# application partition (ota_0); image is the runtime image\'s slot (issue #87,',
		'# spec/runtime-image.md 7); storage is the LittleFS partition (issue #5); coredump holds the core',
		'# dump of the last crash (issue #4, doc/diagnostics.md); zb_storage, zb_fct and zb_db belong to',
		'# the Zigbee coordinator (issue #15, doc/zigbee.md).',
		'#',
		'# Name,     Type, SubType,  Offset,   Size,     Flags',
	]
	for (part, ptype, subtype, offset, size) in rows:
		lines.append('%-10s %-5s %-9s 0x%06x, 0x%06x,' % (part + ',', ptype + ',', subtype + ',', offset, size))
	return '\n'.join(lines) + '\n'


def check_layout(name):
	"""The checks of spec/runtime-image.md 7: the slot is there, on a 64 KB boundary, and the
	partitions the spec names sit where it puts them. Returns the problems."""
	rows = {part: (subtype, offset, size) for (part, _ptype, subtype, offset, size) in layout(name)}
	problems = []
	if 'image' not in rows:
		return ['%s: no partition image' % name]
	subtype, offset, _size = rows['image']
	if subtype != IMAGE_SUBTYPE:
		problems.append('%s: image has subtype %s, not %s' % (name, subtype, IMAGE_SUBTYPE))
	if offset % IMAGE_ALIGN:
		problems.append('%s: image starts at 0x%x, not on a 64 KB boundary' % (name, offset))
	for part, (want_offset, want_size) in EXPECTED[name].items():
		_subtype, got_offset, got_size = rows[part]
		if (got_offset, got_size) != (want_offset, want_size):
			problems.append('%s: %s at 0x%x of 0x%x, spec/runtime-image.md 7 says 0x%x of 0x%x' % (
				name, part, got_offset, got_size, want_offset, want_size))
	return problems


def describe(name):
	out = []
	for (part, _ptype, _subtype, offset, size) in layout(name):
		out.append('  %-9s 0x%06x  0x%06x  %8.1f KB' % (part, offset, size, size / KB))
	return '\n'.join(out)


def main():
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument('--check', action='store_true', help='compare instead of writing')
	parser.add_argument('--print', action='store_true', help='print the layouts')
	parser.add_argument('--check-updater', metavar='BIN',
						help='fail if the updater image BIN exceeds its partition less %d KB' % (UPDATER_MARGIN // KB))
	args = parser.parse_args()
	if args.check_updater:
		size = os.path.getsize(args.check_updater)
		limit = UPDATER_SIZE - UPDATER_MARGIN
		print('updater %s: %d B of %d B (the partition %d KB less a margin of %d KB): %d B left' % (
			args.check_updater, size, limit, UPDATER_SIZE // KB, UPDATER_MARGIN // KB, limit - size))
		return 0 if size <= limit else 1
	root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
	failed = False
	for name in LAYOUTS:
		path = os.path.join(root, 'partitions-%s.csv' % name)
		text = render(name)
		if args.print:
			print('%s:\n%s' % (name, describe(name)))
		if args.check:
			for problem in check_layout(name):
				print(problem, file=sys.stderr)
				failed = True
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
