# Network bring-up

How the controller gets onto the network, finds the time and announces itself (issue #2). The
connection workflow is ported from the phoneblock dongle's proven WiFi workflow; its decisions live
in the portable core (`hc::Connectivity`, tested under a fake clock), the ESP platform only
executes them.

| Piece | Where |
|---|---|
| Connection workflow: when to connect, pair, retry, fall back | `core/include/hc/connectivity.h` |
| Backoff policy | `core/include/hc/backoff.h` |
| Status LED patterns | `core/include/hc/status_led.h` |
| Hold-to-pair button logic | `core/include/hc/hold_button.h` |
| Setting keys, defaults, validators, `store_wifi_credentials()` | `core/include/hc/network_settings.h` |
| POSIX TZ validation, local time formatting | `core/include/hc/tz.h` |
| Improv Wi-Fi serial: frames, reader, service and its lifetime | `core/include/hc/improv_protocol.h`, `core/include/hc/improv.h` |
| WiFi station + WPS driver, WiFi scan, mDNS, SNTP, button | `platform/esp/components/hc_net` |
| USB Serial/JTAG port shared by Improv and the console | `platform/esp/components/hc_improv` |
| Console | `platform/esp/components/hc_console` |
| Web flasher (ESP Web Tools manifest and page) | `web/flasher/` |
| NVS settings, status LED, event-loop queue | `platform/esp/components/hc_platform` |

## Provisioning

**From the browser: the web flasher.** `web/flasher/` flashes the firmware with ESP Web Tools and
then hands the device its WiFi credentials over the same USB cable (Improv Wi-Fi serial); see
"Improv / web flasher" below.

**WPS push-button.** A device without stored credentials listens for WPS all the
time, so the procedure is:

1. Power the board. The status LED blinks fast (100 ms on, 100 ms off): it is listening for WPS.
2. Press the WPS button on the router (on a FRITZ!Box: "WPS" or "Connect" / "Neues Gerät").
3. Within a few seconds the log shows `WPS: credentials received for <ssid>`, the LED changes to a
   slow blink (500 ms on, 500 ms off: connecting) and then to a short pulse (100 ms on, 900 ms off:
   online, no runtime image yet). The log shows `network up <ip>`, the mDNS name and later the
   local time.

The credentials are stored in the settings exactly as `wifi set` stores them (`wifi.ssid`,
`wifi.psk`); if several access points answer, the first credential set is taken.

**Re-pairing** a device that already has credentials (a new router): hold the BOOT button for about
3 s (a short press does nothing), or type `wifi wps` on the console, then press the router's WPS
button. The current connection stays up while the device waits for the router; only when the
router answers does the WPS exchange leave the old access point for a few seconds (a WiFi station
can be associated with one access point only). If nobody answers within the WPS walk time (about
2 minutes) or the exchange fails, the device keeps or restores its old connection.

**Fallback: the console.** On the USB Serial/JTAG port (`idf.py -p /dev/ttyACM1 monitor`), prompt
`hc>`:

| Command | Effect |
|---|---|
| `wifi set <ssid> [<psk>]` | stores the credentials (no key: open network) and connects; quote an SSID with spaces (`wifi set "My Net" secret123`) |
| `wifi wps` | starts WPS pairing (refused while pairing is active) |
| `wifi clear` | erases the credentials, disconnects; the device then listens for WPS |
| `wifi status` | SSID, link state and address, workflow mode, last reason, RSSI, failures, next step, mDNS name, NTP server |
| `status` | uptime, network, LED state, local time and whether it is synchronized, heap, the boot record, the tasks' stack reserve, whether Improv is still offered (the diagnostics commands: `doc/diagnostics.md`) |
| `tz set <posix-tz>` | stores and applies the time zone |
| `ntp set <server>` | stores the SNTP server, restarts SNTP if it runs |
| `settings list` | all stored settings, `wifi.psk` masked |
| `restart` | restarts the controller |

The key is never logged or printed; `wifi set` also removes its line from the console's command
history, so the up arrow cannot bring it back. The terminal shows the key while it is typed (the
line editor echoes), so type it only on the local USB console.

## Improv / web flasher

[Improv Wi-Fi serial](https://www.improv-wifi.com/serial/) lets a browser page hand a device its WiFi
credentials over the serial port it was just flashed through. The page is `web/flasher/index.html`
with [ESP Web Tools](https://esphome.github.io/esp-web-tools/) (Chrome or Edge on a desktop; Web
Serial needs HTTPS or `localhost`); `web/flasher/manifest.json` describes the esp32c6 build
(bootloader, partition table, the OTA updater, the application and the otadata that selects it; offsets
in `doc/ota.md`). "Connect, install, set up
WiFi" is one browser session: after the install ESP Web Tools resets the board, asks it over Improv
for its state and device info, offers the networks it scanned, and sends the chosen SSID and key.
The device stores them exactly as WPS and `wifi set` do (`store_wifi_credentials()`), restarts the
connection workflow and answers with `http://homecontrol-<mac>.local/` once the link is up; the
page then offers it as "visit device". Without a link after 25 s, or after 3 failed attempts (a
wrong key), it answers "unable to connect"; the credentials stay stored (a corrected retry
overwrites them, WPS and the failsafe remain).

The protocol is the phoneblock dongle's, ported: the RPCs `wifi settings`, `get current state`,
`get device info` (`HomeControl`, `<version> (<build>)`, the chip, `homecontrol-<mac>`) and `get
wifi networks` (one entry per SSID, strongest first, hidden networks left out; the IDF refuses to
scan while WPS owns the station, so the core pauses pairing for the scan,
`Connectivity::pause_pairing()`, and resumes it afterwards). A bad checksum or malformed RPC is
answered with the error `invalid RPC`, an unknown command with `unknown RPC`. The decisions live in
`hc::Improv` (core, tested under the fake clock); the ESP platform only moves bytes.

**Window and grace.** Improv is offered from boot for 5 minutes (`Improv::kWindowMs`) and ends
earlier 60 s (`Improv::kGraceMs`) after the network first comes up - but never sooner than 60 s
after the last frame from a client (so a user in the middle of the dialog is not cut off, and a
`wifi settings` exchange always completes). The log then says `Improv Wi-Fi serial ended (...): the
serial port belongs to the console` and the port carries only the console and the log. A provisioned
device that boots offers Improv for about a minute; opening the port from the browser resets the
board (doc/hardware.md), so a client always meets a fresh window. Nothing is sent before a client
has sent a frame: on the monitor Improv is invisible.

**Status LED.** While a `wifi settings` exchange waits for the link the LED shows *pairing* (fast
blink), also during the short pause of pairing for a scan.

**Sharing the port with the console.** The IDF REPL reads the port itself, so it cannot share it:
the console output path (`console_output.h` in `hc_platform`, started first thing in `app_main`)
installs the USB Serial/JTAG driver, and while Improv is active a reader task of `hc_improv` reads the
raw bytes and splits them with `hc::ImprovReader`. A frame starts with the magic `IMPROV` followed by
version 1; bytes matching a prefix of the magic are held back (at most 100 ms) and released to the
console as soon as the match breaks, so typing `IMPORT` or `IMPROVE` on the console works. Frames go
to the event loop; everything else reaches the console's line editor through the VFS device
`/dev/hcin`, the REPL task's stdin. When Improv ends the reader task exits and the console reads the
driver directly. Frames are queued as whole records on the console output path, framed by
newlines; log lines go between them, never inside one (ESP Web Tools skips the noise). The output
path never blocks the sender (doc/diagnostics.md "Console output").

**Testing it locally** (Chrome on the desk machine, the C6 connected):

```bash
cd platform/esp && idf.py -B build-esp32c6 build && (cd updater && idf.py -B build-esp32c6 build) && cd ../..
scripts/flasher-local.sh                  # copies the five parts to web/flasher/firmware/esp32c6/
cd web/flasher && python3 -m http.server 8000
```

Open `http://localhost:8000/`, click *Connect*, choose the `USB JTAG/serial debug unit` of the C6
(never the S3). The copied binaries are not versioned (`.gitignore`). For a release the binaries
come from the GitHub release assets (the URL pattern is noted in the manifest, to be filled with the
first release). The partition layout is the asymmetric OTA layout of `doc/ota.md`.

## The connection workflow

`hc::Connectivity` has a link state (`down`, `connecting`, `up <ip>`) and a mode:

| Mode | Entered when | Does |
|---|---|---|
| `normal` | credentials stored (start, `wifi set`, pairing success, failsafe over) | connects; after a failure or loss reconnects with the backoff |
| `unprovisioned` | no credentials (start, `wifi clear`, connect refused) | pairs; restarts pairing 1 s after every failure or timeout, forever |
| `re-pairing` | button held 3 s, `wifi wps`, with credentials | pairs while the link stays up; failure: back to `normal` (reconnect if the link was lost) |
| `failsafe (pairing)` | 7 consecutive failures in `normal` | pairs for 120 s (restarting within the phase after a failure) |
| `failsafe (stored credentials)` | the pairing phase is over | retries the stored credentials with the backoff for 120 s, then back to pairing |

The failsafe solves the "router replaced" case without a console while a transient outage still
heals by itself: the credentials are never erased, the link coming up or a pairing success ends
it. Seven failures with the backoff take at least 1+2+4+8+16+32 s of waiting plus the attempts
(about 75 s or more), which matches the dongle's threshold of about 75 s (30 immediate retries).

Rules that keep the driver and the radio from racing each other (the dongle's lessons):

- Every switch between connecting and pairing first stops the other activity (disconnect, or
  stop WPS) and waits 500 ms before the next driver call.
- Down reports while pairing, or while a next step is scheduled, are not failures: they come from
  the WPS exchange or from an activity the core stopped.
- Duplicate pairing failures (the IDF reports `FAILED` and `TIMEOUT` back to back) collapse into
  one restart.
- A WiFi scan (Improv) pauses the workflow: pairing or an attempt in flight is stopped, every step
  that comes due is held, and the latest one runs 500 ms after the scan (at most 20 s of pause).
- An attempt without an outcome after 30 s counts as a failure; a pairing without an outcome after
  150 s counts as a pairing failure (the IDF's own WPS timeout is 120 s).
- The driver never reports from within a call the core made; on the ESP the IDF events reach the
  core through the event-loop queue.

In the ESP driver the station scans all channels, so a hidden SSID is found too. WiFi power save
is `min` by default (modem sleep between DTIM beacons, the mode Espressif's coexistence examples
use): the 802.15.4 radio gets the air while WiFi sleeps, and with power save off a Zigbee battery
device lost its parent within seconds of joining (doc/zigbee.md, the measurements). The setting
`wifi.ps` (`none` | `min` | `max`, console `wifi ps`, the Settings page, `PUT /api/settings/wifi.ps`)
changes it on the running link; `none` is the phoneblock dongle's choice (mains powered; modem sleep
loses ACKs that some access points punish with a disassociation, reason 34) and the right one for a
household without Zigbee devices that wants the lowest latency.

**Backoff:** 1 s, 2 s, 4 s, 8 s, 16 s, 32 s, 60 s, 60 s, ... after consecutive failures; reset
when the link comes up and when the workflow changes mode.

Every transition is logged under the tag `hc.net`: `network connecting (attempt 3)`,
`network up 192.0.2.10`, `network down: wifi reason 201 (NO_AP_FOUND); reconnect in 4 s
(failure 3)`, `network mode normal -> failsafe (pairing) (...)`, `pairing failed: ...; listening
again in 1 s`. The heartbeat line names the link state and, if it is not `normal`, the mode.

## Status LED

| Pattern | Meaning |
|---|---|
| fast blink, 100 ms on / 100 ms off | pairing (WPS): press the router's WPS button; or an Improv WiFi setup is connecting |
| slow blink, 500 ms on / 500 ms off | no IP address yet, or reconnecting |
| short pulse, 100 ms on / 900 ms off | online but no runtime image loaded yet; with an image the LED shows `ready`, or `degraded` while the KNX tunnel is down under a KNX image |
| short dropout, 900 ms on / 100 ms off | degraded: configured, but something fails (reserved for later packages) |
| solid | ready |

The core derives the pattern (`Controller::led_status()`, `hc::StatusLed`, 50 ms ticks) and switches
the light; the board's LED is chosen in menuconfig under *HomeControl status LED*: a WS2812 pixel
on GPIO8 (default on the ESP32-C6 devkits, dim cyan), a plain GPIO LED (optionally active low), or
none (default for the S3 build).

## Transmit power

`CONFIG_HC_WIFI_MAX_TX_POWER` (menuconfig *HomeControl network*, quarter dBm, 8..84) limits the
station's transmit power when it starts; the limit is logged (`WiFi transmit power limit …`). The
project default is 11 dBm (44) because the development board browns out at the driver's maximum
of 20 dBm on its USB supply (`doc/hardware.md`). Lower power means shorter range: raise it for a
board with a proper supply.

## mDNS

When the network first comes up the controller announces itself:

- host name `homecontrol-<last three bytes of the WiFi MAC in lower-case hex>`, for example
  `homecontrol-a1b2c3.local` (also sent as the DHCP host name);
- service `_homecontrol._tcp` on port 80 (the web server and API, `doc/web.md`), instance name =
  the host name (unique per device, so E-Home's controller search lists every controller; #32), TXT
  records `version=<hc::kVersion>` and `build=<git describe of the build>`;
- service `_http._tcp` on port 80 with the TXT record `path=/` (added 2026-10-18 at the author's
  request): the standard announcement of a web interface, which routers such as the FRITZ!Box turn
  into a link to the device in their home-network list, as they do for the phoneblock dongle.

Check from the desk machine: `avahi-browse -r _homecontrol._tcp`. E-Home's search sends a *legacy
unicast* query (from a port other than 5353) for the PTR of `_homecontrol._tcp.local`; the ESP-IDF
responder answers it unicast to the asking port with the SRV, TXT and A records as additionals, so
one exchange yields host, port, version and address (verified against the board with a hand-made
query, 2026-10-18). The POSIX build does not announce (it logs so at start).

## Time: SNTP and the time zone

SNTP (`esp_netif_sntp`) starts when the network first comes up, against the setting `ntp.server`
(default `pool.ntp.org`). Its first delivery marks the ESP clock synchronized; the controller
notices within a second and logs `wall clock synchronized: 2026-10-17 10:00:00 CEST (UTC+02:00)`
once. `status` shows the local time and whether it is synchronized.

The time zone is the setting `tz`, a POSIX TZ string, default `CET-1CEST,M3.5.0,M10.5.0/3` (central
European time with the EU rules). The core validates it (a zone with daylight saving time needs
both transition rules: the C libraries silently fall back to US rules otherwise) and the platform
applies it with `setenv("TZ")` and `tzset()`. An invalid stored value is logged and replaced by
the default. The POSIX build takes the host's synchronized clock and applies `tz` too.

## Settings

| Key | Type | Default | Set by |
|---|---|---|---|
| `wifi.ssid` | string | none (device pairs) | WPS, `wifi set`, erased by `wifi clear` |
| `wifi.psk` | string | none (open network) | WPS, `wifi set`, erased by `wifi clear` |
| `tz` | string | `CET-1CEST,M3.5.0,M10.5.0/3` | `tz set` |
| `ntp.server` | string | `pool.ntp.org` | `ntp set` |

Keys have 1 to 15 characters of `A-Z a-z 0-9 . _ -`; string values at most 127 bytes without
control characters; integers are 32 bit; every violation is refused with a reason, nothing is
truncated. These are persisted formats: never rename a key or change its type.

- **ESP**: NVS namespace `hc` of the default partition; strings as NVS strings, integers as `i32`,
  booleans as `u8`. The IDF WiFi driver keeps no copy (`WIFI_STORAGE_RAM`): the settings are the
  only store. NVS is not encrypted: whoever reads the flash reads the WiFi key.
- **POSIX**: a text file, `homecontrol.settings` in the working directory or `--settings <file>`,
  written atomically (temporary file and rename) with mode 0600. Format 1, one entry per line:

  ```
  # comment
  wifi.ssid=s:Example Net
  count=i:-42
  flag=b:1
  ```

  The key ends at the first `=`, the type letter (`s`, `i`, `b`) and `:` follow, a string is the
  rest of the line verbatim. Unparsable lines are logged at load and dropped at the next write.

## POSIX

The host build's `NetworkDriver` reports the link up as soon as the core asks, with the address of
the first running non-loopback IPv4 interface (127.0.0.1 if there is none, logged). It needs no
credentials and has no pairing (a pairing request is refused with a log line). mDNS is not
implemented on POSIX.
