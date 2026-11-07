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
| The updater (a second ESP-IDF project), its upload server and page | `platform/esp/updater/` |
| Upload instruction, token check, image header check, the upload rows of the decision table | `core/include/hc/update_check.h` (host-tested) |
| `POST /api/system/update/upload`, the Settings page's "Install firmware file" | `core/src/web_api_system.cpp`, `web/ui/dist/system.js` |
| NVS settings shared by both apps | `platform/esp/components/hc_settings` |
| Partition tables and their generator | `platform/esp/partitions-<size>.csv`, `platform/esp/tools/gen_partitions.py` |
| Flash size, table selection, app version | `platform/esp/hc_project.cmake` |
| otadata image that boots ota_0 after a USB flash | `platform/esp/tools/gen_otadata.py` |
| Development update server | `scripts/update-server.py` |
| Releases, the web flasher and the Pages site | `scripts/release.sh`, `scripts/pages-site.py`, `.github/workflows/pages.yml`, `web/flasher/` |

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
| `storage` | data / littlefs | 0x3B2000, 204 KB (268 KB before the core dump partition, 312 KB before the Zigbee partitions) | 0x502000, 2956 KB | 0x702000, 9100 KB |
| `coredump` | data / coredump | 0x3E5000, 64 KB (doc/diagnostics.md) | 0x7E5000 | 0xFE5000 |
| `zb_storage`, `zb_fct`, `zb_db` | data / fat, fat, nvs | 0x3F5000, 16 + 4 + 24 KB (doc/zigbee.md) | 0x7F5000 | 0xFF5000 |

Sizes measured on 2026-10-17 (esp32c6, ESP-IDF 5.3):

| Image | Size | Partition | Free |
|---|---|---|---|
| updater (`hc_updater.bin`, -Os, HTTPS) | 794,592 B (0xC1FE0) | 960 KB | 19 % |
| updater without HTTPS (measured for comparison) | 773,696 B | | |
| updater with the upload server (esp_http_server, page; 2026-10-18) | 825,712 B (0xC9970), +30,288 B over the 795,424 B before it | 960 KB | 16 % |
| application (`homecontrol.bin`, with the OTA client) | 1,239,776 B (0x12EAE0) | 2752 KB | 56 % |
| application before this package (single-app table) | 1,034,576 B | | |
| updater on ESP-IDF 5.5.6 (2026-11-03) | 845,872 B (0xCE430) | 960 KB | 14 % |
| application on ESP-IDF 5.5.6 with Zigbee and E-Home (2026-11-03), 2,355,696 B on 5.3 before | 2,393,856 B (0x248700) | 2752 KB | 15 % |

The updater partition is the measured updater plus about 20 %, rounded up to 64 KB; the 4 MB
application has 2752 KB (originally everything that left the storage at least 256 KB; the storage
gave the 64 KB of the core dump partition, so the application kept its size). TLS costs the updater only
about 21 KB because the WiFi stack links mbedTLS anyway, so the updater speaks HTTPS. The
application grew by about 200 KB with the HTTP client, TLS and the certificate bundle. Both builds
check their binary against their own partition (factory resp. ota_0) and fail when it does not fit;
the IDF's generic check additionally prints a warning that the application does not fit the
updater partition, which is expected.

