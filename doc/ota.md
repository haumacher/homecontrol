# OTA update with asymmetric partitions

How the controller replaces its own firmware (issue #3). The design is the author's (ROADMAP.md,
2026-10-17): *one partition for the updater (small) and one for the app (large). The app checks
whether an update is available, then boots to the updater partition. The updater overwrites the app
and boots back.* Compared with the IDF's two equal OTA slots this gives the application almost
twice the space on a 4 MB module.

| Piece | Where |
|---|---|
| Versions, manifest, URL rules, update instruction, the updater's decision table | `core/include/hc/update_check.h` (host-tested) |
| The application's side: confirmation, periodic check, hand-over to the updater | `platform/esp/components/hc_ota` |
| Console `update ...` | `platform/esp/components/hc_console` |
| The updater (a second ESP-IDF project) | `platform/esp/updater/` |
| NVS settings shared by both apps | `platform/esp/components/hc_settings` |
| Partition tables and their generator | `platform/esp/partitions-<size>.csv`, `platform/esp/tools/gen_partitions.py` |
| Flash size, table selection, app version | `platform/esp/hc_project.cmake` |
| otadata image that boots ota_0 after a USB flash | `platform/esp/tools/gen_otadata.py` |
| Development update server | `scripts/update-server.py` |

## Partition layout

Generated per flash size from one table of sizes (`tools/gen_partitions.py`; CI runs it with
`--check` against the committed CSVs). NVS stays where the single-app table had it, so stored
settings survive the switch; app partitions start on 64 KB boundaries.

| Partition | Type / subtype | 4 MB | 8 MB | 16 MB |
|---|---|---|---|---|
| (bootloader, partition table at 0x8000) | | 0x0–0x9000 | | |
| `nvs` | data / nvs | 0x9000, 24 KB | same | same |
| `phy_init` | data / phy | 0xF000, 4 KB | same | same |
| `updater` | app / factory | 0x10000, 960 KB | same | same |
| `app` | app / ota_0 | 0x100000, **2752 KB** | 0x100000, 4096 KB | 0x100000, 6144 KB |
| `otadata` | data / ota | 0x3B0000, 8 KB | 0x500000 | 0x700000 |
| `storage` | data / littlefs | 0x3B2000, 312 KB | 0x502000, 3064 KB | 0x702000, 9208 KB |

Sizes measured on 2026-10-17 (esp32c6, ESP-IDF 5.3):

| Image | Size | Partition | Free |
|---|---|---|---|
| updater (`hc_updater.bin`, -Os, HTTPS) | 794,592 B (0xC1FE0) | 960 KB | 19 % |
| updater without HTTPS (measured for comparison) | 773,696 B | | |
| application (`homecontrol.bin`, with the OTA client) | 1,239,776 B (0x12EAE0) | 2752 KB | 56 % |
| application before this package (single-app table) | 1,034,576 B | | |

The updater partition is the measured updater plus about 20 %, rounded up to 64 KB; the 4 MB
application gets everything that leaves the storage at least 256 KB. TLS costs the updater only
about 21 KB because the WiFi stack links mbedTLS anyway, so the updater speaks HTTPS. The
application grew by about 200 KB with the HTTP client, TLS and the certificate bundle. Both builds
check their binary against their own partition (factory resp. ota_0) and fail when it does not fit;
the IDF's generic check additionally prints a warning that the application does not fit the
updater partition, which is expected.

**Choosing the flash size.** `HC_FLASH_SIZE` (4MB default, 8MB, 16MB) is a CMake cache variable of
each build directory, read at its first configure: `idf.py -B build-n8 -D HC_FLASH_SIZE=8MB
set-target esp32c6`. It sets the flash size and the table through a generated sdkconfig fragment;
the updater must be built with the same value. A build whose sdkconfig was created for another
table (or before the OTA layout) refuses to configure and says to delete its `sdkconfig`. The web
flasher always writes the 4 MB layout, which runs on every module.

## Who writes what

All keys live in NVS namespace `hc` (the settings of `doc/network.md`); persisted names, never
renamed.

