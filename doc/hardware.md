# Hardware

## Target: ESP32-C6

- Single RISC-V core at 160 MHz, 512 KB HP SRAM (about 320–400 KB usable heap after WiFi and the
  802.15.4 stack), 16 KB LP SRAM, no PSRAM option.
- WiFi 6 (2.4 GHz only), Bluetooth 5 LE, IEEE 802.15.4 (Zigbee 3.0 / Thread). No Ethernet MAC: a
  wired option later means an SPI MAC+PHY such as the W5500 (ESP-IDF supports it as `esp_eth`).
- Modules: ESP32-C6-WROOM-1 with 4, 8 or 16 MB flash (N4/N8/N16). The OTA partition layout
  depends on the flash size: `platform/esp/tools/gen_partitions.py` generates one table per size,
  a build selects it with `HC_FLASH_SIZE` (see "Partition table" below and `doc/ota.md`).
- USB Serial/JTAG built in: the dev board shows up as `Espressif USB JTAG/serial debug unit`
  (`303a:1001`), one `/dev/ttyACMn` per board.

## On the desk (2026-10-17)

| Port | Chip | Flash | MAC | Use |
|---|---|---|---|---|
| `/dev/ttyACM1` | ESP32-C6 (QFN40, rev 0.2, ROM 2022-09-19) | **4 MB** | E8:F6:0A:F8:83:EC | **HomeControl development board** |
| `/dev/ttyACM0` | ESP32-S3 (QFN56 rev 0.2) | 16 MB | E4:B3:23:F3:23:84 | another project, do not touch |

Ports are assigned in plug-in order; after a replug check `/dev/serial/by-id/` (the MAC is in the
name). The firmware found on the C6 at first contact crash-looped (illegal instruction right after
boot), and every `esptool` connect — with `default_reset`, `usb_reset`, 15 attempts — ended in a
pySerial "Write timeout": the USB Serial/JTAG does not get the chip into download mode while the
firmware panics. What works: hold BOOT, press RESET, release BOOT (the chip then waits in the ROM
download mode), and flash with `--before no_reset`:

```bash
source ~/tools/esp/esp-idf/export.sh
cd platform/esp/build-esp32c6 && esptool.py --chip esp32c6 --port /dev/ttyACM1 --before no_reset --after hard_reset write_flash @flash_args
```

(`idf.py flash` passes `--before default_reset` itself, so the environment variable `ESPTOOL_BEFORE`
does not help there.) With a healthy firmware on the chip the ordinary `idf.py flash` auto-reset
is expected to work again.

**Leaving download mode needs a physical reset.** Every reset the host can trigger over the USB
Serial/JTAG port (`rst:0x15 (USB_UART_HPSYS)`: esptool's `--after hard_reset`, the IDF monitor,
DTR/RTS toggling) resets the HP system only and does not re-sample the strapping pins. The boot
mode latched by the BOOT+RESET grip therefore persists (`boot:0x4 (DOWNLOAD…)`) until the RESET
(EN) button is pressed or the board is power-cycled (replug the USB cable). So after a manual
download-mode flash: press RESET once by hand, then open the monitor.

**Opening the serial port resets the board.** Any host program opening `/dev/ttyACMn` (pyserial even
with DTR and RTS preset low, the IDF monitor) makes the USB Serial/JTAG controller reset the chip:
a capture always starts with a fresh boot and loses the state before it. For a run that must not be
interrupted (WPS pairing, a reconnect test), open the port once *before* the test and keep it open
for the whole run; a listener that stops and reopens loses the device state. Stored credentials
survive the reset, the wall clock keeps its value in the RTC but is flagged unsynchronized until
SNTP confirms it.