**The staging area behind the updater (homecontrol #66).** The flash of the `updater` partition
behind the updater's image is the device's staging area for received runtime images
(`hc::esp::EspImageStaging`, `platform/esp/components/hc_image`): with the 825,792 B updater of
5.5.6 on the C6 148 KB (from the image's end rounded up to a 4 KB sector, plus one guard sector, to the
partition's end). The application never writes the factory partition otherwise, so the space is
free, and it is not the running partition, which the IDF's write protection
(`CHECK_WRITE_ADDRESS`, `CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS`) guards as a whole. At boot the
application verifies the updater's image (`esp_image_verify`, which also gives its length; logged
as `image staging: N KB in partition 'updater' at 0x…, behind the updater's image (… B, verified in
… ms) and a guard sector, mapped at 0x… for the received and the stored image; … KB of the address
space free for mapping`); if it does not verify or cannot be mapped, the staging stays off for that
boot (logged with the reason) and images are read into the heap. A
staging erases only the 4 KB sectors the body reaches, as it arrives, never below the guard
sector, and reads the body back through the area's mapping (`esp_partition_mmap` of the whole area
once at boot, three of the MMU's 64 KB pages; the 2.5 MB application leaves most of the 16 MB address
space free; the flash writes invalidate the cache of the pages they reach). Two slots share the area
(homecontrol #71): the received images (`received()`) and the stored image the image service loads at
boot and returns to after a failed apply (`stored()`, doc/image.md "Loading the stored image"); a
slot reserves the sectors it needs at the area's start or, while the other holds that, behind the
other's reservation, and is refused (the image goes to the heap) when the area has no room beside
it. The area is mapped once rather than per staging because two mappings of one MMU page share one
entry: unmapping the first would take the second's text along. The content is transient:
nothing reads it after a restart, a new staging starts over, and a USB flash of a larger updater
only shortens the area (it is measured again at the next boot). While the updater runs (an OTA
update or upload) the application does not, so the two never use the partition at once. An image
larger than the area goes to the heap as before.

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
| `update.channel` | string | Settings (Update channel), console `update channel` | application | `stable` (also when not stored), `beta` or `off`: the update source `https://haumacher.github.io/homecontrol/<channel>` (`hc::setting::kReleaseSite`); `off`: no periodic check, no language packs |
| `update.url` | string | console `update url set`, erased by `update url clear` | application | a development update server in place of the channel (`<url>/manifest.json`); not in the web UI. An update.url of a firmware before the channels (a URL into the release site, `off`, empty) becomes the channel at the next start (`hc::migrate_update_source`) |
| `update.image` | string | application (written last) | updater | image URL; its presence marks a complete instruction |
| `update.version` | string | application | updater, application | version the manifest announced |
| `update.sha256` | string | application | updater | 64 hex digits of the image |
| `update.size` | integer | application | updater | image size in bytes |
| `update.attempts` | integer | application (0), updater (+1 before each download) | updater, application | download attempts for this instruction |
| `update.image` = `upload` | string | application (written last) | updater | an *upload* instruction: not a URL on purpose (below) |
| `update.token` | string | application | updater | the upload's one-time token, 32 lower-case hex digits (128 random bits) |
| `update.uploaded` | string | updater (after the uploaded image verified) | application | version installed from an upload; reported and erased after the confirmed start |
| `wifi.ssid`, `wifi.psk` | string | provisioning (doc/network.md) | application, updater | WiFi credentials |

The application erases the instruction keys after its next confirmed start (`update.image` first,
`update.token` with them). The updater never erases them except when they are unreadable, and after a
successful upload (which leaves `update.uploaded` for the application's report instead).

**Two kinds of instruction under one key.** `update.image` holds the image URL of a download or the
marker `upload`. An updater flashed before uploads existed reads `update.image` first and requires
an `http(s)://` URL: it reports the upload instruction as *corrupt* (`update.image is not a valid
URL`), erases its five keys and boots the application - it never mistakes an upload for a download.
It does not know `update.token`, which the application then finds alone after its confirmed start
and reports as `firmware upload did not start: the updater discarded the request (it predates
uploads ...)`. In practice the application refuses the request before that (below: the updater's
version marker); the test `UpdateUpload: an updater that predates uploads reads the upload
instruction as corrupt` keeps the old reader's logic and checks both instructions against it.

## The manifest

`<update.url>/manifest.json`, a flat JSON object of at most 1024 bytes (`UpdateManifest::kMaxLength`,
the manifest's own limit: the OTA service reads it with its own esp_http_client into a buffer of
that size and the parser refuses anything longer; it does not use the streaming `HttpFetcher` of
issue #43, which carries the language packs, the external values and the E-Home enrollment):

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

## Manifest signature

The manifest is signed with the **release key** (issue #35): `<update.url>/manifest.sig` beside it
holds the detached Ed25519 signature (RFC 8032, plain Ed25519) over the manifest's **exact bytes as
served**, as 128 hex digits with an optional line end (or the 64 raw bytes). The application fetches
both in its check task and calls `hc::verify_manifest_signature` before it parses the manifest; the
image itself stays guarded by the manifest's `sha256`, which the updater checks as before (the
updater needs no key). Refused, with the reason in the log, the console's `update status` and the
status's `update.last` (the System card on the Settings page):

| Verdict | `last` |
|---|---|
| no `manifest.sig` (404) | `manifest refused: the manifest is not signed (no manifest.sig next to it; the release key is e12f 90a9 fd8b f2c4)` |
| not 128 hex digits / 64 bytes | `manifest refused: manifest.sig is not a signature (128 hex digits or 64 bytes)` |
| another key, or the manifest changed | `manifest refused: the manifest's signature does not match the release key ... (another key, or the manifest changed)` |
| another HTTP error on `manifest.sig` | `check failed: manifest.sig: HTTP status <n>` |

**The release key in the firmware.** The build reads the public key (64 hex digits) from the file
the CMake variable `HC_RELEASE_KEY` names, default the committed `platform/esp/release-key.pub`,
into `hc_generated/hc_release_key.h` (`hc::release_public_key()`); the status's `update.release_key`
and `update status` show its fingerprint. The private half lives with the release owner, never in
the repository (the author's copy: `~/.config/homecontrol/release-key.secret`, mode 600; sign a
release's manifest with `scripts/sign.py manifest <secret-file> manifest.json`).

**Development key.** `scripts/update-server.py` signs with a development key it generates on first
use into the git-ignored `scripts/dev-release-key` (public half `scripts/dev-release-key.pub`) and
prints the build line for a firmware that trusts it:
`idf.py -B build-esp32c6 -DHC_RELEASE_KEY=$PWD/scripts/dev-release-key.pub build` (a changed key
file reconfigures). `--key <secret-file>` signs with another key, `--unsigned` serves no
`manifest.sig` (to watch the refusal). A firmware built with the committed release key refuses the
development server's manifests, and vice versa: the browser upload (below) works with any build.

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
- **Check.** 15 minutes after start and then every 24 hours, unless the update channel is `off`, if the network
  is up (otherwise skipped with a log line), and on the console. The manifest and its signature are
  fetched on the `update` lane of the one fetch worker (doc/external.md "One fetch worker": second
  after the console's image download, 15 s timeout; the worker's task `hc_fetch` has an 8 KB stack; HTTPS verified against the IDF certificate bundle's common CAs, plain
  HTTP accepted), the decision runs on the event loop. For an applicable update the application
  checks that the image fits ota_0, stores the instruction, makes the updater the boot partition
  (`esp_ota_set_boot_partition(factory)`, which erases otadata) and restarts 2 s later, after the
  log line `update 0.1.0 -> 0.2.0 (... bytes, <url>): restarting into the updater in 2 s`.

Console (prompt `hc>`):

| Command | Effect |
|---|---|
| `update status` | version and build, partition and its state, update source, next check, last result |
| `update check` | checks the update source now; installs only a newer version |
| `update now [<url>]` | fetches `<url>/manifest.json` (default: the update source) and installs its image whatever the version (development) |
| `update channel stable\|beta\|off` | chooses the update channel (as the Settings page does) |
| `update url set <url>` | a development update server in place of the channel (`http://` or `https://`, at most 127 characters) |
| `update url clear` | erases it: the channel applies again |

## The updater

A minimal ESP-IDF application in the factory partition: NVS, plain esp_wifi station with the
application's credentials (no WPS, no console, no mDNS), the status LED (below), esp_http_client,
esp_http_server (only for an upload), the OTA API. Log level info, flash at 40 MHz and the transmit power limit come from the application's
`sdkconfig.defaults`, which the updater shares. At boot it logs `HomeControl updater <version>`,
then repeats:

1. Read the instruction (none / pending / awaiting upload / corrupt; a corrupt one is logged and
   erased).
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
| awaiting upload | | valid | serve the upload for 10 minutes, then boot ota_0 (`hc::decide_upload_expired`) |
| awaiting upload | | missing or aborted | serve the upload without a limit (the upload is the only way out), reminder in the log every minute |
| awaiting upload, 10 minutes over | | valid / missing or aborted / an upload is being written | boot ota_0 / serve on without a limit / wait for the upload to end |

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

**The radio after a power-on (#69).** Before WiFi starts, the updater (and the application, through
`EspSystemInfo::start_temperature_sensor()`) installs the die temperature sensor and logs `die
temperature <n> C`. The PHY's calibration uses the sensor, but after every reset other than a
software restart (power-on, the USB reset of esptool, a brownout) the IDF startup code gates the
sensor's bus clock (`esp_system/port/soc/esp32c6/clk.c`), and the WiFi stack does not turn it on
again: the receiver then hears no access point at all, every scan comes back empty and every connect
fails with reason 201 (NO_AP_FOUND), until the next software restart. The application installed the
sensor for its System page, and the clock survives a software restart, so the handover from the
application to the updater always worked; the updater after a power-on or a USB reset never
connected (measured 2026-11-04: 0 access points in every scan, against 3 to 4 after the fix, and the
network up 10 s after the reset). The application was deaf in the same way until its first
temperature reading or its own recovery (43 s to the network after a USB reset, 14 s with the fix).

**The status LED.** The updater drives the application's LED (the same menuconfig choice
*HomeControl status LED*: the WS2812 on GPIO8 of the C6 devkits, a plain GPIO LED, or none; the
component `hc_status_light`) with patterns of its own (`hc::UpdaterLed`, 50 ms ticks, an esp_timer
callback). Each one differs from the others and from the application's patterns
([network.md, "Status LED"](network.md#status-led)) by its timing alone, so a plain LED tells them
apart without the colour; the WS2812 adds a dim colour.

| Pattern | Colour | Meaning |
|---|---|---|
| dark | | before the first decision; booting the application |
| three 50 ms flashes, then dark to 1 s | red | emergency: no application in ota_0 and no instruction; hold the button for 3 s to upload one (below) |
| solid, at once on press | amber | the button is down and the 3 s hold is counting (only while ota_0 holds no application) |
| slow blink, 800 ms on / 800 ms off | blue | the button window is open: an upload without the token is accepted (5 minutes) |
| 500 ms on / 500 ms off | magenta | WiFi has no address yet (before an upload is served or a download starts), and between download attempts |
| short pulse, 100 ms every 1.6 s | cyan | serving the upload started from the application's Settings page, waiting for the image |
| 150 ms on / 150 ms off | green | busy: a download or an upload is writing or verifying ota_0 |
| flicker, 50 ms on / 50 ms off, for 2 s | red | error, over the pattern underneath: an upload refused or failed, a download attempt failed |

Two patterns look like the application's on purpose or by necessity: *connecting* is the
application's 500/500 with the same meaning, and *held* is solid like the application's *ready*, but
only while a finger is on the button. A press does not show *held* while ota_0 holds a valid
application (the button opens no window then). The window's blink takes precedence over
*connecting*: after a hold in the emergency wait WiFi starts only then, and the blink acknowledges
the hold at once; *connecting* shows when the window closes before the network is up. When the window
closes, the LED returns to the emergency (or, for an upload with the token, to the cyan pulse); when
an upload starts, it turns green.

**The updater's own update is by USB only.** It is small and rarely changes; it cannot replace
itself while it runs, and the application does not write the factory partition (except the
staging area behind the updater's image, above). Flash it with the
full procedure below (`write_flash @flash_args` includes it) or alone:
`esptool.py ... write_flash 0x10000 platform/esp/updater/build-esp32c6/hc_updater.bin`.

## Upload from the browser

Issue #39, decided by the author on 2026-10-18 (ROADMAP.md): an installed device has no serial
console and no update server; its owner downloads a release's `homecontrol.bin` and installs it from
the device's Settings page. The application cannot receive its own replacement (it runs from
ota_0), so the hand-over goes through the updater, which has WiFi, NVS and the OTA API anyway.

**Sequence.**

1. Settings → System → Firmware update → *Install firmware file*. The page reads the chosen file's
   app descriptor (magic `0xE9`, descriptor magic, project name `homecontrol`) and refuses anything
   else before the device is touched; then it explains (the device restarts into the updater for up
   to 10 minutes, KNX and Zigbee pause, keep the page open) and asks for confirmation.
2. `POST /api/system/update/upload` (spec/api.md "System"). The application generates a one-time
   token (128 bits from the platform's random source, `Crypto::random`, as hex), checks that it runs
   from ota_0, that no update check or update is under way, and that the updater in the factory
   partition serves uploads (its app descriptor version carries the build identifier `upload`, e.g.
   `0.1.0-dev+upload`, set by `updater/CMakeLists.txt`; an older updater is refused with 409
   instead of restarting into it for nothing), stores the instruction (`update.token`, then
   `update.image` = `upload`), makes the updater the boot partition and answers
   `{"ok": true, "url": "http://<its IP>/", "token": "<hex>", "restart_in_ms": 750, "timeout_s": 600}`.
   It restarts 750 ms later through the API's deferred action (log `restarting into the updater for
   the firmware upload`).
3. The page stores the token in `sessionStorage` (key `hc.upload.token`), stops its WebSocket and
   polls `GET /status` at the same origin: connection errors mean "not yet", an answer with
   `updater` means the updater serves.
4. The updater finds the upload instruction, joins WiFi with the application's credentials, logs its
   address and starts `esp_http_server` on port 80. The page sends the file with
   `PUT /firmware` (`Authorization: Bearer <token>`, an `XMLHttpRequest` for the progress events) and
   shows the percentage, then "verifying".
5. The updater checks the first 4 KB before it touches the flash (`hc::check_app_image`: an ESP image
   for this chip whose app descriptor names `homecontrol`; the updater's own binary, another
   project's binary or an error page is refused with 422 and the reason), streams the body into ota_0
   (`esp_ota_begin/write/end`; `esp_ota_end` verifies the image), stores `update.uploaded` = the
   image's version, erases the instruction and the token, selects ota_0 for boot, answers
   `{"ok": true, "version": "<version>"}` and restarts a second later.
6. The new application confirms itself as after a download (first boot, rollback: "Rollback and power
   loss" applies unchanged) and logs `update to X completed (uploaded from the browser)`; the page
   polls the open `GET /api/auth/session` until it answers and shows version and build. Its session
   is gone with the restart: *Reload* leads to the login.

**The updater's routes** (port 80, one server task: the handlers run one after the other, so a
`/status` request during an upload waits until the upload ends; refusals are
`{"error": "<reason>", "code": "<key>"}` and logged under `hc.updater`):

| Route | Answer |
|---|---|
| `GET /` | the self-contained page (English, inline CSS and JS) for a browser that arrived by a reload: with the token in `sessionStorage` it offers the file chooser and uploads; without, it says that the upload is started from the application's Settings page and offers *Start the application* |
| `GET /status` | `{"updater": "<descriptor version>", "state": "awaiting_upload" \| "writing" \| "verifying" \| "done" \| "failed", "received": <bytes>, "expected": <bytes>, "remaining_s": <seconds until the old application boots, null without a limit>, "error": "<last failure>"}` |
| `PUT /firmware` | the raw image with `Authorization: Bearer <token>` and a `Content-Length`. 200 `{"ok": true, "version": "..."}`; 401 `updater.unauthorized` (missing or wrong token, the flash untouched); 409 `updater.busy` (an upload runs or is done); 411 `updater.length_required`; 413 `updater.too_large` (larger than ota_0); 422 `updater.wrong_image` (the header check, flash untouched) or `updater.verify_failed` (`esp_ota_end`, with the reason); 400 `updater.connection` (the body ended early); 500 `updater.flash` |
| `POST /abort` | no token needed: it only boots the old application, which the timeout does anyway. 200 `{"ok": true}`; 409 `updater.no_application` (ota_0 missing or rolled back), `updater.busy` |

A failed upload leaves the instruction and the token in place: the page (or the updater's own page)
can upload again. After an interrupted write ota_0 is *missing* and the updater serves without a
limit.

**The token** lives only in NVS (`update.token`) and in the browser tab's `sessionStorage`; it is never
logged, compared in constant time (`hc::upload_authorized`) and erased with the instruction - after
the upload, by the application after a timeout or a cancel, or as part of a corrupt instruction.

**Same origin.** The hand-over through `sessionStorage` works because the updater answers at the same
origin as the application: the same IP address and port 80. The updater runs no mDNS (it stays
small), so a page opened as `http://homecontrol-xxxxxx.local/` could not reach it under that name, and
`sessionStorage` is per origin: the Settings page offers the installation only when it was opened by
the device's IP address, and otherwise shows the link `http://<IP>/` (log in there again). The DHCP
lease survives the restart in practice (same MAC, same host name, a few seconds); if the address
changes anyway, the page says so and the device is found in the router's list.

**Timeouts.** Without a complete upload within 600 s the updater boots ota_0 if it holds a valid image
(log `boot application: no firmware upload within 10 minutes ...`); the application then reports
`firmware upload abandoned: no image was installed` and erases the instruction. With ota_0 missing or
rolled back it serves without a limit and logs `waiting for the firmware upload at http://<IP>/ (no
time limit, ...)` every minute. A running upload is never cut off by the timeout. While an image streams in, the updater
waits up to 2 minutes for the next data (12 receive timeouts of 10 s, each logged `upload: no data
for n s`): on the ESP32-C6 the WiFi link loses segments while the flash is written, and TCP's
retransmission timeout doubles with each loss in a row (#69: 15 s measured, about 6 % of the bytes
sent again, ~14 KB/s). A failed upload keeps the one-time token while the updater runs, so the page
or a client can send the image again without a new one.

**Recovery with the button.** A client that loses the one-time token after ota_0 was overwritten (it
gave up, the browser tab was closed) cannot get a new one: the application that issues it is gone.
While ota_0 holds no bootable application, holding the device's button (`CONFIG_HC_WPS_BUTTON_GPIO`,
the BOOT button of the devkits, also the application's WPS button) for 3 s authorizes an upload
without the token for 5 minutes (log `button held: ...`, `button_s` in `GET /status`, a hint on
the updater's page and in the 401 answer; the status LED turns amber while the hold counts, then
blinks blue for the window, see "The status LED" above). The updater also watches the button while it waits with
no application and no instruction (`next step in 300 s ... or hold the button`), and then serves
the upload page. Security: the window needs someone at the device, who could flash it over USB as
well; a device with a working application is never opened this way.

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
| while the updater waits for an upload | updater (otadata selects it) | the instruction is still there: it waits again, the 10 minutes start over |
| during the upload's write | updater | ota_0 *missing*: it waits for an upload without a limit (the token is still valid) |
| after `update.uploaded` was stored, before ota_0 was selected | updater | no instruction, ota_0 valid: boots it; the application reports the upload |

The bootloader writes otadata with a sequence number and CRC in two alternating sectors, so an
interrupted otadata write leaves the previous selection.

## Security

HTTPS servers are authenticated against the IDF certificate bundle (common CAs), plain HTTP is
accepted for development. The manifest is signed with the release key whose public half the
firmware carries ("Manifest signature" above), and the SHA-256 in the signed manifest binds the
image: a malicious or compromised update source can withhold updates but not install an image the
release key did not announce. The binary itself is not signed separately, and secure boot and flash
encryption stay off (irreversible on the chip, development boards are shared; #20). The browser
upload is authorized by the device's own session or token, not by the release key.

## Releases and the web flasher

Decided on 2026-11-04 (ROADMAP.md): pushing a tag `vX.Y.Z` builds, signs and publishes the
release in GitHub Actions; the releases are turned into the GitHub Pages site
https://haumacher.github.io/homecontrol/, which is the web flasher, the update source and the home
of the language packs at once. (v1.0.0 was still built and signed on the owner's machine.)

**Publishing a release:**

```bash
# hc::kVersion (core/include/hc/version.h) is X.Y.Z, committed and pushed
scripts/release.sh --dry-run          # optional: the same build and signature locally
git tag -a vX.Y.Z -m 'HomeControl X.Y.Z' && git push origin vX.Y.Z
```

**The release key in CI.** The secret half of the release key is the secret `HC_RELEASE_KEY` (the
64 hex digits of `~/.config/homecontrol/release-key.secret`, as `scripts/sign.py` reads them) of the
repository environment `release`, whose deployment policy admits only tags `v*`: no branch, pull
request or manual run of the workflow can read it. Only the job `sign` uses the environment; it
writes the key to a `mktemp` file (mode 600), signs, and shreds the file. The signature must verify
against the committed `platform/esp/release-key.pub` before anything is published. The reasons:
E-Home's apt signing key already lives in CI the same way, a tag-only environment protects the key
as well as a single machine does, and every published binary now has a public build log on the
standard ESP-IDF image. The owner's copy stays the backup and signs `scripts/release.sh`'s check.

**`.github/workflows/release.yml`** (on a pushed tag `v*`):

1. `build`: the tag must be `v` + `hc::kVersion`; the host tests; the partition check; the updater
   and the application with `espressif/esp-idf-ci-action` (ESP-IDF v5.5.6) in fresh build
   directories `build-esp32c6-release` (`-D IDF_TARGET=esp32c6 -D HC_FLASH_SIZE=4MB -D
   HC_EXTRA_CA=`, `HC_RELEASE_KEY` the committed default); `scripts/release-assets.py assemble`
   checks the builds (the release key in `hc_generated/hc_release_key.h`, no extra CA, no updater
   test option `HC_UPDATER_TEST_BUTTON_WINDOW`, the 4 MB table, the application's version) and
   writes the unsigned assets: the six flash parts, `install.json` (the ESP Web Tools manifest from
   the template `web/flasher/manifest.json`, its offsets checked against the build's
   `flasher_args.json`), the update manifest `manifest.json` with `url` =
   `/homecontrol/firmware/<version>/homecontrol.bin`, the language packs (`scripts/i18n-pack.py
   manifest`, as `i18n-manifest.json` and `lang-<code>.json.gz`), and `homecontrol.elf` and
   `hc_updater.elf` for decoding core dumps (release assets only).
2. `sign` (environment `release`): `scripts/release-assets.py sign` writes `manifest.sig`, verifies
   it, writes `SHA256SUMS` and the site preview.
3. `publish`: `gh release create --verify-tag` with every asset (a version with a pre-release
   suffix becomes a GitHub pre-release).
4. `pages`: calls `pages.yml` as a reusable workflow. A release created with `GITHUB_TOKEN` starts
   no other workflow, so the `release: published` trigger of `pages.yml` would not fire.

**Dry run.** Started by hand (`gh workflow run release.yml --ref master`), the workflow builds and
assembles the current branch, signs with a key generated in the job (the environment refuses a
branch, so the release key is never available to it) and verifies against that key's public half;
it creates no release and deploys nothing. The assets and the site preview are the run's artifacts.

**`scripts/release.sh`** is the same check on the owner's machine and publishes nothing:
a clean tree, not behind its upstream, ESP-IDF v5.5.6, warnings when HEAD is not on the tag of
`hc::kVersion` or the release exists; the host tests in `build-release-host/`; the same `idf.py`
command lines in wiped `build-esp32c6-release` directories; `release-assets.py assemble` and `sign`
with the local key into `build-release/<version>/assets/`, and the site built from this release
alone into `build-release/<version>/site/`. `--dry-run` also prints the tag commands.

**The Pages site** (`.github/workflows/pages.yml`, source "GitHub Actions"; called by the release
workflow, and run when a release is published by hand, when `web/flasher/` or the site script
change on master, and by hand; the `github-pages` environment admits `master` and tags `v*`). The
deploying job holds the concurrency group `pages` in every workflow that runs it, so two deployments
never run at once. GitHub Pages takes only the first deployment of a commit (a later one of the same
SHA reports success and changes nothing): when the tagged commit itself changed `web/flasher/`, the
master run has deployed that commit before the release existed, and the release's deployment is
ignored; then run `gh workflow run pages.yml` after the next commit on master. It downloads
the GitHub releases with `gh`: the newest five completely, the older ones only their language
packs. The releases are the store; nothing is fetched from the live site, so a deployment is
reproducible from the releases alone (deleting a GitHub release takes it off the site at the next
run). A release takes about 3.6 MB of the site (the ELF files stay in the release only; `SHA256SUMS` lists them too), five about 18 MB, far below the 1 GB limit of Pages.

| Path | Content |
|---|---|
| `/`, `/de/` | the flasher page (English, German), `flasher.css`, `flasher.js` (`web/flasher/`) |
| `/manifest.json` | ESP Web Tools manifest of the stable release, parts in `firmware/<version>/` |
| `/releases.json` | the releases on the site and the stable one (the page's release choice) |
| `/firmware/<version>/` | the flash parts, `install.json`, `manifest.json` + `manifest.sig`, `SHA256SUMS`, `i18n/` |
| `/stable/manifest.json`, `/stable/manifest.sig` | the stable release's signed update manifest, byte for byte |
| `/beta/manifest.json`, `/beta/manifest.sig` | the same of the newest release, pre-releases included |
| `/stable/<version>/i18n/`, `/beta/<version>/i18n/` | the language packs of every release ever published |

The **update source** of an installed controller is its **update channel** on this site (Settings,
Update channel; since 2026-11-07, before which the owner had to enter a URL): `stable` (the default)
is `https://haumacher.github.io/homecontrol/stable`, `beta` the same with `beta`, `off` none. There
is no URL to enter, so none to get wrong; a development update server is the console's `update url
set`. The Pages site is the update server for the GitHub releases rather than the release downloads
of github.com themselves: `releases/latest/download/manifest.json` answers with two redirects to
release-assets.githubusercontent.com, whose URLs exceed the 127 characters of an image URL, the
release assets are flat (no `<version>/i18n/` beside the manifest), and a channel could not be
pointed back to an older release. The device reads `<channel>/manifest.json` and `.sig`, the image
from `/homecontrol/firmware/<version>/` (the leading `/` keeps scheme and host), and its language
pack from `<channel>/<version>/i18n/`. Because the image URL is absolute within the site, the same
signed bytes serve from `stable/`, `beta/` and `firmware/<version>/`. The stable release is the
newest one without a pre-release suffix; the workflow run by hand with `stable` = an older version
on the site points the channel back to it (devices that already updated stay, `hc::check_update`
installs only newer versions; devices not yet updated stop at the older one, and the flasher offers
it). The beta channel is the newest release, pre-releases (`vX.Y.Z-beta.N`, published as GitHub
pre-releases) included, and never older than stable: while no pre-release is newer it serves the
stable release.

**What the web flasher writes, and the erase decision.** The ESP Web Tools manifest has the parts
bootloader (0x0), partition table (0x8000), updater (0x10000), application (0x100000), the otadata
that selects ota_0 (0x3b0000) and the empty LittleFS `storage.bin` (0x3b2000), all of the 4 MB
table, the only one the page offers (it runs on 8 and 16 MB modules, which then use 4 MB). NVS
(0x9000), the core dump and the Zigbee partitions are never part of it. With
`new_install_prompt_erase: true` ESP Web Tools 10.4 decides like this (its `install-dialog.js`):

- a board that answers Improv with the firmware name `HomeControl` (opening the port resets it, so
  an installed controller always offers Improv then) gets *Update HomeControl*, written **without
  erase**: WiFi, password, remembered sessions, the E-Home enrollment and installation key, the
  settings and the Zigbee network survive; the storage (runtime image, values, history, language
  pack) is replaced by the empty image, because the firmware never formats an unreadable
  partition itself (`hc_storage`) and so the flasher cannot leave it out for a new board. E-Home
  deploys the image again;
- any other board (new, erased, another firmware, or HomeControl crashing too early for Improv)
  gets *Install HomeControl* and the question *Erase device*, **unticked by default**: ticked, the
  whole flash is erased first; unticked, the parts are written over whatever is there.

Without `new_install_prompt_erase` ESP Web Tools would erase every board that does not answer
Improv, without asking: a controller that crashes at boot would lose its NVS on a repair attempt.
The page explains both paths, recommends the over-the-air update or the browser upload (which keep
everything) for an installed controller, and offers older releases on the site through
`firmware/<version>/install.json`.

**The web flasher on the C6's own USB port.** ESP Web Tools 10.4 needs two adaptations there,
both in `web/flasher/flasher.js` (measured on the bench with a Chrome under the policy
`SerialAllowUsbDevicesForUrls` and Playwright, November 2026):

- The port dialog lists every serial device of the computer (a laptop's mobile modem shows up as a
  "Rolling Wireless" port): the page filters it to Espressif's vendor id 0x303a, the chip's own USB
  port.
- After writing, ESP Web Tools resets with the RTS line. On a board that came up in download mode
  when it was plugged in (a new or erased board: its ROM finds no firmware and waits for the
  download over USB, without restarting), that reset starts the download mode again: HomeControl
  does not start, Improv is not found and the WiFi setup never comes; RESET or a re-plug was needed.
  A board that ESP Web Tools put into download mode itself (an update of a running HomeControl)
  starts the firmware after that reset. The page therefore takes ESP Web Tools' open of the port
  after the installation (for Improv): if the ROM answers SYNC there, it resets the chip with the
  LP watchdog (WRITE_REG on `LP_WDT_RWDT_*` at 0x600b1c00, the sequence esptool's `watchdog_reset`
  uses on the ESP32-C3; esptool 4.12 leaves it out on the C6 because the USB port disappears with
  the reset), waits for the board to come back on USB (0.4 s on the bench), sets the dialog's port
  to it and opens it; ESP Web Tools then finds Improv and offers *Connect to Wi-Fi*. If the board
  does not come back within 8 s, the page asks for RESET and continues as soon as it is there.
  When the firmware runs already, the ROM does not answer and nothing happens.

A bench trap: pyserial (esptool, miniterm) leaves `VMIN=0` on the tty, and the kernel keeps the
setting for the device node across opens and re-plugs. Chrome opens the port non-blocking without
setting `VMIN`, so every quiet moment reads as end of stream, which Chrome reports as "The device
has been lost". After any pyserial use on the node: `stty -F /dev/ttyACMx min 1 time 0`.

## TLS records

TLS sends a body in records of up to 16 KB. With `CONFIG_MBEDTLS_DYNAMIC_BUFFER`
(`sdkconfig.defaults.app`, doc/ehome.md) the application allocates its receive buffer at the size of
each record, so a 16 KB record needs a free block of about 16.7 KB while it is read. The application
asks every server for records of at most 4 KB (RFC 6066 Max Fragment Length,
`platform/esp/main/tls_fragment.c`, as the PhoneBlock dongle does); measured on 2026-11-07:

| Server | Honours 4 KB | Records of a large body |
|---|---|---|
| apps.haumacher.de (E-Home) | yes | at most 4 KB |
| github.com | yes | at most 4 KB (only redirects are fetched there) |
| haumacher.github.io (GitHub Pages, the update channels) | no | 16 401 bytes |
| release-assets.githubusercontent.com | no | 16 401 bytes |

The application fetches only small bodies from GitHub: the manifest (at most 1 KB), its signature
and the language packs (the German one of 1.0.0 has 10 132 bytes). Pages sends a body that small in
one record of its size, so the receive buffer, allocated per record, takes a block of the pack's
size, not 16.7 KB. `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` stays at the default 16384 all the same: with
dynamic buffers it is only the limit of a record, it costs no memory, and a smaller limit would refuse
the record of a pack above it (`MBEDTLS_ERR_SSL_INVALID_RECORD`). The image, the only large download,
is the updater's: it runs alone, with the static buffers of `sdkconfig.defaults` (16 KB + 4 KB), and
receives the 16 KB records of GitHub's servers without strain. Where the 4 KB request pays is E-Home:
an image E-Home deploys over the WebSocket (up to 148 KB) arrives in 4 KB records instead of 16 KB
ones, while the application runs everything else.

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

The firmware on the board must trust the server's signing key: build it with
`-DHC_RELEASE_KEY=$PWD/scripts/dev-release-key.pub` (the server prints the line; "Manifest
signature" above), or the check reports `manifest refused: ...`.

**Before anything is erased: the backup.** `erase_flash`, `erase_region`, `write_flash --erase-all`
and a `write_flash` at a wrong offset destroy what no build can bring back: the WiFi credentials, the
device password, the tokens, the E-Home enrollment and the installation key (NVS), the image, the
saved values and the history (storage), the Zigbee network (`zb_storage`, `zb_db`). On 2026-11-04 a
`write_flash ... --erase-all` meant for the updater alone erased all of it on the desk board; the
recovery took WPS, a new password, a new enrollment and a new Zigbee network. So:

1. Read the whole flash first and check it: `esptool.py --chip esp32c6 --port <port> read_flash 0
   0x400000 full.bin` (14 s on the 4 MB board), then `verify_flash 0x10000 <the same bytes>` over
   the partitions that do not change while the board runs (updater and application). NVS and
   storage change by themselves between two reads (a few bytes of a running board), so a full
   `verify_flash` reports a mismatch there; that is expected.
2. Write only the partition the change needs, by its offset (`0x10000` the updater, `0x100000` the
   application, `0x3b0000` otadata on the 4 MB layout); never `--erase-all`, never `erase_flash`.
3. Read the command line in full before running it, wrappers and flags included.
4. If something was lost after all: `write_flash 0x9000 nvs.bin` (and the other data partitions)
   from the backup, cut out of `full.bin` with `dd`, restores the board as it was.

**Testing the recovery path without erasing the application.** The updater's menuconfig option
*HomeControl updater testing → TEST ONLY: take the idle path* (`HC_UPDATER_TEST_BUTTON_WINDOW`, with
`HC_UPDATER_TEST_BUTTON_WINDOW_AFTER_S`) builds an updater that treats ota_0 as empty, opens the
button window by itself that many seconds after boot, accepts a token-less upload over a valid
application, and logs what a scan sees after each failed connect (`test scan: ...`). Build it in its
own directory (`idf.py -B build-esp32c6-test menuconfig`), then:

```bash
esptool.py --chip esp32c6 --port <port> --after no_reset write_flash 0x10000 updater/build-esp32c6-test/hc_updater.bin
esptool.py --chip esp32c6 --port <port> --after hard_reset erase_region 0x3b0000 0x2000   # otadata: boot the updater
curl -T build-esp32c6/homecontrol.bin http://<IP>/firmware   # no token while the window is open
```

The upload selects ota_0 again (which rewrites otadata) and restarts into the application. Flash the
normal updater back to 0x10000 afterwards: the test build lets anyone on the network flash the
device without the token while its window is open.

**Upload from the browser instead of USB.** With an updater that serves uploads on the board (flashed
once over USB with the procedure above), a new application build is installed from the browser:
open `http://<the board's IP>/` (by address, not by its `.local` name), Settings → System →
*Install firmware file*, choose `platform/esp/build-esp32c6/homecontrol.bin`. The log shows
`hc.ota` `firmware upload: instruction stored ...`, then `hc.updater` `waiting for the firmware upload
at http://<IP>/ ...`, `upload: homecontrol <version>, <n> bytes from the browser`, `upload of <version>
written and verified; restarting into it`, and after the restart `hc.ota` `update to <version>
completed (uploaded from the browser)`. A machine does the same with curl:
`curl -X POST -H 'Authorization: Bearer hct_...' http://<IP>/api/system/update/upload` (note `url`
and `token`), then after the restart
`curl -T homecontrol.bin -H 'Authorization: Bearer <token>' <url>firmware`.

On the console: `update now http://<desk>:8070` installs the served build whatever its version;
`update url set http://<desk>:8070` and `update check` install it only if it is newer (bump
`kVersion` in `core/include/hc/version.h` for the second build). The updater's log lines carry the
tag `hc.updater`, the application's `hc.ota`.