| Key | Type | Written by | Read by | Meaning |
|---|---|---|---|---|
| `update.url` | string | console `update url set`, erased by `update url clear` | application | update source: `<url>/manifest.json`; empty: no periodic check |
| `update.image` | string | application (written last) | updater | image URL; its presence marks a complete instruction |
| `update.version` | string | application | updater, application | version the manifest announced |
| `update.sha256` | string | application | updater | 64 hex digits of the image |
| `update.size` | integer | application | updater | image size in bytes |
| `update.attempts` | integer | application (0), updater (+1 before each download) | updater, application | download attempts for this instruction |
| `wifi.ssid`, `wifi.psk` | string | provisioning (doc/network.md) | application, updater | WiFi credentials |

The application erases the five `update.*` instruction keys after its next confirmed start
(`update.image` first). The updater never erases them except when they are unreadable.

## The manifest

`<update.url>/manifest.json`, a flat JSON object of at most 1024 bytes:

```json
{"version": "0.2.0", "url": "homecontrol.bin", "sha256": "<64 hex digits>", "size": 1239776,
 "min_version": "0.1.0"}
```

- `version`: semantic version (semver 2.0, `MAJOR.MINOR.PATCH[-pre][+build]`); it must equal the
  version in the image's app descriptor, which the build takes from `hc::kVersion`.
- `url`: absolute (`http://`, `https://`) or relative to `update.url` (a leading `/` keeps only its
  scheme and host); the result must fit 127 characters.
