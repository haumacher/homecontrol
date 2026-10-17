# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

HomeControl: a smart-home runtime controller for an ESP32-C6 (WiFi, KNX over KNXnet/IP, Zigbee as
the goal), written in C++17 on ESP-IDF with a portable core that also builds and runs on Linux.
Engineering (project model, datapoints, rules, schedules, visualization pages) happens in the sibling
project E-Home (`../ehome`, TopLogic/Java); this project is the runtime that executes what E-Home
deploys. The GitHub repository is `haumacher/homecontrol` (the local checkout directory is called
`homedriver`); its issue tracker is the work queue. `ROADMAP.md` records direction and decisions.
The working method is the skill `homecontrol-workflow` in `.claude/skills/`.

## Layout

- `core/` — the portable library: the controller, the event loop, the network workflow
  (`Connectivity`, `Backoff`), Improv Wi-Fi serial provisioning (`Improv`, `ImprovReader`), status
  LED patterns, settings keys and validators, the datapoint model (`DatapointModel`, `Technology`)
  and the KNX DPT codecs (`doc/datapoints.md`), KNXnet/IP tunnelling (`hc::knx`: `KnxTunnel`,
  `KnxAdapter`, `TelegramLog` over the platform's `UdpSocket`, `doc/knx.md`), the time service,
  astronomy and the scheduler (`doc/time.md`), the KNX time master, last-value persistence and
  short-term history over the platform's `BlobStore` (`ValueStore`, `History`, `doc/persistence.md`),
  the web server's portable half (JSON API `WebApi`, WebSocket feed `LiveFeed` and codec `websocket`,
  `Auth`, `JsonWriter`/`json_parse`, `LogRing`, SHA-256/PBKDF2; `doc/web.md`); the rule interpreter (`hc::rules`, `doc/rules.md`); the runtime image (`hc::image`: `Loader`,
  `ImageService`, `ImageStore`, the RAM stand-in `MemoryBlobStore`; `doc/image.md`). external values from HTTP JSON sources (`ExternalValues`, `doc/external.md`); Plain C++17, no ESP-IDF or POSIX
  `Auth`, `JsonWriter`/`json_parse`, `LogRing`, SHA-256/PBKDF2; `doc/web.md`); the monitor stream for E-Home's group monitor (`MonitorFeed`: KNX, Zigbee and the controller's actions, spec/api.md "Monitor"); the rule interpreter (`hc::rules`, `doc/rules.md`); the Zigbee adapter (`hc::zigbee`: `ZigbeeAdapter`, `DeviceDatabase`, the cluster mapping, over the platform's `ZigbeeDriver`, `doc/zigbee.md`), the runtime image (`hc::image`, `doc/image.md`; dynamic datapoints of providers such as the Zigbee adapter, `DynamicSpecProvider`). Plain C++17, no ESP-IDF or POSIX
  includes; everything the platform provides (clock, settings, time zone, network driver, status
  light, logging, the Improv serial port, the WiFi scan, the diagnostics) is an interface in `core/include/hc/platform/`. Unit tests in `core/test/`.
  `core/hc_core.cmake` is the single source list of both builds; the event loop model is in
  `doc/core.md`, the network bring-up in `doc/network.md`.
- `platform/posix/` — the Linux build: implements the platform interfaces with POSIX (settings in a
  text file), builds the controller as an ordinary executable (the simulator and the SBC fallback)
  and the test runner (core tests plus `platform/posix/test/`).
- `platform/esp/` — the ESP-IDF project: `main/` (boots NVS, the controller, the network and the
  console; `idf_component.yml` lists the managed components `mdns` and `led_strip`, and `esp-zigbee-lib`/`esp-zboss-lib` for the
  esp32c6 and esp32h2, pinned in the
  versioned `dependencies.lock.<target>`), `components/` (`hc_core` compiles `core/` as a
  component; `hc_platform` the `hc::esp` platform: clock, NVS settings, time zone, status LED,
  event-loop queue, the diagnostics `EspDiagnostics` (reset reason, boot record in NVS `diag`, core
  dump, task watchdog on the event loop; `doc/diagnostics.md`); `hc_net` WiFi station with WPS and the scan, mDNS, SNTP, the WPS button; `hc_improv` the USB
  Serial/JTAG port shared by Improv and the console (reader task, console input device);
  `hc_console` the serial console and its command table (grouped `help`, `status` with the boot
  record and the task stacks, `log level|tail`, `coredump status|clear`, `crash yes`, `factory reset
  yes`; `doc/diagnostics.md`); `hc_settings` the NVS settings, shared with
  the updater; `hc_ota` the application's side of the OTA update: confirmation, update check,
  hand-over to the updater; `hc_web` the web server: esp_http_server with the core's API and
  WebSocket feed, the UI files from the LittleFS image, the `web` console command; `hc_knx` the
  KNX tunnel; `hc_image` the `image` console command and its download; `hc_zigbee` the coordinator; `hc_storage` the LittleFS mount and the blob store; `hc_external` the HTTP fetcher), `updater/` (the small OTA updater app of the
  WebSocket feed, the UI files from the LittleFS image, the `web` console command; `hc_storage` the
  one mount of the LittleFS `storage` partition at `/storage` and the BlobStore in `/storage/state`
  (`LittleFsBlobStore`, the `storage` console command, `doc/persistence.md`); later knx,
  zigbee, …), `updater/` (the small OTA updater app of the
  asymmetric partition layout, its own ESP-IDF project), `partitions-<size>.csv` (generated by
  `tools/gen_partitions.py`; the `coredump` partition, 64 KB, sits between `storage` and the Zigbee
  partitions), `hc_project.cmake` (flash size and table selection shared by both
  projects), `sdkconfig.defaults` (shared by both projects) and `sdkconfig.defaults.app` (the
  application only, e.g. the web server's HTTP settings); the OTA design is `doc/ota.md`.
- `spec/` — the contract with E-Home: the runtime image format (`spec/runtime-image.md`, fixtures
  and the KNX test-system template in `spec/fixtures/images/`), the rule language, and the
  conformance fixtures both rule interpreters (C++ here, Java in E-Home) must pass.
- `web/` — the device web UI in `web/ui/dist/` (plain ES modules and CSS, no framework, no build
  step; `doc/web.md`), gzip-compressed into the LittleFS image `storage.bin` by
  `platform/esp/tools/gen_ui_image.py` during the firmware build; the REST and WebSocket API is
  `spec/api.md`;
  `web/flasher/` the web flasher (ESP Web Tools page and manifest; local firmware copies from
  `scripts/flasher-local.sh` in `web/flasher/firmware/` are not versioned).
- `scripts/` — developer helpers (`flasher-local.sh`, `update-server.py`: the development OTA update
  server).
- `doc/` — design notes; `doc/hardware.md` for boards, flash sizes, partition tables and wiring,
  `doc/diagnostics.md` for the reset reason, the boot record, core dumps, the watchdog and the console,
  `doc/core.md` for the core and its event loop, `doc/ota.md` for the OTA update.
- `third_party/` — vendored code with its license (doctest, the unit test framework; cpp-httplib,
  the POSIX build's HTTP server).
- `.github/workflows/ci.yml` — host build with tests; the esp32c6 firmware build (partition table
  check, application, updater).

## Build, run, verify

- **Host**: `cmake -S platform/posix -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure`.
  The core's tests are the fast edit loop; run them after every core change.
- **ESP-IDF v5.3** is installed at `~/tools/esp/esp-idf` (tools in `~/.espressif`). Every Bash
  call that uses it must source it first: `source ~/tools/esp/esp-idf/export.sh` (the shell state
  does not persist between calls). Each target builds in its own directory, with its sdkconfig
  inside it (settings belong in `sdkconfig.defaults`):
  `cd platform/esp && idf.py -B build-esp32c6 set-target esp32c6 && idf.py -B build-esp32c6 build`
  (the same with `esp32s3` proves portability; never flash it). The OTA updater is its own project,
  built the same way in `platform/esp/updater` (`idf.py -B build-esp32c6 build` there); the
  application's `flash_args` includes it. The flash size (and with it the partition table) is the
  CMake variable `HC_FLASH_SIZE`, default 4MB; a changed `sdkconfig.defaults` or flash size needs the
  build directory's `sdkconfig` deleted. Flash the C6 from the build
  directory (the reset sequence of `idf.py flash` fails on this board; put it into download mode
  with BOOT held during RESET): `cd platform/esp/build-esp32c6 && esptool.py --chip esp32c6 --port
  /dev/ttyACM1 --before no_reset --after hard_reset write_flash @flash_args` (bootloader, table,
  updater, application at 0x100000, otadata, the web UI image `storage.bin` at the storage
  partition; `@app-flash_args` writes only the application). Watch with
  `idf.py -B build-esp32c6 -p /dev/ttyACM1 monitor` (exit with Ctrl-]). OTA updates during
  development: `scripts/update-server.py` and the console's `update now` (`doc/ota.md`).
- **Run on the host**: `build/homecontrol [--settings <file>] [--http <port>] [--demo]` (default
  `homecontrol.settings` in the working directory; format in `doc/network.md`); the web UI and API
  on http://localhost:8080/ (WebSocket on port 8081), `--demo` loads simulated datapoints; stop
  with Ctrl-C.
- **Provisioning the device**: from the browser with the web flasher (`web/flasher/`, ESP Web Tools
  flashes and then sends the credentials over Improv Wi-Fi serial; offered for 5 minutes after
  boot, until 60 s after the link first came up); or a board without WiFi credentials listens for
  WPS (LED blinks fast): press the router's WPS button; to re-pair hold BOOT 3 s; fallback on the
  serial console (prompt `hc>`, in the monitor): `wifi set <ssid> <psk>`, then `wifi status`
  (`doc/network.md`).
- **Boards on the desk**: two Espressif USB-JTAG boards are connected. **Use only the ESP32-C6**
  at `/dev/ttyACM1` (MAC E8:F6:0A:F8:83:EC; `ls -l /dev/serial/by-id/` shows which port is which
  after a replug). The ESP32-S3 at `/dev/ttyACM0` belongs to another project: never flash it.
  Details and flash sizes in `doc/hardware.md`.
- **KNX during development**: only the author's test system, whose address is in the developer's
  private notes, never in the repository. The author's home installation is off limits without
  their explicit permission; no build, fixture or test carries a real gateway address.
- Verify behavior changes by running the host tests, building both ESP targets, then flashing the
  board and reading the monitor (or the device's web UI) for anything that touches the platform.
- Commit to `master` and push to `origin/master` after every verified change (standing directive).
  Feature branches and pull requests are for external contributors.

## Code style

- C++17, `-Wall -Wextra -Werror` on the host build; the ESP-IDF build uses the IDF's warning set.
- Namespaces: `hc` for the core, `hc::posix` and `hc::esp` for the platforms. Headers under
  `core/include/hc/`, sources under `core/src/`.
- Indent with tabs. Every source file starts with:
  ```
  /*
   * Copyright (c) <year the file was created> Bernhard Haumacher
   */
  ```
- No exceptions and no RTTI in the core (the ESP build disables both); report errors as return
  values (`hc::Result`/`std::optional`). No dynamic allocation in steady-state paths of the
  event loop; allocate when a runtime image is loaded.
- Everything the core logs goes through `hc::Log`, not `printf`; the platforms route it.
- Persisted formats (the runtime image, NVS keys, LittleFS files) are frozen once a build that
  writes them may have been flashed; changes need a compatible read path and a fixture test.
