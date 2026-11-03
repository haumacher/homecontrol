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
| no `manifest.sig` (404) | `manifest refused: the manifest is not signed (no manifest.sig next to it; the release key is dfba 89a7 15ad 2217)` |
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
application's credentials (no WPS, no console, no mDNS, no status LED), esp_http_client,
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

**The updater's own update is by USB only.** It is small and rarely changes; it cannot replace
itself while it runs, and the application does not write the factory partition. Flash it with the
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
time limit, ...)` every minute. A running upload is never cut off by the timeout.

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