- `sha256`, `size`: of the image file.
- `min_version` (optional): a release below it refuses the update ("install an intermediate release
  first").
- Unknown members are ignored. Anything malformed is refused with the reason in the log.

**When an update applies** (`hc::check_update`): the manifest's version is newer by semver
precedence (a pre-release is older than its release: `0.1.0-dev` < `0.1.0` < `0.1.1-dev`; build
metadata does not count). `-dev` (pre-release) builds update to any newer release and ignore
`min_version`. `update now` installs the manifest's image whatever its version.

## The application

- **Start.** Logs the partition it runs from and its state. An application that does not run from
  ota_0 (flashed to the wrong offset) logs an error and refuses every update.
- **Confirmation.** When the network first comes up, or after 60 s without it, the application calls
  `esp_ota_mark_app_valid_cancel_rollback()` (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`). A new image
  that resets before (crash, watchdog, power loss) is marked aborted by the bootloader, which then
  boots the updater. After confirming, a leftover instruction is reported as `update to X completed`
  (the running version is X) or `update to X abandoned after N attempt(s)` (the updater booted the
  previous image) and erased.
- **Check.** 15 minutes after start and then every 24 hours, if `update.url` is set and the network
  is up (otherwise skipped with a log line), and on the console. The manifest is fetched in a
  short-lived task (8 KB stack, HTTPS verified against the IDF certificate bundle's common CAs, plain
  HTTP accepted), the decision runs on the event loop. For an applicable update the application
  checks that the image fits ota_0, stores the instruction, makes the updater the boot partition
  (`esp_ota_set_boot_partition(factory)`, which erases otadata) and restarts 2 s later, after the
  log line `update 0.1.0 -> 0.2.0 (... bytes, <url>): restarting into the updater in 2 s`.

Console (prompt `hc>`):

| Command | Effect |
|---|---|
| `update status` | version and build, partition and its state, update source, next check, last result |
| `update check` | checks `update.url` now; installs only a newer version |
| `update now [<url>]` | fetches `<url>/manifest.json` (default `update.url`) and installs its image whatever the version (development) |
| `update url set <url>` | stores the update source (`http://` or `https://`, at most 127 characters) |
| `update url clear` | erases it: no periodic check |

## The updater

A minimal ESP-IDF application in the factory partition: NVS, plain esp_wifi station with the
application's credentials (no WPS, no console, no mDNS, no status LED), esp_http_client, the OTA
API. Log level info, flash at 40 MHz and the transmit power limit come from the application's
`sdkconfig.defaults`, which the updater shares. At boot it logs `HomeControl updater <version>`,
then repeats:

1. Read the instruction (none / pending / corrupt; a corrupt one is logged and erased).
2. Inspect ota_0: *missing* (does not pass `esp_image_verify`), *aborted* (verifies, but the
   bootloader rolled it back: `esp_ota_get_state_partition` says aborted or invalid), else *valid*.
3. Decide (`hc::decide_updater_step`, the table below), log the decision, wait its delay, act.

| Instruction | Attempts | ota_0 | Action |
|---|---|---|---|
| none or corrupt | | valid or aborted | boot ota_0 |
| none or corrupt | | missing | wait, log again every 5 min (flash over USB) |
| pending | 0 | any | download now |
| pending | 1, 2 | any | download after 10 s (also over an aborted image: the re-download) |
| pending | ≥ 3 | valid | boot ota_0 (the download failed before it touched the old application) |
| pending | ≥ 3 | missing or aborted | download every 5 minutes, forever |

**Download.** The attempt counter is stored first. Then WiFi (30 s), the HTTP request (15 s network
timeout, status must be 200, a `Content-Length` must equal `update.size`), and the first 4 KB are
checked *before anything is erased*: an ESP image for this chip whose app descriptor names
`homecontrol` and the announced version (so an error page, the updater's own image or another
build never erases the application). Only then `esp_ota_begin` erases ota_0, the stream is written
and hashed, the SHA-256 must equal `update.sha256`, `esp_ota_end` verifies the image, and
`esp_ota_set_boot_partition(ota_0)` selects it (state *new*: the application must confirm itself).
Restart. Every failure is logged with its reason and whether the application partition was
already overwritten.

**First boot.** A board flashed over USB or with the web flasher gets an otadata image that selects
ota_0 with state *undefined* (`tools/gen_otadata.py`, the entry otatool's `switch_ota_partition`
writes): the bootloader starts the application directly, and an early reset (opening the serial
port resets the board) cannot roll a USB-flashed image back. If the otadata is blank anyway (erased,
or written by an older flash procedure), the bootloader starts the updater, which finds no
instruction and a valid ota_0 and boots it within a second or two.

**The updater's own update is by USB only.** It is small and rarely changes; it cannot replace
itself while it runs, and the application does not write the factory partition. Flash it with the
full procedure below (`write_flash @flash_args` includes it) or alone:
`esptool.py ... write_flash 0x10000 platform/esp/updater/build-esp32c6/hc_updater.bin`.

## Rollback and power loss

| Interrupted | Next boot | Outcome |
|---|---|---|
| while the application stores the instruction | application (otadata intact) | the instruction is incomplete (`update.image` written last) or is reported as abandoned and erased after the confirmation; the next check tries again |
| after the application erased otadata, before its restart | updater | downloads as instructed |
| during the download, before `esp_ota_begin` | updater | attempt counted; retries; after 3 attempts boots the intact old application, which reports the update abandoned |
| during the write (ota_0 half written) | updater (otadata blank) | ota_0 *missing*: retries, after 3 attempts every 5 minutes until it succeeds |
| after `esp_ota_set_boot_partition(ota_0)` | new application (state new → pending) | confirms after network up or 60 s, reports `completed`, erases the instruction |
| new application crashes or resets within its first minute | bootloader marks it aborted, boots the updater | instruction still pending: re-download (attempts permitting), else every 5 min |
| a USB-flashed application resets early | application again (state undefined) | nothing to roll back |

The bootloader writes otadata with a sequence number and CRC in two alternating sectors, so an
interrupted otadata write leaves the previous selection.

## Security

The update source is trusted: HTTPS servers are authenticated against the IDF certificate bundle
(common CAs), plain HTTP is accepted for development, and the image itself is not signed - the
SHA-256 in the manifest protects against corruption, not against a malicious source. The
authenticity question (signed images, secure boot) is issue #20.

## Development procedure

Build both apps (each target in its own directory; the updater into the directory of the same name
under `updater/`, which the application's `flash_args` references, with the same `HC_FLASH_SIZE`):

```bash
source ~/tools/esp/esp-idf/export.sh
cd platform/esp && idf.py -B build-esp32c6 build
cd updater && idf.py -B build-esp32c6 build
```

Flash everything (bootloader, partition table, updater at 0x10000, application at 0x100000, otadata
selecting ota_0) from the application's build directory - with the board in download mode as in
`doc/hardware.md`:

```bash
cd platform/esp/build-esp32c6 && esptool.py --chip esp32c6 --port /dev/ttyACM1 --before no_reset --after hard_reset write_flash @flash_args
```

`app-flash_args` writes only the application (at 0x100000). Then serve a build and update to it:

```bash
scripts/update-server.py              # serves platform/esp/build-esp32c6 on port 8070, prints the URLs
```

On the console: `update now http://<desk>:8070` installs the served build whatever its version;
`update url set http://<desk>:8070` and `update check` install it only if it is newer (bump
`kVersion` in `core/include/hc/version.h` for the second build). The updater's log lines carry the
tag `hc.updater`, the application's `hc.ota`.
