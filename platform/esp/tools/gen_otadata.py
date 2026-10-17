#!/usr/bin/env python3
# Copyright (c) 2026 Bernhard Haumacher
#
# Writes an otadata partition image that selects the first OTA partition (ota_0), so that a board
# flashed over USB boots the application directly instead of passing through the updater (the
# factory partition) once (doc/ota.md, "First boot").
#
#   gen_otadata.py <size> <output>
#
# The layout is the IDF's esp_ota_select_entry_t (components/bootloader_support/include/
# esp_flash_partitions.h) in the first of the two sectors, the same entry otatool.py's
# switch_ota_partition writes: ota_seq = 1 (ota_0), seq_label and ota_state left erased (0xFF:
# ESP_OTA_IMG_UNDEFINED, "can boot and work without limits": an image flashed over USB is not on
# probation and cannot be rolled back by an early reset), crc = crc32(ota_seq) as the bootloader
# computes it. The second sector stays erased.

import binascii
import struct
import sys


def main():
	if len(sys.argv) != 3:
		print('usage: gen_otadata.py <size> <output>', file=sys.stderr)
		return 2
	size = int(sys.argv[1], 0)
	if size < 0x2000 or size % 0x1000:
		print('otadata size must be at least two 4 KB sectors', file=sys.stderr)
		return 2
	seq = struct.pack('<I', 1)
	crc = struct.pack('<I', binascii.crc32(seq, 0xFFFFFFFF) & 0xFFFFFFFF)
	data = bytearray(b'\xff' * size)
	data[0:4] = seq          # ota_seq
	# 4..23 seq_label[20], 24..27 ota_state: erased
	data[28:32] = crc        # crc over ota_seq
	with open(sys.argv[2], 'wb') as f:
		f.write(data)
	return 0


if __name__ == '__main__':
	sys.exit(main())
