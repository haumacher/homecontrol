# Diagnostics

What the controller tells about itself when something went wrong (issue #4): why it started, how the
run before ended, a core dump of the last crash, a watchdog on the event loop, and the serial console
to look at all of it. The log ring and `GET /api/log` are in `doc/web.md`.

| Piece | Where |
|---|---|
| The reset reason, the platform's boot facts (interface) | `core/include/hc/platform/diagnostics.h` |
| The `boot` member of the status, the facts as text | `core/include/hc/diagnostics.h`, `core/src/diagnostics.cpp` |
| Log levels per tag | `hc::Log::set_tag_level()` (`core/include/hc/log.h`) |
| The tail of the log ring | `hc::LogRing::read_tail()` |
| Device: reset reason, boot record, core dump, task watchdog | `platform/esp/components/hc_platform` (`EspDiagnostics`) |
| Device: the console | `platform/esp/components/hc_console` |
| POSIX: PID, start time, the previous run's signal | `platform/posix` (`PosixDiagnostics`) |

## The reset reason

The controller's first line names it (a warning after a crash or a brownout, so it stands out); the
second lists the platform's boot facts:

```
W (812) hc.ctl: HomeControl 0.1.0-dev (build 1a2b3c4) starting, reset reason: brownout
I (813) hc.ctl: boot: count 57, brownouts 4, previous_uptime_s 1260, coredump no
```

| Reason | Device (`esp_reset_reason()`) | POSIX |
|---|---|---|
| `power-on` | power applied | first start: no marker of a previous run |
| `software restart` | `esp_restart()`: `restart`, an update, a factory reset | the previous run stopped on SIGINT, SIGTERM or SIGHUP |
| `panic` | an exception or `abort()`; a core dump follows | the previous run died on SIGSEGV, SIGBUS, SIGFPE, SIGILL or SIGABRT |
| `task watchdog` | the event loop (or the idle task) was not fed for 10 s | - |
| `interrupt watchdog` | interrupts were blocked too long | - |
| `other watchdog` | RTC or main watchdog | - |
| `brownout` | the supply dropped (doc/hardware.md, "Supply and transmit power") | - |
| `USB reset` | the USB Serial/JTAG peripheral (esptool, the monitor opening the port) | - |
| `external reset`, `deep sleep wake-up`, `SDIO reset`, `JTAG reset`, `eFuse error`, `power glitch`, `CPU lock-up` | as named | - |
| `unknown` | not determinable | the marker of the previous run has no stop record: SIGKILL, power loss, an error exit during start-up |

The numbers of `hc::ResetReason` are stored in the boot record and are frozen.

## The device's boot record

NVS namespace `diag` in the `nvs` partition (separate from the settings, so `settings list` does not
show it), every key a u32. Frozen once flashed.

| Key | Content |
|---|---|
| `boots` | starts since the record began (incremented at every boot) |
| `reason` | this start's `hc::ResetReason` |
| `uptime` | seconds since boot of the current run, written every minute; at the next boot the previous run's uptime, at most a minute short; set to 0 at boot, so a run that dies within its first minute reports 0, not the run before |
| `brownouts` | starts after a brownout |

The boot update is one commit; the uptime is one 32-byte NVS entry and a commit per minute (a value
that did not change is not written; a loop that fell behind writes once, the periodic timer coalesces
missed periods). Wear: 1440 entries a day fill about 11 of the 4 KB NVS pages, spread over the
partition's six: about two erases per sector and day, some 130 years for 100,000 cycles. A factory
reset erases the record with the rest of NVS.

`status` on the console, `/api/status` and the log report it:

```json
"boot": {"reason": "task watchdog", "count": 58, "brownouts": 4, "previous_uptime_s": 360,
         "coredump": true, "coredump_task": "hc_loop", "coredump_pc": "0x4200a1b2"}
```

The POSIX build reports `{"reason": "software restart", "pid": 4052811, "started": 1792274312,
"previous_signal": "SIGTERM"}` (`previous_signal` is `none recorded` when the previous run left no
stop record, absent on a first start). Its marker is a text file beside the settings
(`homecontrol.settings` -> `homecontrol.run`): `pid <pid> started <unix s>`, and `signal <n>` appended
when the run stops (by `main` for SIGINT/SIGTERM, by an async-signal-safe handler for the fatal
signals, which then let the signal end the process as before).

`write_boot_status(json, controller.diagnostics())` writes the member; the platform passes its
`Diagnostics` in `hc::Platform::diagnostics`.

## Core dumps

The panic handler writes an ELF core dump (all tasks: the used part of each stack and the TCBs, CRC32)
to the `coredump` partition: 64 KB, between `storage` and the Zigbee partitions on every flash size
(`tools/gen_partitions.py`, doc/hardware.md). Configured in `sdkconfig.defaults.app`
(`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`, `..._DATA_FORMAT_ELF`); the updater does not write dumps. A new
crash overwrites an older dump.

At boot the application checks the partition (`esp_core_dump_image_check`); if a valid dump is stored
it logs the summary (`esp_core_dump_get_summary`, `esp_core_dump_get_panic_reason`), and keeps the dump
until `coredump clear`:

```
W (1210) hc.diag: core dump stored (11520 B, from this reset): task hc_loop, PC 0x4200a1b2, Store access fault; GET /api/coredump downloads it, 'coredump clear' erases it
```

"from this reset" when the reset reason is a crash, "from an earlier crash" otherwise. A crash reset
without a valid dump says so. A device whose partition table predates the diagnostics (an OTA update
does not change the table) logs `no coredump partition ...`: flash the full image once.

**Reading a dump.** `GET /api/coredump` streams the raw image (`application/octet-stream`, the
size of the dump; authenticated like every API path, since the dump holds memory contents; 404
`coredump.none` when none is stored, always on POSIX). Decode it against the ELF of the same build:

```bash
curl -s -H "Authorization: Bearer $TOKEN" http://<device>/api/coredump -o core.bin
esp-coredump info_corefile --core core.bin --core-format raw platform/esp/build-esp32c6/homecontrol.elf
```

or read it over the cable: `cd platform/esp && idf.py -B build-esp32c6 -p /dev/ttyACM1 coredump-info`
(reads the partition with esptool; if the board's reset sequence fails, put it into download mode
with BOOT held during RESET, doc/hardware.md). `info_corefile` prints the crashed task's registers and backtrace and
every task's state; `dbg_corefile` opens GDB on it.

## The task watchdog

The event-loop task subscribes to the IDF task watchdog when the loop starts and feeds it at the top of
every loop turn; a turn sleeps at most a second, so a working loop feeds at least once a second. The
timeout is 10 s, and a timeout panics (`CONFIG_ESP_TASK_WDT_TIMEOUT_S=10`,
`CONFIG_ESP_TASK_WDT_PANIC=y`): a loop stuck in a handler for 10 s leaves a core dump with the stuck
task's backtrace and starts again with the reason `task watchdog`. The idle task stays watched (a task
that never yields starves it). The start-up before the loop (WiFi, LittleFS, the runtime image, the
web server) is not watched; the line `event loop running <n> ms after the controller started` tells
how long it held the loop's timers back (the LED tick and the button poll report missed periods when
it exceeds their periods).

