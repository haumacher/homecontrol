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
  (`Connectivity`, `Backoff`), the network name (`NetworkName`: picked once over the platform's
  `NameService`, stored as `net.hostname`, changed by the user; doc/network.md), Improv Wi-Fi serial provisioning (`Improv`, `ImprovReader`), status
  LED patterns, settings keys and validators, the datapoint model (`DatapointModel`, `Technology`)
  and the KNX DPT codecs (`doc/datapoints.md`), KNXnet/IP tunnelling (`hc::knx`: `KnxTunnel`,
  `KnxAdapter`, the telegram tap `TelegramLog`, the gateway search `GatewaySearch` over the platform's
  `UdpSocket`, `doc/knx.md`), the time service,
  astronomy and the scheduler (`doc/time.md`), the KNX time master, last-value persistence and
  short-term history over the platform's `BlobStore` (`ValueStore`, `History`, `doc/persistence.md`),
  the storage figures the status reports, measured off the loop after each change (`StorageStats`),
  the web server's portable half (JSON API `WebApi`, WebSocket feed `LiveFeed` and codec `websocket`,
  `Auth`, `JsonWriter`/`json_parse`, `LogRing`, SHA-256/PBKDF2, the WebSocket send ring `WsSendRing`;
  `doc/web.md`), the monitor stream
  for E-Home's group monitor (`MonitorFeed`: KNX, Zigbee and the controller's actions, spec/api.md
  "Monitor"), the rule interpreter (`hc::rules`, `doc/rules.md`), the blocks (`hc::blocks`: the
  block type interface and registry, the loader of the image's `blocks`, the runtime the rule engine
  calls inside its cascades, the room controller; `spec/blocks.md`, `doc/blocks.md`), the runtime image (`hc::image`:
  the binary image read in place `bin::View`, its `validator`, the `Loader` of its plan, `ImageService` over
  the platform's `ImageSlot` (RAM `MemoryImageSlot`), the RAM stand-in `MemoryBlobStore`, dynamic datapoints of
  providers such as the Zigbee adapter through `DynamicSpecProvider`; `doc/image.md`), the Zigbee
  adapter (`hc::zigbee`: `ZigbeeAdapter`, `DeviceDatabase`, the cluster mapping, over the platform's
  `ZigbeeDriver`; `doc/zigbee.md`), external values from HTTP JSON sources (`ExternalValues`,
  `doc/external.md`), the one fetch worker of every HTTP request (`FetchQueue`, `doc/external.md`), the TLS gate that starts one TLS session at a time with room in the DMA-capable memory (`TlsGate` over the platform's `MemoryWatch`, `doc/core.md`), the language packs of the web UI (`LanguagePacks`, the `ui.lang` setting;
  `doc/i18n.md`), the MQTT client (`Mqtt`: states, commands, Home Assistant discovery, history
  export over the platform's `MqttClient`; `spec/mqtt.md`, `doc/mqtt.md`), and the E-Home
  connection (`EhomeLink`: enrollment, the outbound WebSocket over the platform's
  `WebSocketClient`, requests under the "E-Home" principal; spec/api.md "E-Home connection",
  `doc/ehome.md`). Plain C++17, no ESP-IDF or POSIX includes; everything the platform provides
  (clock, settings, time zone, network driver, status light, logging, the Improv serial port, the
  WiFi scan, the diagnostics, the sockets and fetchers) is an interface in
  `core/include/hc/platform/`. Unit tests in `core/test/`. `core/hc_core.cmake` is the single
  source list of both builds; the event loop model is in `doc/core.md`, the network bring-up in
  `doc/network.md`.
- `platform/posix/` — the Linux build: implements the platform interfaces with POSIX (settings in a
  text file), builds the controller as an ordinary executable (the simulator, the SBC fallback and
  the Linux package, `doc/linux.md`; options and environment in `options.h`) and the test runner
  (core tests plus `platform/posix/test/`, and `service_test.py`, the executable as the package runs
  it).
- `platform/esp/` — the ESP-IDF project: `main/` (boots NVS, the controller, the network and the
  console; `idf_component.yml` lists the managed components `mdns`, `led_strip` and
  `esp_websocket_client`, and `esp-zigbee-lib`/`esp-zboss-lib` for the esp32c6 and esp32h2, pinned
  in the versioned `dependencies.lock.<target>`), `components/` (`hc_core` compiles `core/` as a
  component; `hc_status_light` the status LED, shared with the OTA updater; `hc_platform` the
  `hc::esp` platform: clock, NVS settings, time zone, event-loop queue, the system facts for the System page, the diagnostics `EspDiagnostics` (reset
  reason, boot record in NVS `diag`, core dump, task watchdog on the event loop;
  `doc/diagnostics.md`), the console output `ConsoleOutput` (all of stdout through a 4 KB
  `hc::OutputRing` to the writer task `hc_out`, so no task waits for the USB host; drops are counted;
  `doc/diagnostics.md` "Console output"); `hc_net` WiFi station with WPS and the scan, mDNS, SNTP, the WPS button;
  `hc_improv` the USB Serial/JTAG port shared by Improv and the console (reader task, console input
  device); `hc_console` the serial console and its command table (grouped `help`, `status` with the
  boot record and the task stacks, `log level|tail`, `coredump status|clear`, `crash yes`, `factory
  reset yes`; `doc/diagnostics.md`); `hc_settings` the NVS settings, shared with the updater;
  `hc_ota` the application's side of the OTA update: confirmation, update check, hand-over to the
  updater, the firmware upload instruction; `hc_web` the web server: esp_http_server with the
  core's API and WebSocket feed, the UI files from the LittleFS image, the `web` console command;
  `hc_knx` the KNX tunnel; `hc_image` the `image` console command and its download; `hc_zigbee` the
  coordinator; `hc_storage` the one mount of the LittleFS `storage` partition at `/storage` and the
  BlobStore in `/storage/state` (`LittleFsBlobStore`, the `storage` console command,
  `doc/persistence.md`); `hc_external` the HTTP fetcher; `hc_i18n` the language pack download and
  the `lang` console command; `hc_mqtt` the MQTT client; `hc_ehome` the E-Home connection
  (`EspWebSocketClient` on the managed component `esp_websocket_client`)), `updater/` (the small
  OTA updater app of the asymmetric partition layout, its own ESP-IDF project: downloads from the
  update server or takes the firmware upload from the browser, shows its states on the status LED;
  its managed `led_strip` pinned in the versioned `updater/dependencies.lock.<target>`; `doc/ota.md`),
  `partitions-<size>.csv` (generated by `tools/gen_partitions.py`; the `coredump` partition, 64 KB,
  sits between `storage` and the Zigbee partitions), `hc_project.cmake` (flash size and table
  selection shared by both projects), `sdkconfig.defaults` (shared by both projects) and
  `sdkconfig.defaults.app` (the application only, e.g. the web server's HTTP settings, the
  FreeRTOS run-time statistics, the mbedTLS dynamic buffers); the OTA design is `doc/ota.md`.
- `spec/` — the contract with E-Home: the runtime image format (`spec/runtime-image.md`, fixtures
  and the KNX test-system template in `spec/fixtures/images/`), the Zigbee bindings and export
  (`spec/zigbee.md`), the REST and WebSocket API including the E-Home connection (`spec/api.md`),
  the rule language, the blocks (`spec/blocks.md`), and the conformance fixtures both rule
  interpreters and block runtimes (C++ here, Java in E-Home) must pass (`spec/fixtures/rules/`,
  `spec/fixtures/blocks/`).
- `web/` — the device web UI in `web/ui/dist/` (plain ES modules and CSS, no framework, no build
  step; `doc/web.md`), gzip-compressed and compiled into the firmware by
  `platform/esp/tools/gen_ui_files.py` during the firmware build (an OTA update brings it along); every text goes through `t(key)`
  over the English base `web/ui/dist/lang/en.json`, the committed language packs are
  `web/ui/lang/<code>.json` (German first; same key set, checked by `scripts/i18n-pack.py check`
  and the host test; `doc/i18n.md`); `web/flasher/` the web flasher (ESP Web Tools page and
  manifest; local firmware copies from `scripts/flasher-local.sh` in `web/flasher/firmware/` are
  not versioned).
- `scripts/` — developer helpers (`flasher-local.sh`, `update-server.py`: the development OTA update
  server, also serving the release's language packs; `i18n-pack.py`: pack check, DeepL fill-in with
  `DEEPL_AUTH_KEY`, the per-release i18n manifest; `c6-model/`: the core and the swap memory test with
  the C6's sizes under `qemu-riscv32-static`, doc/image.md "Memory of a swap").
- `doc/` — design notes; `doc/hardware.md` for boards, flash sizes, partition tables and wiring,
  `doc/diagnostics.md` for the reset reason, the boot record, core dumps, the watchdog and the
  console, `doc/core.md` for the core and its event loop, `doc/ota.md` for the OTA update and the
  firmware upload, `doc/i18n.md` for the UI languages and language packs, `doc/ehome.md` for the
  E-Home connection.
- `spike/blocks-wasm/` — the WebAssembly block spike (#59, `doc/blocks-wasm.md`): the draft block
  ABI, the test block in C/Rust/Zig with its built modules, the host test on WAMR
  (`spike/blocks-wasm/host`, its own CMake project), the Chicory test (`java/`) and the C6
  measurement app (`esp/`, flashed to ota_0 only). Not part of the core or the firmware build.
- `third_party/` — vendored code with its license (doctest, the unit test framework; cpp-httplib,
  the POSIX build's HTTP server; Monocypher 4.0.2, the core's Ed25519 for signed runtime images and
  update manifests, compiled as C into both builds).
- `debian/` — the Debian package `homecontrol` (the controller as a Linux service: systemd unit,
  `/etc/default/homecontrol`, data in `/var/lib/homecontrol`, the UI in `/usr/share/homecontrol/www`;
  `build-deb.sh`, `ci-build.sh`, `hc-version`, the apt repository script `apt-repo.sh` and its page
  `apt-index.html`; `doc/linux.md`).
- `.github/workflows/ci.yml` — host build with tests; the esp32c6 firmware build (partition table
  check, application, updater); the Debian package for trixie amd64.

## Build, run, verify

- **Host**: `cmake -S platform/posix -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure`.
  The core's tests are the fast edit loop; run them after every core change.
- **ESP-IDF v5.5.6** is installed at `~/tools/esp/esp-idf-v5.5` (tools in `~/.espressif`; the v5.3 in
  `~/tools/esp/esp-idf` belongs to other projects). Every Bash
  call that uses it must source it first: `source ~/tools/esp/esp-idf-v5.5/export.sh` (the shell state
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
  $(readlink -f /dev/serial/by-id/*E8:F6:0A:F8:83:EC*) --before no_reset --after hard_reset write_flash @flash_args` (bootloader, table,
  updater, application at 0xf0000, `image-empty.bin` (one erased sector at the `image` partition: no
  runtime image), otadata, `storage.bin`, an empty LittleFS image of the storage partition, which wipes
  the saved state; `@app-flash_args` writes only the application). Watch with
  `idf.py -B build-esp32c6 -p <the C6's node> monitor` (exit with Ctrl-]). OTA updates during
  development: `scripts/update-server.py` and the console's `update now` (`doc/ota.md`).
- **Run on the host**: `build/homecontrol [--settings <file>] [--http <port>] [--bind <address>] [--demo]`
  (default `homecontrol.settings` in the working directory; the image, the blobs and the run marker
  live beside it; format in `doc/network.md`); the web UI and API on http://localhost:8081/
  (WebSocket on port 8082; `HOMECONTROL_HTTP_PORT`/`HOMECONTROL_HTTP_BIND` set both too), `--demo`
  loads simulated datapoints; stop with Ctrl-C.
- **Linux package** (`debian/`, `doc/linux.md`): `docker run --rm -v "$PWD":/src -w /src debian:trixie
  debian/ci-build.sh` (or `debian:bookworm`; `--nocheck` skips the tests) builds
  `dist/homecontrol_<version>_<codename>_<arch>.deb` with the distribution's hardened `-O2` flags,
  which find warnings the Debug host build does not; the release workflow builds Bookworm and Trixie
  for amd64, arm64 and armhf, and pages.yml publishes the signed apt repository under `/apt`
  (`debian/apt-repo.sh`). A systemd container for tests must mask `systemd-binfmt`,
  `systemd-sysctl` and `systemd-modules-load`: privileged, they change the host's binfmt and sysctl
  settings.
- **Provisioning the device**: from the browser with the web flasher (`web/flasher/`, ESP Web Tools
  flashes and then sends the credentials over Improv Wi-Fi serial; offered for 5 minutes after
  boot, until 60 s after the link first came up); or a board without WiFi credentials listens for
  WPS (LED blinks fast): press the router's WPS button; to re-pair hold BOOT 3 s; fallback on the
  serial console (prompt `hc>`, in the monitor): `wifi set <ssid> <psk>`, then `wifi status`
  (`doc/network.md`).
- **Boards on the desk**: two Espressif USB-JTAG boards are connected. **Use only the ESP32-C6**
  (MAC E8:F6:0A:F8:83:EC); resolve its port by id every time, never by a remembered number:
  `readlink -f /dev/serial/by-id/*E8:F6:0A:F8:83:EC*` (it moves between `/dev/ttyACM0` and
  `/dev/ttyACM1` after a reset while a reader holds the old node). The ESP32-S3 (the other `ttyACM`) belongs to another project: never flash it.
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
