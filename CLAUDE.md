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

- `core/` — the portable library: datapoints, DPT codecs, runtime image, scheduler, astro, rule
  interpreter. Plain C++17, no ESP-IDF or POSIX includes; everything the platform provides (clock,
  storage, network, logging) is an interface in `core/include/hc/platform/`. Unit tests in
  `core/test/`. `core/hc_core.cmake` is the single source list of both builds; the event loop
  model is in `doc/core.md`.
- `platform/posix/` — the Linux build: implements the platform interfaces with POSIX, builds the
  controller as an ordinary executable (the simulator and the SBC fallback) and the test runner.
- `platform/esp/` — the ESP-IDF project: `main/` (the app and the `hc::esp` platform),
  `components/` (`hc_core` compiles `core/` as a component; later knx, zigbee, web, ota, storage, …), `updater/` (the small OTA updater app of the asymmetric partition layout),
  `partitions*.csv`, `sdkconfig.defaults*`.
- `spec/` — the contract with E-Home: the runtime image format, the rule language, and the
  conformance fixtures both rule interpreters (C++ here, Java in E-Home) must pass.
- `web/` — the device web UI (static single-page app), built into the LittleFS image.
- `doc/` — design notes; `doc/hardware.md` for boards, flash sizes and wiring, `doc/core.md` for
  the core and its event loop.
- `third_party/` — vendored code with its license (doctest, the unit test framework).
- `.github/workflows/ci.yml` — host build with tests and the esp32c6 firmware build.

## Build, run, verify

- **Host**: `cmake -S platform/posix -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure`.
  The core's tests are the fast edit loop; run them after every core change.
- **ESP-IDF v5.3** is installed at `~/tools/esp/esp-idf` (tools in `~/.espressif`). Every Bash
  call that uses it must source it first: `source ~/tools/esp/esp-idf/export.sh` (the shell state
  does not persist between calls). Each target builds in its own directory, with its sdkconfig
  inside it (settings belong in `sdkconfig.defaults`):
  `cd platform/esp && idf.py -B build-esp32c6 set-target esp32c6 && idf.py -B build-esp32c6 build`
  (the same with `esp32s3` proves portability; never flash it). Flash the C6 from the build
  directory (the reset sequence of `idf.py flash` fails on this board; put it into download mode
  with BOOT held during RESET): `cd platform/esp/build-esp32c6 && esptool.py --chip esp32c6 --port
  /dev/ttyACM1 --before no_reset --after hard_reset write_flash @flash_args`. Watch with
  `idf.py -B build-esp32c6 -p /dev/ttyACM1 monitor` (exit with Ctrl-]).
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
