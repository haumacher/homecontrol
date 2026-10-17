# Hardware

## Target: ESP32-C6

- Single RISC-V core at 160 MHz, 512 KB HP SRAM (about 320–400 KB usable heap after WiFi and the
  802.15.4 stack), 16 KB LP SRAM, no PSRAM option.
- WiFi 6 (2.4 GHz only), Bluetooth 5 LE, IEEE 802.15.4 (Zigbee 3.0 / Thread). No Ethernet MAC: a
  wired option later means an SPI MAC+PHY such as the W5500 (ESP-IDF supports it as `esp_eth`).
- Modules: ESP32-C6-WROOM-1 with 4, 8 or 16 MB flash (N4/N8/N16). The OTA partition layout
  (issue "OTA with asymmetric partitions") depends on the flash size; generate the partition table
  per size, do not hard-code one.
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

**Console.** The dev boards have no UART-to-USB bridge, so `sdkconfig.defaults` routes the
console to the USB Serial/JTAG port (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`): bootloader and app
logs appear on `/dev/ttyACMn`. A changed `sdkconfig.defaults` is **not** applied to an existing
`build-<target>/sdkconfig`; delete that file (or run `set-target` again) to regenerate it.

The 4 MB flash of this board is the small end of the module range: the OTA partition layout
(issue #3) must have a 4 MB variant, and the application partition will be tight once Zigbee and
the web UI are in. A production board should carry 8 or 16 MB (issue #22).

**Button and LED.** The firmware uses the devkit's BOOT button (GPIO9 on the C6, a strapping pin
read only after boot) as the WPS re-pairing button: held for 3 s it starts WPS
(`CONFIG_HC_WPS_BUTTON_GPIO`, -1 disables it). The ESP32-C6-DevKitC-1/DevKitM-1 status LED is a
WS2812 RGB pixel on GPIO8, driven over RMT with the `led_strip` component; other boards choose
"plain GPIO" (with an active-low flag) or "none" in menuconfig under *HomeControl status LED*
(the S3 build defaults to none). The blink patterns are in `doc/network.md`.

**Partition table.** With WiFi, mDNS, WPS and the console the application is about 1 MB, more than
the default single-app table's 1 MB partition allows with any margin, so `sdkconfig.defaults`
selects the single-app *large* table (1.5 MB application, NVS unchanged at 0x9000) until the OTA
layout of issue #3 replaces it.

## KNX

The controller reaches the bus over KNXnet/IP. Two installations are reachable from the
development desk: a **test system** the author has released for free use during development, and
the author's **home installation**, which is touched only with their explicit permission. Their
addresses, device lists and router names are deliberately not in this repository (private); they
live in the developer's local notes. Builds, fixtures and tests never contain a gateway address of
a real installation, and no gateway is ever chosen from discovery automatically.

## Later options

- ESP32-P4 + ESP32-C6 (the C6 as wireless co-processor) if RAM or wired Ethernet become
  necessary; the portable core and the POSIX build keep that door open.
- Battery-backed RTC (DS3231 over I2C) for correct time at a start without network.
- KNX TP transceiver (NCN5130 or TPUART) instead of the IP router.