**Console.** The dev boards have no UART-to-USB bridge, so `sdkconfig.defaults` routes the
console to the USB Serial/JTAG port (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`): bootloader and app
logs appear on `/dev/ttyACMn`. A changed `sdkconfig.defaults` is **not** applied to an existing
`build-<target>/sdkconfig`; delete that file (or run `set-target` again) to regenerate it.

The 4 MB flash of this board is the small end of the module range: the OTA layout has a 4 MB
variant, and the application partition (2752 KB) will get tight once Zigbee and the web UI are in.
A production board should carry 8 or 16 MB (issue #22).

**Button and LED.** The firmware uses the devkit's BOOT button (GPIO9 on the C6, a strapping pin
read only after boot) as the WPS re-pairing button: held for 3 s it starts WPS
(`CONFIG_HC_WPS_BUTTON_GPIO`, -1 disables it). The ESP32-C6-DevKitC-1/DevKitM-1 status LED is a
WS2812 RGB pixel on GPIO8, driven over RMT with the `led_strip` component; other boards choose
"plain GPIO" (with an active-low flag) or "none" in menuconfig under *HomeControl status LED*
(the S3 build defaults to none). The blink patterns are in `doc/network.md`.

**Partition table.** The asymmetric OTA layout of `doc/ota.md`: a small updater (factory) and one
large application partition (ota_0), generated per flash size by `platform/esp/tools/gen_partitions.py`
into `platform/esp/partitions-<size>.csv`. NVS stays at 0x9000 (24 KB) as in the single-app table
before it, so stored settings survive the switch.

| Partition | 4 MB (this board) | 8 MB | 16 MB |
|---|---|---|---|
| `nvs` | 0x9000, 24 KB | same | same |
| `phy_init` | 0xF000, 4 KB | same | same |
| `updater` (factory) | 0x10000, 960 KB | same | same |
| `app` (ota_0) | 0x100000, 2752 KB | 4096 KB | 6144 KB |
| `otadata` | 0x3B0000, 8 KB | 0x500000 | 0x700000 |
| `storage` (LittleFS, issue #5) | 0x3B2000, 204 KB (268 KB before the core dump partition, 312 KB before the Zigbee partitions) | 2956 KB | 9100 KB |
| `coredump` (issue #4) | 0x3E5000, 64 KB | 0x7E5000 | 0xFE5000 |
| `zb_storage`, `zb_fct`, `zb_db` (issue #15) | 0x3F5000, 16 + 4 + 24 KB | 0x7F5000 | 0xFF5000 |

The `coredump` partition (doc/diagnostics.md) took its 64 KB from the end of `storage`, whose offset
stays; the 4 MB application keeps its 2752 KB (the C6 application measured 2,101,296 bytes on
2026-10-18, 75 %). A board flashed with an older table needs the full `write_flash @flash_args` once:
an OTA update does not change the partition table (the application then logs that it has no coredump
partition), and the LittleFS image of the old, larger `storage` partition does not mount on the
smaller one (values and history are not saved until `storage.bin` is flashed).

Measured on 2026-10-17 (esp32c6): the updater 794,592 bytes (19 % of its partition free), the
application 1,239,776 bytes (56 % free). The build chooses the table with the CMake variable
`HC_FLASH_SIZE` (4MB default; `-D HC_FLASH_SIZE=8MB` at the first configure of a build directory);
a build directory whose `sdkconfig` predates the OTA layout refuses to configure until its
`sdkconfig` is deleted. `write_flash @flash_args` in `platform/esp/build-esp32c6` writes the whole
layout - bootloader, table, the updater from `platform/esp/updater/build-esp32c6` (build it first),
the application at 0x100000 and an otadata image that boots it - and `@app-flash_args` only the
application. Flashing the application at 0x10000 (the old single-app offset) overwrites the updater;
such an application logs that it does not run from ota_0 and refuses updates.

## Flash clock: 40 MHz on this board

The C6 on the desk has its flash in the package (esptool: "Unknown Embedded Flash", JEDEC
manufacturer `c4`, device `6016`: a 4 MB Giga Device part). At the IDF default of **80 MHz** it
misreads: the bootloader rejected a partition table that `read_flash` proved byte-identical to the
image ("partition 0 invalid magic number"), and the application died with "Illegal instruction"
panics while starting WPS; the firmware found on the board at first contact crash-looped the same
way. At **40 MHz** (`CONFIG_ESPTOOLPY_FLASHFREQ_40M=y` in `sdkconfig.defaults`) the same images run
without a fault. Keep 40 MHz until a board is proven at 80 MHz; the setting lives in the
bootloader's image header, so changing it means reflashing the bootloader as well.

Diagnosis recipe when a board misbehaves right after a verified flash: `esptool.py read_flash
<offset> <length> <file>` and `cmp` against the build output. Identical content plus boot errors
means the read side, not the write side.

## Supply and transmit power

The development C6 browns out on its USB supply when WiFi transmits at the driver's maximum of
20 dBm: with the network firmware in WPS mode (continuous probing) the chip logged
`BOD: Brownout detector was triggered` and reset every 10–80 s, 91 times in ten minutes, with no
panic and no watchdog. The USB topology on the desk puts both ESP boards, an LTE modem and a hub on
one root hub. With the transmit power limited to 11 dBm (`CONFIG_HC_WIFI_MAX_TX_POWER=44`, the
project default in `sdkconfig.defaults`; 8.5 dBm behaved the same) the board ran without a single
brownout. A board with a proper supply can raise the limit through menuconfig (*HomeControl
network*); the value is in quarter dBm, 80 is the maximum. A brownout reset is visible at boot: the
controller's first line ends in `reset reason: brownout` (a warning), and the boot record counts
them (`brownouts` in `status` and `/api/status`; doc/diagnostics.md).

## KNX

The controller reaches the bus over KNXnet/IP. Two installations are reachable from the
development desk: a **test system** the author has released for free use during development, and
the author's **home installation**, which is touched only with their explicit permission. Their
addresses, device lists and router names are deliberately not in this repository (private); they
live in the developer's local notes. Builds, fixtures and tests never contain a gateway address of
a real installation, and no gateway is ever chosen from discovery automatically.

## Production requirement: the development board is the minimum (decided 2026-10-18, #22)

The author's decision: the board on the desk — an ESP32-C6 module with **4 MB flash** (N4) and the
C6's 512 KB SRAM — is the minimum a HomeControl installation may run on. Consequences:

- The controller must keep fitting the 4 MB layout (`partitions-4MB.csv`: application 2.75 MB in
  each OTA slot, updater 960 KB, storage, core dump, the Zigbee partitions). Nothing may depend on
  the 8 or 16 MB tables; they only give such modules more storage.
- The size line of every build (`idf.py size`, the "free" percentage of the application partition)
  and the free heap after boot stay part of every package's report and of the build gate. When a
  feature does not fit, the first thing to drop is the esp32s3 target's unused components, the
  second is making the feature an optional component selected at build time, never a silent cut.
- Heap: with WiFi, the KNX tunnel, Zigbee formed, the web server and the test-system image the
  board has about 110 KB free (minimum 93 KB); a WebSocket client costs about 30 KB. Steady-state
  code allocates nothing; images allocate on load.

## Later options

- ESP32-P4 + ESP32-C6 (the C6 as wireless co-processor) if RAM or wired Ethernet become
  necessary; the portable core and the POSIX build keep that door open.
- Battery-backed RTC (DS3231 over I2C) for correct time at a start without network.
- KNX TP transceiver (NCN5130 or TPUART) instead of the IP router.