**The console no longer stalls the loop.** The console's start used to block the event loop for about
half a second (the line editor's terminal probe waits up to 500 ms for an answer): the LED tick and the
button poll reported missed periods right after `console ready`. `console_start()` now only creates
the REPL task; the command registry, the line editor's set-up and the probe run in that task.

## The console

On the USB Serial/JTAG port (`idf.py -B build-esp32c6 -p /dev/ttyACM1 monitor`), prompt `hc>`. Every
command body runs on the event loop. `help` lists the commands grouped; `help <command>` shows one.
An installed device's console is out of reach, so every command that operates the device has a
route and a place in the web UI (issue #38, doc/web.md "Console parity"); the last column names it.
Console-only by design: `web password clear`, `crash` and `web demo`; also `wifi wps` and
`wifi clear`.

| Group | Command | Effect | Also via (spec/api.md, the web UI) |
|---|---|---|---|
| system | `status` | uptime, network, LED, time, heap (free, minimum, largest block), the boot record, the console output queue (lines dropped, of them while nobody read the port, bytes queued, USB host attached), every task with priority and the stack it never used, Improv | `GET /api/system/diagnostics`; the System page (also each task's state and CPU share, the loop's missed periods and longest turn) |
| | `uptime` | `up 0 d 01:02:03 (3723 s), boot 57, reset reason: software restart` | `GET /api/status` (`uptime_s`, `boot`) |
| | `settings list` | all stored settings, `wifi.psk` masked | `GET /api/settings` (the shown keys) |
| | `restart` | restarts | `POST /api/system/restart`; Settings, System |
| | `factory reset yes` | removes every blob of the state directory (the stored runtime image, persisted values, history), erases the Zigbee network (`zb_storage`, `zb_db`) and the `nvs` partition (settings with the WiFi credentials, the web password, the boot record), then restarts; without `yes` it only prints what it would do | `POST /api/system/factory-reset` with the device name; Settings, System |
| network | `wifi set <ssid> [<psk>]`, `wifi status` | doc/network.md | `PUT /api/system/wifi`, `GET /api/status` (`network`); Settings, System |
| | `wifi wps`, `wifi clear` | doc/network.md | console only (both take the device off its network; WPS needs the router's button anyway) |
| | `ntp set <server>` | doc/network.md | `PUT /api/settings/ntp.server` |
| time | `tz set <posix-tz>` | doc/time.md | `PUT /api/settings/tz` |
| update | `update status \| check \| now [<url>] \| url set <url> \| url clear` | doc/ota.md | `GET /api/status` (`update`), `POST /api/system/update/check`, `POST /api/system/update/now`, `PUT /api/settings/update.url`; Settings, System |
| diagnostics | `log level` | the core's threshold and the tags with their own | `GET /api/system/log-level` |
| | `log level <tag\|*> <error\|warn\|info\|debug>` | sets a tag's level at run time: an `hc.*` tag in `hc::Log` (also what reaches the ring and `/api/log`, at most 8 tags) and in the IDF log; other tags (`wifi`, `httpd`, ...) in the IDF log only (their debug lines exist only in a build with `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG`); `*` sets the default and drops every per-tag level; not stored, a restart returns to info | `PUT /api/system/log-level`; Settings, System |
| | `log tail [<n>]` | the newest n lines of the log ring (default 20): `seq ms level tag: message` | `GET /api/log`; the Log page |
| | `coredump status` | whether a dump is stored, its size, task, PC, panic reason, how to read it | `GET /api/status` (`boot.coredump*`), `GET /api/coredump`; Settings, System |
| | `coredump clear` | erases the stored dump | `DELETE /api/coredump`; Settings, System |
| | `crash yes` | stores to address 0: a panic, a core dump, a restart (tests the pipeline) | console only by design (a test needing the cable to read the result) |
| | `crash stall yes` | spins on the event loop without feeding the watchdog: a `task watchdog` reset after 10 s | console only by design |
| components | `web status` | the web server's state | `GET /api/status` (`web`) |
| | `web token create\|list\|revoke` | API tokens (doc/web.md) | `POST`/`GET /api/tokens`, `DELETE /api/tokens/<name>`; Settings, System |
| | `web session clear` | log out everywhere: ends every session, the remembered ones included (homecontrol #53) | `POST /api/auth/sessions/clear`; Settings, System |
| | `web password clear` | resets the device password, ends every session (the remembered ones too), onboarding opens again | console only by design (the recovery from a lost password needs physical access) |
| | `web demo` | the demo datapoints until the next restart | console only by design (a development aid) |
| | `storage status` | the mount, free space, the blob store's files | `GET /api/status` (`storage`); Settings, System |
| | `knx status`, `knx gateway set\|clear`, `knx nat on\|off` | doc/knx.md | `GET /api/status` (`knx`), `PUT /api/settings/knx.gateway`, `PUT /api/settings/knx.nat`; Settings |
| | `knx log`, `knx write`, `knx read` | doc/knx.md | the monitor stream (`GET /api/monitor`, WebSocket `monitor` with `send` and `read`; E-Home's group monitor) |
| | `zigbee status\|devices\|permit\|remove\|identify\|label\|interview` | doc/zigbee.md | `GET /api/zigbee`, `POST /api/zigbee/permit`, `/api/zigbee/devices/<ieee>`; the Zigbee page |
| | `image status\|clear` | doc/image.md | `GET /api/status` (`image`), `GET`/`DELETE /api/image`; Settings |
| | `image load <url>` | downloads an image and loads it | `PUT /api/image` with the image itself (the client downloads it) |
| | `external status\|fetch <id>` | doc/external.md | `GET /api/status` (`external`), `POST /api/external/<id>/fetch`; Settings, System |
| | `lang status\|set <code>\|sync` | doc/i18n.md | `GET /api/settings` (`i18n`), `PUT /api/settings/ui.lang`, `POST /api/lang/sync`; Settings |
| | `mqtt status\|publish-discovery\|set password\|clear password` | doc/mqtt.md | `GET /api/status` (`mqtt`), `POST /api/mqtt/discovery`, `PUT /api/settings/mqtt.password`; Settings |

Other components add commands through `ConsoleContext::extensions` (`hc::esp::ConsoleCommand`); they
appear under *components*.

## Console output

No task waits for the USB host (issue #46). Before, stdout went through the IDF's VFS of the USB
Serial/JTAG driver, which writes one character at a time and, with a host draining the port, waits
up to 50 ms per character whenever the driver's buffer is full: a burst of log lines on the event
loop cost hundreds of milliseconds, the loop missed its timers, warned about it (more output) and
ended in the task watchdog.

Now `app_main` first starts the console output path (`hc_platform/console_output.cpp`): it installs
the driver (transmit buffer 2 KB), creates the writer task `hc_out` (priority 3, below the serial
reader and the event loop; 2 KB stack) and points stdout and stderr at the device `/dev/hcout`.
Its `write()` appends to an `hc::OutputRing` of 4 KB under a short critical section (a copy of the
bytes with the CRLF translation: microseconds) and wakes the writer, which takes the queued bytes
and does the blocking driver writes, at most 256 bytes and 100 ms per call. Everything printed goes
this way: `hc::Log` through the ESP sink and the IDF components through `esp_log` (both print with
`vprintf` to stdout), the console's command output, prompt and line editor, and the Improv frames as
whole records. The bootloader and the IDF start-up before `app_main` print through the IDF's own
synchronous path, the panic handler through its own path past the queue. The log ring of the web UI
(`/api/log`, `log tail`) is filled by `hc::Log` before the console output and never loses a line to
the USB port.

When the queue is full a line is dropped whole (never its tail without its head) and counted; the
next output that finds room is preceded once by `… N line(s) dropped (console output could not keep
up)`, also when the queue merely ran empty. The counter is in `status` (`console`) and on the System
page (`GET /api/system/diagnostics`, `console`: `dropped`, `dropped_unread` of them while nobody read
the port, `queued`, `capacity`, `host`). Nobody reads when no USB host is attached (a charger instead
of a computer) or the host has not opened the port; the device tells this from the writer having
made no progress for a second (`usb_serial_jtag_is_connected()` covers only the first). Then the
queue fills and stays full, the first 6 KB since then wait for the next reader,
and every further line is dropped and counted; the event loop does not notice. One loop turn can
queue at most the 4 KB (the writer runs when the loop is idle): a console command printing more,
such as `log tail` over the whole log ring, ends in the marker line.

### Why the writer polls the port

The IDF driver's interrupt-driven transmit path is not used (issue #46, measured on the board): its
handler leaves the "IN empty" interrupt asserted when the FIFO is empty but not yet writable (the
host has not taken the previous packet), so the interrupt fires back to back until the host's next
poll; with a host that drains slowly (about one packet per 50 ms was measured with `cat` on Linux)
the storm takes the CPU from every task, the event loop misses its timers, and the task watchdog
ends it. The handler also answers every poll with an empty packet while nothing is queued and keeps
the interrupt enabled, which keeps the FIFO busy for anybody else. The writer task therefore keeps
that interrupt disabled and transmits by polling: when the FIFO is writable it places one USB packet
(64 bytes) and flushes it, after a full packet it sends the empty one that ends the transfer, and
otherwise sleeps a tick. Throughput is bound by the host's polling; a reading host keeps up with the
logs. The driver stays installed for the receive side (the Improv reader and the console). A
`cat /dev/ttyACM<n>` attached for two minutes leaves `loop.missed` at its idle rate and
`longest_turn_ms` below 100 ms; before, the board rebooted within half a minute.

## Task stacks

Every task's stack is its measured need plus a margin (issue #45): the size, the use measured on the
C6 (2026-11-03: IDF 5.5.6, 45 minutes up with E-Home over wss, the KNX tunnel up and one Zigbee
device; the size minus `stack_free` of `GET /api/system/diagnostics`, which is the high-water
mark), and the path that may go deeper than the measurement. The setting is next to each figure in
the code (`kLoopTaskStack`, `kStack`, `kTaskStack`, ...) or in `sdkconfig.defaults.app`.

**The high-water mark underestimates a printf.** The ROM's formatter reserves its frame at once
(`_svfprintf_r` 1072 B, `snprintf` 176 B) and mostly never writes it (its buffer for floats), so a
task that prints may need about 1.25 KB more than its high-water mark shows, and the stack guard
fires on the stack pointer alone. That was #48: the "lines dropped" marker of the console output was
formatted with `snprintf` inside `out_write()`, i.e. inside the logging task's own printf, and
overflowed `sys_evt` (2304 B then) and, with 1536 B, `hc_out`; the marker is now formatted by hand.
Every task that may print keeps at least 1.3 KB over its high-water mark.

| Task | Size (before) | Used | Not measured on the desk |
|---|---|---|---|
| `hc_loop` | 9216 (12288) | 3664; 5540 loading the largest fixture image (2026-10-18) | a rule set at the image limits, console commands |
| `httpd` | 4608 (6144) | 2052; 2692 with page loads (2026-10-18) | |
| `hc_ehome_ws` | 5120 (6144) | 2612 (TLS to the development CA) | the handshake with a public RSA chain through the certificate bundle |
| `hc_zigbee` | 4608 (6144) | 2236 | a join with the key exchange, an interview |
| `hc_udp` | 3072 (4096) | 784 (gateway by address) | `getaddrinfo` for a gateway by name |
| `hc_console` | 3584 (4096) | 1832 | editing a line with the history; its commands print |
| `hc_out` | 2048 (unchanged; 1536 overflowed, #48) | 304 | |
| `mdns` | 3584 (4096) | 1712; 2120 (2026-10-18) | a name conflict |
| `tiT` (lwIP) | 2560 (3072) | 688 | a log line's printf |
| `esp_timer` | 2560 (3584) | none measurable | a callback that logs |
| `Tmr Svc` | 2048 (unchanged) | 260 | a callback that logs |
| `sys_evt` | 3584 (unchanged, issue #48) | 1192 | |
| `wifi` | 6656 (the IDF's, with WPA3) | 2600 | WPS, a WPA3 handshake |
| `IDLE` | 1536 (the IDF's) | 284 | |

The short-lived workers (`hc_ota_fetch`, `hc_ext_fetch`, `hc_image_get`, the MQTT client's task, the
Improv reader `hc_serial`) keep their sizes: they did not run during the measurement. The `hc.ws`
line at every WebSocket open reports the E-Home client's reserve after the handshake; the line at
the end of every session the reserve of its sender task `hc_ehome_tx` (issue #55, 3072 B, 1572 B
used).

## Testing on the board

1. Flash the full image once (the partition table changed): `write_flash @flash_args`.
2. Boot: the first `hc.ctl` line names the reason (`USB reset` or `power-on` after flashing), the second
   the boot facts; `hc.diag` says nothing about core dumps (none stored); no `missed ... period(s)`
   warning from the event loop; `hc.diag: event loop under the task watchdog (10 s, panics and resets)`.
3. `crash yes`: a `Guru Meditation Error ... Store access fault` with the backtrace, the core dump being
   written, the restart. Next boot: `starting, reset reason: panic`, `hc.diag: core dump stored (... B,
   from this reset): task hc_loop, PC 0x...`; `coredump status` repeats it; `/api/coredump` and
   `esp-coredump info_corefile` decode it with `cmd_crash` in the backtrace; `coredump clear`, then
   `coredump status` says none is stored.
4. `crash stall yes`: after 10 s `Task watchdog got triggered ... hc_loop`, a core dump, the restart; next
   boot: `starting, reset reason: task watchdog`, the dump's task is `hc_loop`.
5. `/api/log` after each restart shows the boot lines with the reason.
