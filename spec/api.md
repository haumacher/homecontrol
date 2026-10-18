# The device API (REST and WebSocket)

The controller's web server (doc/web.md) offers a JSON API under `/api/` and a live feed at `/ws`.
The device's own UI (`web/ui/dist/`) uses nothing else; machines (scripts, E-Home, a home server)
use the same endpoints with a bearer token. The core implements both (`hc::WebApi`,
`hc::LiveFeed`); the ESP and POSIX servers only carry HTTP. This document is the contract; a change
that breaks a client needs a new path or field, not a changed meaning.

## Conventions

- Bodies are UTF-8 JSON objects (`Content-Type: application/json`), at most 1024 bytes in a
  request (413 `api.body_too_large` otherwise), except `PUT /api/image` (256 KB). Responses carry `Cache-Control: no-store`.
- Every refusal is a 4xx/5xx status with the body `{"error": "<English reason>", "code": "<key>"}`.
  The `code` is stable and is the key a UI translates (doc/web.md, "Texts"); the `error` text is
  for logs and humans and may change. Some refusals add fields (listed below).
- Times: `*_ms` are milliseconds of the controller's monotonic clock (since boot; compare with
  `now_ms` of the same response), `*_s` are seconds, `unix` is seconds since the epoch.
- The ESP device serves everything on port 80. The POSIX build serves HTTP on `--http <port>`
  (default 8080) and the WebSocket on the port above it; `GET /api/auth/session` says which
  (`ws_port`, 0 = the page's port).
- What a platform cannot do (restart, update itself, keep a core dump, run a Zigbee coordinator)
  is refused with 501 `system.unavailable` and the reason, never with a silent success.

## Authentication

A device without a password is **not onboarded**: it answers only `GET /api/auth/session` and
`POST /api/auth/onboard`; everything else is refused with 403 `auth.onboarding_required`.

Once onboarded, every endpoint except the four under `/api/auth/` needs either

- the **session cookie** `hc_session` (set by onboarding and login: `HttpOnly; SameSite=Strict;
  Path=/`, no `Max-Age`, i.e. a browser-session cookie; the device forgets all sessions at reboot
  and after 24 h without use), or
- a **bearer token**: `Authorization: Bearer hct_<32 hex digits>`, created with
  `POST /api/tokens` (see "System") or on the serial console with `web token create <name>` (shown
  once; doc/web.md).

Without either: 401 `auth.required`; with an unknown or revoked token: 401 `auth.token_invalid`.

**CSRF**: a POST, PUT or DELETE authenticated by the cookie must carry the header
`X-Requested-With: HomeControl` (403 `auth.csrf` otherwise). A cross-site form cannot set it, and a
cross-site script cannot send it without a CORS preflight, which the device never grants. Requests
with a bearer token need no such header.

### `GET /api/auth/session` (open)

```json
{"onboarded": true, "authenticated": true, "via": "session", "device": "homecontrol-a1b2c3",
 "version": "0.1.0-dev", "build": "g1a2b3c4", "ws_port": 0, "min_password": 8, "lang": "de"}
```

`build` is the build id (`git describe`) of the running firmware; with `version` it tells a page that
waits for a restart (a firmware upload) which firmware came back.

`via` is `"session"`, `"token"` or `null`. `lang` is the UI language `ui.lang` (doc/i18n.md), open
like the rest of this answer so that the login page speaks it too.

### `POST /api/auth/onboard` (open, only while not onboarded)

Body `{"password": "<8 to 64 characters>"}`. Stores the password (salted PBKDF2-HMAC-SHA256) and
logs the caller in: 200 `{"ok": true}` with `Set-Cookie`. Refusals: 400 `auth.password_missing`,
400 `auth.password_invalid` (length), 409 `auth.password_set` (onboarded already; the password is
reset only on the console), 500 `auth.storage_error`, 500 `auth.no_random`.

### `POST /api/auth/login` (open)

Body `{"password": "..."}`. 200 `{"ok": true}` with `Set-Cookie`. A wrong password: 401
`auth.wrong_password` with `failures` (consecutive failures from this address) and
`free_failures` (5). From the sixth failure on, the address is blocked for 30 s, doubling with each
further failure up to 15 min: 429 `auth.rate_limited` with `retry_after_s` and a `Retry-After`
header; a block refuses even the right password. Also 409 `auth.onboarding_required`, 409
`auth.password_changed` (the password was reset while the hash was computed), 400 as for
onboarding.

### `POST /api/auth/logout` (open)

Ends the session of the cookie (if any) and clears the cookie: 200 `{"ok": true}`. The session's
WebSocket connections are closed with 1008 `auth.session_ended`.

## Status

### `GET /api/status`

```json
{"device": {"name": "homecontrol-a1b2c3", "version": "0.1.0-dev", "build": "...", "ui": "3c0dcd52e390bb91"},
 "uptime_s": 5321,
 "network": {"state": "up", "ip": "192.0.2.10", "mode": "normal", "pairing": false, "mdns": "homecontrol-a1b2c3.local",
             "mdns_announced": true},
 "time": {"synchronized": true, "local": "2026-10-17 14:03:12 CEST (UTC+02:00)", "unix": 1792231392,
          "tz": "CET-1CEST,M3.5.0,M10.5.0/3", "ntp_server": "pool.ntp.org"},
 "memory": {"free": 201344, "min_free": 187220},
 "improv": {"active": false, "state": "...", "ends_in_s": 0},
 "update": {"available": true, "partition": "ota_0", "confirmed": true, "checking": false, "source": "",
            "last": "none since start"},
 "led": "setup",
 "web": {"sessions": 1, "tokens": 0, "ws_clients": 1},
 "datapoints": 8,
 "image": {"loaded": true, "project": "example-house", "revision": 7, "name": "Example house", "source": "api",
           "datapoints": 17, "rules": 2, "schedules": 2, "degraded": false}}
```

`image` (builds with the runtime image service): without an image `{"loaded": false}`, after a
refused load while no image runs also `"refused": "<code>"`. `source` is how the image came: `boot` (the stored
one at start), `api`, `console`, `file` (POSIX `--image`), `restore` (the stored one again after a
failed apply). `degraded`: the image has KNX datapoints and the tunnel is down (the LED shows
degraded). The WebSocket's `status` message carries the same member.

Members only `GET /api/status` carries (not the WebSocket's `status` message, which must fit one
1400-byte frame):

- `storage`: `{"total", "used", "free"}` (bytes of the storage partition; ESP only) and `{"files",
  "bytes"}` (the blob store's files and their size).
- `knx`: `{"state": "down|connecting|up", "reason", "gateway": "<host:port or \"\">", "nat",
  "channel", "address" (both only while up), "queued", "telegrams"}`.
- `external`: `{"sources": [{"id", "preset", "interval_s", "fetches", "failures", "fetching",
  "last_ok_ago_s" (once one succeeded), "error", "next_s"}]}`.
- `lang`: the language pack as `i18n` of `GET /api/settings` (`lang`, `state`, `detail`,
  `release`, `offered`).
- `mqtt`: `{"state", "configured"}`, with a broker also `broker`, `base_topic`, `discovery`,
  `functions`, `published`, `commands`, `refusals`, `connects`, `failures`, `error` and, while
  waiting, `next_s`.
- `zigbee`: `{"up", "permit_join_s", "devices", "generation"}` (with a coordinator).

Each appears when the build has the service.

`update` is platform-specific: always `available` (whether the device updates itself), the ESP
build adds `partition`, `confirmed`, `checking`, `source` (update.url, `""` if none), `last`,
`last_ago_s` and, with a source, `next_check_s` (doc/ota.md).

## Datapoints

A datapoint (doc/datapoints.md) in the API:

```json
{"id": 0, "name": "Living room light", "type": "boolean", "flags": 3, "writable": true,
 "technology": "knx", "value": true, "quality": "good", "updated_ms": 51230}
```

`type` is the value type's name (`boolean`, `unsigned`, `signed`, `real`, `enumeration`,
`string`, `raw`, `time of day`, `date`, `date-time`, `controlled`, `color`; doc/datapoints.md);
`flags` are the datapoint's flag bits; `technology` the name of the technology that owns it
(`knx`, `zigbee`, `external`, `scheduler`, `rules` for a rule parameter's datapoint, ...; `null`
for an application value); `quality` is `unknown` (no value yet), `good` or `stale`.
Values in JSON:

| type | JSON |
| --- | --- |
| boolean | `true` / `false` |
| unsigned, signed, real, enumeration | a number (converted to the datapoint's type; out of range is refused) |
| string | a string |
| raw | a string of hex digits |
| time of day | `{"day": 0-7, "hour", "minute", "second"}` |
| date | `{"year", "month", "day"}` |
| date-time | `{"year", "month", "day", "hour", "minute", "second", "day_of_week", "flags"}` |
| controlled | `{"control": bool, "value": number}` |
| color | `{"red", "green", "blue"}`, optionally `"white"` and `"valid"` |
| (no value) | `null` |

### `GET /api/datapoints[?from=<id>]`

A page of at most 50 datapoints from id `from` (default 0):
`{"now_ms": ..., "count": <all>, "from": 0, "datapoints": [...], "next": 50}`; `next` is `null`
on the last page.

### `GET /api/datapoints/<id>`

One datapoint, 404 `dp.unknown_id`.

### `POST /api/datapoints/<id>/command`

Body `{"value": <JSON value>}`. Hands the value to the datapoint's technology: 200
`{"ok": true, "datapoint": {...}}`. Refusals: 400 `dp.value_missing`, 404 `dp.unknown_id`, 409
`dp.not_writable`, 422 `dp.wrong_type` (with the reason), 409 `dp.refused` (the technology
refused; the log says why), 503 `dp.busy`. A command to a rule parameter's datapoint
(`rule/<ruleId>/<parameterId>`, spec/rules.md 8.6) sets the parameter as
`PUT /api/rules/<rule>/parameters/<parameter>` does; a value the parameter refuses (range, choices)
is 409 `dp.refused`, a parameter that is not adjustable 409 `dp.not_writable`. A duration
parameter's datapoint takes milliseconds.

## Log

### `GET /api/log[?after=<seq>]`

The lines of the controller's log ring (about 4 KB of the newest lines, kept in RAM) with a
sequence number above `after`:

```json
{"now_ms": 90211, "first": 118, "last_seq": 164, "dropped": 0,
 "lines": [{"seq": 119, "ms": 80211, "level": "I", "tag": "hc.web", "message": "login from 192.0.2.20"}],
 "more": false, "next_after": 164}
```

Poll with `after=<next_after>`; `more` says that the response was cut and another request follows
at once. `first` above `after + 1` means lines were overwritten in between. `dropped` counts lines
that could not be stored (written from two threads at once). 503 `log.busy`: try again.

## Settings

### `GET /api/settings`

```json
{"settings": [{"key": "tz", "value": "CET-1CEST,M3.5.0,M10.5.0/3", "default": "CET-1CEST,M3.5.0,M10.5.0/3", "editable": true},
              {"key": "ntp.server", "value": null, "default": "pool.ntp.org", "editable": true},
              {"key": "update.url", "value": null, "default": "", "editable": true},
              {"key": "ui.lang", "value": "de", "default": "en", "editable": true},
              {"key": "wifi.ps", "value": "min", "default": "min", "editable": true},
              {"key": "mqtt.user", "value": null, "default": "", "editable": true},
              {"key": "mqtt.password", "value": null, "default": "", "editable": true, "secret": true, "stored": true},
              {"key": "knx.gateway", "value": "192.0.2.20", "default": "", "editable": true},
              {"key": "knx.nat", "value": "off", "default": "off", "editable": true},
              {"key": "wifi.ssid", "value": "...", "default": "", "editable": false}],
 "i18n": {"lang": "de", "state": "installed", "detail": "pack 'de' of release 0.2.0 installed (5035 bytes)",
          "release": "0.2.0", "offered": [{"code": "de", "name": "Deutsch"}]}}
```

`value` is `null` when the setting is not stored (the default applies); `ui.lang` and `tz` always
show the value in use. Secrets (`wifi.psk`, the password and token hashes `web.*`) never appear;
the write-only `mqtt.password` (doc/mqtt.md) is listed with `secret: true` and `stored` saying
whether one is kept, its `value` always `null`.
`i18n` (only on a device that downloads language packs, i.e. not the POSIX build) is the state of
the language pack of `ui.lang` (doc/i18n.md): `state` one of `builtin` (English, nothing to
download), `waiting` (a condition is missing, `detail` says which), `fetching`, `installed`,
`not_offered` (the release has no pack for the language), `failed` (`detail` gives the reason; a
retry follows); `offered` the languages the release's manifest lists, by their own names.

### `PUT /api/settings/<key>`

Body `{"value": "<string, at most 127 bytes>"}`; `""` erases `update.url`. Validated as on the
console (`tz` a POSIX TZ string, `ntp.server` a host name, `update.url` an http(s) URL; doc/ota.md;
`ui.lang` a language tag `[a-z]{2}(-[A-Z]{2})?`, doc/i18n.md; `wifi.ps` one of `none`, `min`, `max`,
doc/network.md; `mqtt.user` and `mqtt.password` 1 to 63 bytes, `""` removes them; `knx.gateway` a host or IPv4
address with an optional `:port`, `""` removes it; `knx.nat` `on` or `off`, doc/knx.md) and applied
at once (a new `knx.gateway` or `knx.nat` reconnects the KNX tunnel, a new `ui.lang` starts the download of its language pack, a new `wifi.ps` is set on the
running WiFi link, a new `mqtt.user` or `mqtt.password` reconnects the MQTT client). 200 with the settings list as for GET. Refusals: 404 `settings.unknown`, 403
`settings.not_editable`, 400 `settings.value_missing`, 422 `settings.invalid` (with the reason),
500 `settings.storage_error`.

## System

What the serial console does for an installed device (issue #38). The core implements every route
(`hc::WebApi`, `core/src/web_api_system.cpp`); what only a platform can do goes through the hooks
of `hc::WebHost` (`core/include/hc/platform/web_host.h`). A platform that cannot do something
answers 501 `system.unavailable` with the reason (the POSIX build has no OTA updater and no core
dump). Every refusal is also logged (`<method> <path> from <peer> refused (<code>): <reason>`).

### `POST /api/system/restart`

No body. 200 `{"ok": true, "action": "restart", "in_ms": 750}`: the restart follows after `in_ms`,
so the answer leaves first. Refusals: 501 `system.unavailable`, 409 `system.busy` (a restart, reset
or WiFi change is already pending).

### `POST /api/system/factory-reset`

Body `{"confirm": "<device name>"}` (the `device.name` of the status). Erases what the console's
`factory reset yes` erases (doc/diagnostics.md: every blob, i.e. the runtime image, last values and
history; the settings with the WiFi credentials, the password and the tokens; on the ESP also the
Zigbee network and the boot record), then restarts:
200 `{"ok": true, "action": "factory-reset", "in_ms": 750}`. Refusals: 400
`system.confirm_missing` (no body or no `confirm`), 422 `system.confirm_mismatch` (nothing was
erased), 501 `system.unavailable`, 409 `system.busy`.

### `POST /api/system/update/check` and `POST /api/system/update/now`

`check` asks the update server (update.url) whether a newer release exists and installs it as the
periodic check would; `now` installs the newest release regardless of the version (the console's
`update check` and `update now`, doc/ota.md). `now` takes an optional body `{"url": "<http(s) URL
of a firmware image>"}`. 200 `{"ok": true, "force": <bool>}`: the check runs in the background, its
outcome shows in the log and in the status's `update` member. Refusals: 400 `update.bad_request`
(a `url` with `check`), 422 `update.invalid` (not an http(s) URL), 501 `system.unavailable`, 409
`update.refused` (no update source, a check or an update already running, the network down, the
application not running from `ota_0`; with the reason).

### `POST /api/system/update/upload`

Starts a firmware upload from the browser (doc/ota.md "Upload from the browser"): no body. The device
generates a one-time token (128 random bits as 32 hex digits), leaves it with the instruction for the
OTA updater, makes the updater the boot partition and restarts after the answer:

```json
{"ok": true, "url": "http://192.0.2.10/", "token": "<32 hex digits>", "restart_in_ms": 750, "timeout_s": 600}
```

`url` is the device's current address: the updater serves there (port 80) for `timeout_s` and then
boots the previous application again, unless the application partition holds no bootable image (then
it waits without a limit). The token is shown only in this answer and never logged. Refusals: 501
`system.unavailable` (no OTA updater, the POSIX build), 409 `system.busy` (a restart, reset or WiFi
change is pending, also a second upload request within `restart_in_ms`), 409 `update.refused` (with
the reason: the application does not run from `ota_0`, an update check or an update is under way,
the updater in the device predates uploads - it is updated over USB only -, the device has no
address), 500 `auth.no_random`.

**The updater's own routes** (not under `/api/`, served by the updater while it waits; E-Home may
drive them the same way as the browser):

- `GET /status`: `{"updater": "<version>", "state": "awaiting_upload"|"writing"|"verifying"|"done"|"failed",
  "received": <bytes>, "expected": <bytes>, "remaining_s": <n>|null, "error": "<reason>"}`.
- `PUT /firmware` with `Authorization: Bearer <token>` and the raw `homecontrol.bin` as the body
  (with a `Content-Length`): 200 `{"ok": true, "version": "<version>"}`, after which the device
  restarts into the new image. Refusals `{"error", "code"}`: 401 `updater.unauthorized`, 409
  `updater.busy`, 411 `updater.length_required`, 413 `updater.too_large`, 422 `updater.wrong_image`
  (not the application for this chip; the flash is untouched), 422 `updater.verify_failed`, 400
  `updater.connection`, 500 `updater.flash`.
- `POST /abort` (no token): boots the previous application. 200 `{"ok": true}`; 409
  `updater.no_application`, `updater.busy`.
- `GET /`: a small page that finishes an upload after a reload of the browser tab.

### `GET` and `PUT /api/system/log-level`

```json
{"default": "info", "tags": [{"tag": "hc.knx", "level": "debug"}], "max_tags": 8, "platform_tags": true}
```

`PUT` body `{"tag": "<tag>", "level": "error|warn|info|debug"}` (the console's `log level`): `*`
sets the default and clears every tag level, an `hc.*` tag sets the core's level for it (at most
`max_tags`), any other tag goes to the platform's log (`platform_tags`: the ESP-IDF components, e.g.
`wifi`); the ESP build also hands `*` and `hc.*` to the IDF. Answers with the list as for GET; not
persisted (a restart restores `info`). Refusals: 422 `log.invalid` (unknown level; a tag that is
empty, longer than 23 characters or contains a space or `*`; all 8 tag slots taken), 422
`log.platform_tag` (a non-`hc.*` tag on a platform without its own log).

### `PUT /api/system/wifi`

Body `{"ssid": "<1 to 32 bytes>", "psk": "<8 to 63 characters, or \"\" for an open network>"}`.
Validated and stored as the console's `wifi set`; the link reconnects after the answer:
200 `{"ok": true, "ssid": "...", "open": false, "reconnect_in_ms": 750, "mdns": "homecontrol-a1b2c3.local"}`.
The device keeps its name, so the UI is found again at `mdns` once the new network is joined;
wrong credentials take it off the network (the console or a factory reset bring it back), so the
UI warns before sending. The psk is never logged or echoed. Refusals: 422 `wifi.invalid` (with the
reason), 409 `system.busy`, 500 `settings.storage_error`.

### `GET /api/system/diagnostics`

The System page (issue #42, `core/src/web_api_diagnostics.cpp`): everything the console's `status`
prints and what else helps with a complaint, read on each request (the page polls every 5 s; the
WebSocket's `status` message stays within its 1400 bytes and carries none of it). Session or token.
Each section a platform cannot fill says `"available": false` with `"reason"`; a figure the platform
does not know is left out.

```json
{"uptime_s": 3725, "now_ms": 3725012,
 "device": {"name", "version", "build", "ui"}, "boot": {...}, "update": {...},
 "chip": {"available": true, "model": "ESP32-C6", "revision": "v0.1", "cores": 1, "cpu_mhz": 160,
          "flash_size": 4194304, "flash_mode": "dio", "flash_freq": "80m", "idf": "v5.3", "ota_state": "valid"},
 "led": "ready", "time": {"synchronized": true, "local": "..."}, "improv": {"active": false},
 "memory": {"available": true, "free": 181240, "min_free": 150112, "largest": 90100, "total": 401000},
 "temperature": {"available": true, "celsius": 41.2, "accuracy": 2, "source": "die sensor"},
 "network": {"state": "up", "ip": "192.0.2.10", "mode": "normal", "pairing": false, "reason": "link up",
             "failures": 0, "link_ups": 3, "reconnects": 2, "up_for_s": 600},
 "wifi": {"available": true, "ssid": "example", "channel": 6, "rssi": -61, "tx_power_dbm": 15,
          "power_save": "min", "mac": "aa:bb:cc:dd:ee:ff", "gateway": "192.0.2.1", "netmask": "255.255.255.0",
          "dns": "192.0.2.1"},
 "storage": {"available": true, "total": 1048576, "used": 65536, "free": 983040, "files": 4, "bytes": 12000,
             "nvs": {"available": true, "used": 80, "free": 420, "usable": 294, "total": 500, "namespaces": 6}},
 "knx": {"available": true, "state": "up", "reason": "", "connections": 2, "reconnects": 1, "attempts": 3,
         "queued": 0, "channel": 7, "telegrams": 1234},
 "zigbee": {"available": true, "up": true, "channel": 15, "pan_id": 6754, "devices": 3},
 "tasks": {"list": [{"name": "hc_loop", "state": "running", "priority": 5, "stack_free": 3412,
                     "cpu_permille": 21, "current": true}, ...],
           "available": true, "count": 21, "cpu": true},
 "loop": {"missed": 3, "longest_turn_ms": 812, "longest_turn_at_ms": 20000, "longest_turn_ago_s": 3705,
          "timers": 17, "timer_capacity": 32, "stack_free": 3412}}
```

- `device`, `boot`, `update`, `led`, `time`, `improv`: as in `GET /api/status`.
- `chip`: the platform's facts. The device: `model`, `revision`, `cores`, `cpu_mhz`, `flash_size`
  (bytes), `flash_mode`, `flash_freq`, `idf`, `ota_state` (the running partition's: `new`, `pending
  verify`, `valid`, `invalid`, `aborted`, `undefined`). POSIX: `model` (the CPU), `cores`, `arch`, `os`.
- `memory`: the heap in bytes (`largest`: the largest block it can hand out; `total`: its size).
  POSIX has no heap budget (`"available": false`) and reports the process's `resident` memory.
- `temperature`: one reading per request. The device reads the die sensor (driver
  `temperature_sensor`, range 20..100 °C, `accuracy` ±2 °C as the IDF documents it); POSIX reads
  `/sys/class/thermal/thermal_zone0/temp` if present (`accuracy` left out: unknown).
- `network`: the core's view (doc/network.md): `failures` since the link was last up, `link_ups`
  since boot, `reconnects` (`link_ups` − 1), `up_for_s` while up. `wifi`: the driver's link facts
  (`rssi` in dBm while associated, `tx_power_dbm` the transmit power limit, `power_save` as the
  setting `wifi.ps` names it); POSIX has no radio.
- `storage`: the blob store's partition and files as in `GET /api/status`, and `nvs`: the entries of
  the `nvs` partition (`usable`: free entries outside the page NVS keeps in reserve), `namespaces`.
- `knx`: the tunnel's state, `connections` since start (`reconnects` one less), `attempts`, `queued`,
  `channel` while up, `telegrams` seen. `zigbee`: the network (`channel`, `pan_id` while up) and the
  device count; without a coordinator the reason.
- `tasks`: every task (on POSIX every thread of the process) with `state` (`running`, `ready`,
  `blocked`, `suspended`, `deleted`, `unknown`), `priority`, `stack_free` (bytes never used: the
  high-water mark), `cpu_permille` (its share of all cores' time since boot, FreeRTOS run-time
  statistics; `cpu` false when the build measures none), `core` (when pinned) and `current` (the
  calling task: the event loop's). The list stops before the response buffer is full; `omitted`
  then counts the tasks left out (`count` is all of them).
- `loop`: the event loop since boot (doc/core.md): `missed` timer periods (each also a warning in the
  log), the longest turn (`longest_turn_ms`, its start `longest_turn_at_ms` on the monotonic clock,
  `longest_turn_ago_s`; a turn is a `run_once` or a batch of messages other tasks posted, such as a
  web request), the timers in use, `stack_free` of the loop's task.

Refusals: 405 for another method.

### `DELETE /api/coredump`

Erases the stored core dump (the console's `coredump clear`). 200 `{"ok": true}`. Refusals: 404
`coredump.none`, 501 `system.unavailable`, 500 `coredump.clear_failed`.

### `GET /api/tokens`, `POST /api/tokens`, `DELETE /api/tokens/<name>`

`GET` lists the bearer tokens by name (the secret is never shown again):
`{"tokens": [{"name": "ehome"}], "max": 8}`. `POST` body `{"name": "<1 to 24 characters [A-Za-z0-9._-]>"}`
creates one: 200 `{"ok": true, "name": "ehome", "token": "hct_..."}`, the only time the token is
shown. `DELETE` revokes one: 200 `{"ok": true}`; a WebSocket authenticated by it is closed within a
second. Refusals: 422 `auth.token_name_invalid`, 409 `auth.token_name_taken`, 409
`auth.token_capacity`, 404 `auth.token_not_found`, 500 `auth.storage_error`, 500 `auth.no_random`.

### `POST /api/external/<id>/fetch`

Fetches the external source `<id>` of the runtime image now (the console's `external fetch`,
doc/external.md). 200 `{"ok": true}`; the values follow. Refusals: 404 `external.unknown`, 409
`external.refused` (its fetch is in flight), 501 `system.unavailable`.

### `POST /api/lang/sync`

Starts the download of the language pack of `ui.lang` now (the console's `lang sync`,
doc/i18n.md). 200 `{"ok": true}`; the state shows in the status's `lang`. Refusals: 409 `lang.busy`
(with the reason), 501 `system.unavailable`.

### `POST /api/mqtt/discovery`

Publishes the Home Assistant discovery messages again (the console's `mqtt discovery`,
doc/mqtt.md). 200 `{"ok": true}`. Refusals: 409 `mqtt.refused` (not connected, discovery off),
501 `system.unavailable`.

## Zigbee

The coordinator's network and devices (doc/zigbee.md, spec/zigbee.md). Without a coordinator
(the POSIX build, a target without an 802.15.4 radio, a failed start) every route answers 501
`system.unavailable` with the reason.

### `GET /api/zigbee[?from=<n>]`

```json
{"now_ms": 812345, "generation": 42, "loaded": true, "interviewing": false,
 "network": {"up": true, "pan_id": 6699, "channel": 15, "extended_pan_id": "...", "permit_join_s": 0},
 "count": 1, "max_devices": 32, "from": 0,
 "devices": [{"ieee": "00124b0012345678", "short": 6699, "manufacturer": "IKEA of Sweden",
              "model": "TRADFRI bulb E27 WS opal 980lm", "label": "Hall", "interviewed": true,
              "stale": false, "left": false, "interviewing": false, "last_seen": 1792231392,
              "seen_ago_s": 12, "datapoints": ["zigbee.00124b0012345678.1.on"]}],
 "next": null}
```

Paged by 16 devices: `next` is the `from` of the next page or `null`. `generation` counts the
changes of the list (every WebSocket `zigbee` event increments it). `datapoints` are the names of
the device's datapoints (their values come from `/api/datapoints` or the live feed). `left`: the
device announced its leave (it stays known, rejoining restores it); `stale`: not heard within its
expected interval; `seen_ago_s` only once heard since the start.

### `GET /api/zigbee/export[?from=<n>]`

The device list as E-Home's importer reads it (spec/zigbee.md "Export"; issue #40, the field
round trip without a reachable E-Home): the same paging as the list, each device with its interview
result and every function the mapping derives.

```json
{"format": "homecontrol-zigbee-export", "version": 1, "controller": "homecontrol-a1b2c3",
 "exported": 1792231392,
 "network": {"up": true, "pan_id": 6699, "channel": 15, "extended_pan_id": "...", "permit_join_s": 0},
 "count": 1, "max_devices": 32, "from": 0,
 "devices": [{"ieee": "00124b0012345678", "manufacturer": "IKEA of Sweden", "model": "TRADFRI bulb E27 WS opal 980lm",
              "label": "Hall", "interviewed": true,
              "endpoints": [{"id": 1, "profile": 260, "device": 544, "in": [0, 3, 4, 5, 6, 8, 768, 4096], "out": [5, 25, 32, 4096]}],
              "functions": [{"name": "zigbee.00124b0012345678.1.on", "function": "on_off", "endpoint": 1, "cluster": 6,
                             "attribute": 0, "type": "boolean", "writable": true, "datapoint": "hall.light"},
                            {"name": "zigbee.00124b0012345678.1.level", "function": "level", "endpoint": 1, "cluster": 8,
                             "attribute": 0, "type": "real", "unit": "%", "writable": true, "datapoint": "zigbee.00124b0012345678.1.level"}]}],
 "next": null}
```

`exported` is the controller's wall-clock time in Unix seconds (0 before the clock is set).
`datapoint` is the name the loaded model gives the function (a binding of the runtime image, or the
generic `name`); absent while the model is not loaded. The web UI's "Export device list" joins the
pages into one file `zigbee-devices-<controller>.json` with `format`, `version`, `controller`,
`exported`, `network` and all `devices`.

### `POST /api/zigbee/permit`

Body `{"seconds": 0..254}`: opens the network for joining (0 closes it). 200
`{"ok": true, "seconds": 254}`; joins and interviews follow as WebSocket `zigbee` events.
Refusals: 422 `zigbee.invalid`, 409 `zigbee.refused` (network not up).

### Per device: `/api/zigbee/devices/<ieee>`

`<ieee>` as in the list (16 hex digits; the colon form `00:12:...` is accepted too). Each answers 200 `{"ok": true, "device": {...}}`
with the device as in the list (`DELETE` without `device`).

- `DELETE`: asks the device to leave and forgets it (the console's `zigbee remove`).
- `POST .../identify`: the device identifies itself (blinks) for 10 s.
- `PUT .../label` body `{"label": "<at most 32 bytes, \"\" clears>"}`.
- `POST .../interview`: reads its descriptors and attributes again.

Refusals: 404 `zigbee.unknown_device` (unknown or malformed IEEE), 422 `zigbee.invalid` (label),
409 `zigbee.refused` (with the adapter's reason, e.g. the device is not interviewed yet).

## Language packs (static files, not under `/api/`)

Open like the UI files; the strings are display text and untrusted input (doc/i18n.md).

- `GET /lang/<code>.json` — a pack in the UI bundle: `en.json` (the base, every key) and the
  committed packs (`de.json`); a flat JSON object of key to string. 404 for a language the bundle
  does not carry.
- `GET /i18n/<code>.json` — the pack the device downloaded for its release (the same format,
  `Content-Encoding: gzip`, `Cache-Control: no-store`); 204 No Content when there is none (an
  expected state, so the browser console stays clean). The UI takes its keys in preference to the
  bundled pack's.

## Runtime image

The configuration E-Home deploys (format: `spec/runtime-image.md`, operation: `doc/image.md`).
Authenticated like every endpoint: a bearer token, or the session cookie with the CSRF header.

### `PUT /api/image`

The body is the image (JSON, at most 256 KB; larger: 413 `api.body_too_large` before it is read).
The device validates it completely, then swaps it in on the event loop and stores it. 200:

```json
{"ok": true,
 "image": {"schemaVersion": "1.0", "projectId": "example-house", "revision": 7, "generatedAt": "2026-10-17T12:00:00Z",
           "generator": "E-Home 4.2", "name": "Example house", "bytes": 7126},
 "counts": {"datapoints": 17, "knx": 13, "rules": 2, "schedules": 2, "locations": 4, "functions": 9,
            "historyRings": 1, "timeMaster": true},
 "source": "api", "stored": true}
```

`stored: false` with `storeError` when the image runs but could not be written (after a restart
the previous one loads). A refusal changes nothing and reports every problem:

```json
{"error": "the image is invalid", "code": "image.invalid",
 "refusals": [{"path": "datapoints[0].knx.dpt", "reason": "DPT 9.001 carries real values, the datapoint is boolean"},
              {"path": "functions[1].datapoints.state", "reason": "unknown datapoint 'nothing'"}],
 "total": 2, "loaded": true, "revision": 7}
```

`refusals` lists up to 12, `total` counts all; `loaded`/`revision` say what still runs. Status and
code: 400 `image.bad_json` (not JSON; the reason names the byte), 422 `image.version` (a newer
major schema version), 422 `image.invalid`, 413 `image.too_large`, 507 `image.no_memory` (also
`api.no_memory` from the ESP server when no heap block holds the body), 500 `image.apply_failed`
(valid, but refused while applied; the stored image was loaded again), 400 `image.missing` (empty
body). A load takes up to a few hundred milliseconds on the device; requests meanwhile wait.

### `GET /api/image`

The stored image, byte for byte as it was deployed (`application/json`; the ESP server sends it
chunked). 404 `image.none` when none is stored. A deployment tool compares it, or its
`image.revision`, with what it would deploy.

### `DELETE /api/image`

Unloads the image and removes it with the persisted values and history rings; the device returns
to setup (LED: short pulse). 200 `{"ok": true, "loaded": false}`; 500 `image.storage_error` when
the stored copy could not be removed (the image is unloaded anyway).

All three: 404 `image.unavailable` in a build without the image service (POSIX `--demo`), 405
`api.method_not_allowed` for other methods.

## Dashboard

What the device UI reads of the runtime image to build its pages: the model of rooms and functions,
the history, the rules and the schedules. Authenticated like every endpoint; the two `PUT`s with
the CSRF header when the cookie is used. All five: 404 `image.unavailable` in a build without the
image service (POSIX `--demo`), 405 `api.method_not_allowed` for other methods.

### `GET /api/model[?from=<n>]`

The image's locations, functions and datapoint metadata (spec/runtime-image.md 3.3 to 3.5), so that
a client builds the dashboard without the image. Without an image: 200 `{"loaded": false}`.

```json
{"loaded": true, "project": "example-house", "name": "Example house", "revision": 7,
 "generatedAt": "2026-10-17T12:00:00Z", "generator": "E-Home 4.2", "source": "api", "stored": true,
 "loaded_ms": 51230, "now_ms": 90211, "generation": 3, "total": 30, "from": 0,
 "locations": [{"id": "living", "label": {"en": "Living room", "de": "Wohnzimmer"}, "kind": "room", "parent": "ground"}],
 "functions": [{"id": "living-light", "label": "Ceiling light", "kind": "light", "room": "living",
                "datapoints": {"command": 0, "state": 1}},
               {"id": "evening", "label": "Evening", "kind": "scene", "scene": 1, "datapoints": {"command": 12}}],
 "datapoints": [{"id": 4, "name": "living.temperature", "type": "real", "writable": false,
                 "label": "Living room temperature", "unit": "°C", "technology": "knx",
                 "binding": {"knx": {"dpt": "9.001", "listen": ["1/3/1"]}}, "history": true}],
 "next": null}
```

- `label` is the image's label as it is: a string or an object of language tags (the client picks).
- A function's `datapoints` maps each role to a datapoint id of `/api/datapoints` and the live
  feed; roles the loader does not know (spec I5) are passed on, a client ignores what it does not
  know. `room`, `unit`, `scene` and a location's `kind` and `parent` are left out when the image
  has none.
- A datapoint's `technology` is the name of the technology that owns it (`knx`, `scheduler`, ...,
  `null` for an application value); `binding` the image's technology members verbatim (`knx`,
  `zigbee`, `virtual`); `history` whether it keeps a history ring. `label`, `unit` and `binding`
  are left out when absent.
- The datapoints of the rule parameters (spec/rules.md 8.6, after the image's own) have the
  technology `rules` and a member `"parameter": {"rule": "<ruleId>", "parameter": "<parameterId>"}`;
  their `label` is the parameter's label (the rule's `name` for the enable switch `enabled`), their
  `unit` the parameter's unit, `ms` for a duration. A client may show them with the rule's
  controls of `/api/rules` instead of as separate datapoints.
- `loaded_ms` is when the image was applied (with `source`: deployed when and how).
- **Paging**: the entries share one index space, the locations first, then the functions, then the
  datapoints; a page holds as many as fit the response buffer (8 KB). Continue with
  `from=<next>` until `next` is `null`. `generation` changes with every load: a client that sees
  another generation on a later page starts over.

Refusals: 507 `model.no_memory` (the device could not hold the model of this image; the image
runs), 500 `api.response_too_large`.

### `GET /api/history?ids=<id>[,<id>...][&from=<unix>][&to=<unix>][&points=<n>]`

The short-term history (doc/persistence.md) of up to 8 datapoints over `[from, to)` (UTC seconds;
default the last 24 h), decimated into `points` buckets of equal width (default 48; `points` times
the number of ids at most 160):

```json
{"from": 1792145000, "to": 1792231400, "width_s": 1800,
 "series": [{"id": 4, "points": [[1792145412, 21.5], [1792147200, 21.75, 21.5, 22, 6]]},
            {"id": 0, "code": "history.none", "error": "this datapoint has no history"}]}
```

A point is `[t, value]` for a bucket with one sample (`t` its time) or `[t, last, min, max, count]`
for a bucket of several (`t` the bucket's start). Empty buckets are left out; values have float
precision. Refusals: 400 `history.bad_query` (ids missing, malformed or more than 8; `from >= to`;
`points` 0 or too many), 409 `history.no_time` (the wall clock is not synchronized and `from` or
`to` is missing).

### `GET /api/rules[?from=<n>]`

The rules of the image with their diagnostics (spec/rules.md 9.1) and parameters, paged like the
datapoints (`count`, `from`, `next`):

```json
{"now_ms": 90211, "count": 2, "from": 0, "next": null,
 "rules": [{"id": "hall-presence-light", "name": "Hall light by presence",
            "enabled": true, "enabledAdjustable": true, "enabledOverridden": false, "firings": 12, "state": null,
            "lastTrigger": {"text": "datapoint hall.presence", "ms": 88000},
            "lastAction": {"text": "write hall.light = true", "ms": 88000},
            "errors": 0, "lastError": null,
            "timers": [{"id": "off", "remaining_ms": 95000}],
            "parameters": [{"id": "timeout", "label": "Switch-off delay", "type": "duration", "unit": "min",
                            "adjustable": true, "overridden": false, "value": 180000, "default": 180000,
                            "min": 30000, "max": 3600000, "step": 30000}]}]}
```

`lastTrigger`, `lastAction`, `lastError` (`{"code", "text", "ms"}`) are `null` until they happen;
`*_ms`/`ms` are monotonic like `now_ms`. `timers` lists the running ones. `parameters` are the
declared parameters (`enabled`, parameter 0, is the `enabled*` members): `type` is the rules
type name, `value`/`default`/`min`/`max` in the parameter's JSON form (boolean; number for
integer, real, enumeration and duration (ms); string; `{"hour","minute","second"}` for a time of
day; `{"year","month","day"}` for a date; `{"control","value"}`; the literal text otherwise),
`min`, `max`, `step` and `choices` (`[{"value", "label"}]`) only when declared. The rule's `name`,
a parameter's `label` and a choice's `label` are labels (spec/rules.md 2.2) passed through as the
image gives them: a string, or an object from language tags to strings
(`{"en": "Staircase", "de": "Treppenhaus"}`) that the UI resolves in the viewer's language. The
device's log and the diagnostics' texts use the label resolved in the setting `ui.lang`.

### `PUT /api/rules/<rule>/parameters/<parameter>`

Body `{"value": <JSON value>}` sets the parameter (validated against its type, range and choices,
persisted, spec/rules.md 8.2); `{"reset": true}` returns it to the image's default. The rule's
enable switch is the parameter `enabled`. 200 `{"ok": true, "now_ms", "rule": {...}}` with the
rule as in the list. Refusals: 404 `rule.unknown`, 404 `rule.unknown_parameter`, 403
`rule.not_adjustable`, 422 `rule.invalid_value` (the reason: "outside min..max", "not one of the
choices", "not a value of the parameter's type"), 400 `rule.value_missing`, 400 `api.bad_json`.
Path segments may be percent-encoded.

### `GET /api/schedules[?from=<n>]`

```json
{"synchronized": true, "now": 1792231392, "count": 2, "from": 0, "next": null,
 "schedules": [{"id": "workdayMorning", "label": "Workday morning", "kind": "weekly", "enabled": true,
                "adjustable": true, "nextFiring": 1792384200, "lastFired": 1792297800}]}
```

`nextFiring` (UTC seconds) is `null` when the entry has none (switched off, a past one-shot, no
sun times, the clock not synchronized); `lastFired` is `null` when unknown; `adjustable` is the
image's `enabledAdjustable`.

### `PUT /api/schedules/<id>`

Body `{"enabled": true|false}`: switches the entry like the console does (persisted; switching back
to the image's value removes the override). 200 `{"ok": true, "schedule": {...}}`. Refusals: 404
`schedule.unknown`, 403 `schedule.not_adjustable` (with the scheduler's reason), 400
`schedule.value_missing`, 400 `api.bad_json`.

## Other refusals

404 `api.not_found` (no such endpoint), 405 `api.method_not_allowed`, 400 `api.bad_json` (with the
parser's reason), 500 `api.response_too_large`, 503 `api.busy` (the event loop did not take the
request within 5 s, ESP).

## E-Home connection (issue #41)

E-Home is a service on the public internet that manages many installations; it can never reach a
controller in a home's private network. So the controller connects to E-Home, outbound, over TLS,
and only while its owner has switched the connection on for configuration and deployment. Firmware
does not travel over this connection (it comes from the release server of `update.url` or the
browser upload, doc/ota.md). The connection carries the controller's live feed to E-Home and
E-Home's requests to the controller, in the same JSON the browser and the local API use. The
controller's side is described in doc/ehome.md.

### Settings

| Key | Type | Default | Meaning |
|---|---|---|---|
| `ehome.url` | string, `https://host[:port][/path]`, at most 127 bytes | empty | The E-Home service the controller is enrolled with (set by the enrollment). No user info, query or fragment; `http://` only for development. |
| `ehome.id` | string, 1 to 64 of `[A-Za-z0-9._-]` | empty | The controller's id at that service (assigned by the enrollment). |
| `ehome.name` | string | empty | The installation's name at E-Home, shown on the Settings page; a `ready` with another name updates it. |
| `ehome.secret` | secret, 64 hex digits | empty | The controller's credential (received at the enrollment; never shown, listed or logged). |
| `ehome.socket` | string, `wss://…`, at most 127 bytes | empty | The socket address of the enrollment's answer (not listed; `ws://` only with an `http://` `ehome.url`). |
| `ehome.enabled` | boolean | `false` | The owner's switch: while true the controller keeps the connection up (with the backoff of doc/network.md, 1 s doubling to 60 s); while false it never connects, and switching it off closes an open connection at once. |

The settings list shows `ehome.url`, `ehome.id` and `ehome.name` read-only and `ehome.enabled`
editable (`PUT /api/settings/ehome.enabled` with `true`/`false`, the switch on the Settings page;
switching on without an enrollment is refused 422 `ehome.not_enrolled`). "Disconnect from E-Home"
(`DELETE /api/ehome`, `{"ok": true}`; 409 `ehome.enrolling` while an enrollment runs) closes the
connection and erases all six.

### Enrollment

In E-Home the installer opens the project and asks for an enrollment code (valid 15 minutes, one
use). On the controller's Settings page the owner enters the E-Home address and the code:

`POST /api/ehome/enroll` with `{"url": "https://ehome.example", "code": "ABCD-EFGH"}` (session or
token principal, CSRF header; the code has 1 to 32 of `[A-Za-z0-9-]`). The controller calls
E-Home's `POST <url>/api/controllers/enroll` with `{"code": "ABCD-EFGH", "name": "<host name>",
"version": "<firmware>", "build": "<build id>"}` (the name is the controller's mDNS host name) over
TLS (the IDF certificate bundle; plain `http://` only for development) and expects
`200 {"controller": "<id>", "installation": "<name>", "secret": "<64 hex digits>", "socket": "wss://ehome.example/api/controllers/<id>/socket"}`.
It stores the six settings with `ehome.enabled` true, answers `200 {"ok": true, "installation":
"<name>"}` and connects. The HTTP answer waits for E-Home's (at most 30 s).

Refusals, each with the reason in `error`:

| Status | Code | When |
|---|---|---|
| 400 | `ehome.url_invalid`, `ehome.code_invalid` | The address or the code is malformed (checked before E-Home is called). |
| 409 | `ehome.enrolled` | The controller is enrolled already (disconnect first), or E-Home answered 409 (enrolled there). |
| 409 | `ehome.enrolling` | Another enrollment is under way. |
| 404 | `ehome.code_unknown` | E-Home answered 404: wrong or used code. |
| 410 | `ehome.code_expired` | E-Home answered 410. |
| 502 | `ehome.unreachable` | No answer (DNS, TLS, refused, 30 s without an answer) or another HTTP status from E-Home. |
| 502 | `ehome.bad_answer` | E-Home's 200 lacks a member or one is malformed (the socket not `wss://`, the secret not 64 hex digits). |

For 404, 410 and 409 the controller passes E-Home's own `code` and `error` when its `code` starts
with `ehome.`. The enrollment is not reachable over the E-Home connection itself.

### The socket

With `ehome.enabled` true the controller opens the WebSocket of `socket` (TLS) and authenticates:

```
E-Home → {"type": "challenge", "nonce": "<32 to 128 hex digits>"}
device → {"type": "auth", "controller": "<id>", "mac": "<64 hex digits>", "version": "0.1.0", "build": "a9d0216"}
E-Home → {"type": "ready", "installation": "<name>"}        or {"type": "refused", "code": "...", "error": "..."} and the close
```

`mac` is HMAC-SHA256 with the 32 bytes the secret's hex digits denote as the key, over the bytes the
nonce's hex digits denote, written as 64 lower-case hex digits. Test vector: secret
`00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff`, nonce
`0123456789abcdef0123456789abcdef` → mac
`696a8d99e57711fdb022863b6c952a393186157fa4f95d43d5b9943e657f13e9`. The handshake must complete
within 30 s; any other message before `ready` fails the attempt (the backoff retries), `refused`
shows its `error` as the connection's state.

Text messages are at most 4096 bytes in both directions; the controller closes the socket on a
longer one. After `ready` the socket is the live feed of this document's WebSocket section: E-Home sends
`{"type": "subscribe"}` and receives `status`, `datapoints`, `zigbee` and `monitor` messages exactly
as a browser does, and sends `command`, `read` and the monitor's `send`/`read` as a browser does
(the monitor shows them with `"via": "ehome"`). In addition E-Home makes requests:

```
E-Home → {"type": "request", "id": 7, "method": "GET", "path": "/api/zigbee/export?from=0"}
device → {"type": "response", "id": 7, "status": 200, "body": {...}}
```

`id` is a number or a string (its JSON text below 40 bytes), echoed as given. `method`, `path` (with
query) and `body` are what the local API takes; the answer carries the route's status, its JSON body
(or `{"error", "code"}` on a refusal; `null` for none) and `retry_after_s` when the route sets
one. A response body is at most 4 KB (a larger one is answered 500 `api.response_too_large`); a
stored document (`GET /api/image`, `GET /api/coredump`) is streamed instead:
`{"type": "response", "id": 9, "status": 200, "length": 10226, "content_type": "application/json"}`
followed by binary messages of at most 2 KB carrying `length` bytes; the request ends with the last
byte. Live feed messages (text) may arrive between those binary messages.

A request body that does not fit the 4 KB text message (the runtime image) is announced with
`length` instead of `body`: `{"type": "request", "id": 8, "method": "PUT", "path": "/api/image",
"length": 10226}` followed by binary messages carrying the body (their number does not matter, only
the byte count); the controller answers after the last byte. When `length` exceeds what the route
accepts the answer is 413 `api.body_too_large`, when the controller has no memory for it 507
`api.no_memory`; the announced bytes are then read and dropped. A malformed request is answered 400
`ehome.bad_request`; binary messages nobody announced are dropped.

Requests run with the "E-Home" principal: everything a token may do, minus what is decided on site
only. Refused with 403 `ehome.forbidden` (and logged): `/api/auth/*` (onboarding, login, password),
`/api/tokens*`, `/api/system/wifi`, `/api/system/factory-reset`, `/api/system/update/upload`,
`/api/ehome*` (enrollment and disconnect), and writing `update.url` or any `ehome.*` setting. One
request at a time per socket; a second before the response is refused 429 `ehome.busy` (a second
that announces `length` closes the socket: its binary messages could not be told apart). The device
pings every 30 s; a pong missing after 10 s closes the socket and the backoff reconnects it.

### Status

`GET /api/status` gains `"ehome": {"enabled": true, "enrolled": true, "state": "connected" |
"connecting" | "off" | "failed", "since_s": 812, "installation": "<name>", "error": "..."}`
(`installation` when enrolled, `error` while failed; the address is the `ehome.url` setting); the
WebSocket `status` message carries the same member; the Settings page shows it beside the switch.
E-Home shows each controller of a project as online or offline with the last contact.

## WebSocket `/ws`

Text messages, each one JSON object with a `type`. At most 4 clients; a fifth gets an `error`
message `ws.too_many_clients` and the close code 1013 (try again later). A handshake whose `Origin`
names another host than its `Host` is refused with `ws.foreign_origin` and 1008.

**Authentication**: the session cookie of the upgrade request authenticates the connection at once.
Without it the first message must be `{"type": "auth", "token": "hct_..."}` (answer
`{"type": "auth", "ok": true}`), within 10 s (otherwise 1008 `auth.timeout`). Any other message
from an unauthenticated client closes it with 1008 `auth.required`; a wrong token with 1008
`auth.token_invalid`. The feed re-checks every second: a logout, an expired session or a revoked
token closes the connection with 1008 `auth.session_ended`.

Server to client:

| type | content |
| --- | --- |
| `hello` | on connect: `authenticated`, `onboarded`, `device`, `version` |
| `status` | the members of `GET /api/status`; when one of its discrete fields changes (network state, address, LED, time sync, Improv, mDNS, sessions, clients, datapoint count) and every 10 s |
| `datapoints` | `now_ms`, `updates`: `[{"id", "value", "quality", "updated_ms"}]`. After `subscribe`: a snapshot in chunks with `snapshot: true`, `count`, `from`, `done` and the full datapoints (as in the REST API); then the changes, batched per event-loop turn (each datapoint once, with its latest value) |
| `result` | answer to `command`: `id`, `ref` (echoed), `ok`, and on refusal `error` and `code` (the `dp.*` codes of the REST command) |
| `pong` | answer to `ping` |
| `monitor` | the monitor stream: the answer to a `monitor` message, and the event batches (see "Monitor") |
| `zigbee` | a change of the Zigbee network or its devices: `event`, `ieee` (the device, 16 hex digits; not for `network` and `permit`), `permit_join_s` (only `permit`), `generation` (as in `GET /api/zigbee`) |
| `error` | `error`, `code`: `ws.bad_message`, `ws.unknown_type`, and the refusals before a close |

Client to server:

| type | content |
| --- | --- |
| `auth` | `token` (see above) |
| `subscribe` | start the snapshot and the updates |
| `command` | `id`, `value`, optional `ref` (any JSON value, echoed in the result) |
| `ping` | keep-alive; the UI sends one every 20 s (the ESP server purges the least recently used connection when its sockets run out) |
| `monitor` | `subscribe` (with `since`), `send` or `read` (see "Monitor") |

The `zigbee` events (`hc::zigbee::ZigbeeEvent`): `network` (the network formed or went down),
`permit` (joining opened, extended or closed), `joined` (a new device), `interviewing`,
`interviewed`, `interview_failed`, `left` (the device announced its leave), `removed` (forgotten,
`DELETE`), `changed` (a new label, gone stale, heard again, back after a leave). A
client reads `GET /api/zigbee` for the details; a missed event shows as a jump of `generation`.

A client that misses a message (the server's send buffers were full) gets a fresh snapshot. Close
codes: 1007 invalid UTF-8, 1008 policy (authentication, origin), 1009 message longer than 1024
bytes, 1003 binary or fragmented messages, 1013 too many clients or the controller busy.

## Monitor

The monitor stream (issue #19) is what E-Home's group monitor shows: the group telegrams of the KNX
bus, the Zigbee frames, and what the controller did itself (the rules' commands, the commands from
the UI and the API, the image loads), with the datapoint names of the loaded image. The device keeps
the last 128 events in RAM, each with a sequence number (0 at start, counting up; a restart begins
at 0 again). A reader asks for the events from a sequence number on; one that asks for events that
were overwritten gets a gap marker first, never an error.

### Events

Every event has `seq`, `ms` (monotonic), `t` (seconds since the epoch, only while the wall clock is
synchronized), `kind`, and, when it concerns a datapoint of the current model, `dp` (its id) and
`name`; `value` (in the datapoint's JSON form, as in "Datapoints") when it carries one. After a model
change (an image load or a reload of the providers' datapoints) the older events lose `dp` and
`name` (the ids may mean other datapoints) and keep their `value`.

```json
{"seq": 812, "ms": 80211, "t": 1792231392, "kind": "knx", "dir": "rx", "src": "1.1.10", "dst": "1/2/3",
 "service": "write", "data": "0C 33", "prio": 3, "hops": 6, "dp": 4, "name": "living.temperature", "value": 21.5}
{"seq": 813, "ms": 80240, "kind": "knx", "dir": "tx", "src": "1.1.250", "dst": "1/0/1", "service": "write",
 "data": "01", "short": true, "prio": 3, "hops": 6, "origin": "monitor", "dp": 0, "name": "living.light", "value": true}
{"seq": 814, "ms": 80300, "kind": "zigbee", "event": "report", "ieee": "00124b0001020304", "short": 6699,
 "ep": 11, "cluster": 6, "attr": 0, "dp": 21, "name": "zigbee.00124b0001020304.11.on", "value": true}
{"seq": 815, "ms": 80310, "kind": "zigbee", "event": "action", "ieee": "000b57fffe112233", "ep": 1,
 "cluster": 6, "cmd": 2, "dp": 22, "name": "hall.button", "value": 2}
{"seq": 816, "ms": 80320, "kind": "rule", "rule": "hall-presence-light", "dp": 3, "name": "hall.light", "value": true}
{"seq": 819, "ms": 95000, "kind": "rule", "rule": "temp-watch", "action": "read", "dp": 4, "name": "living.temperature"}
{"seq": 817, "ms": 80400, "kind": "command", "origin": "ws", "via": "session", "dp": 5, "name": "living.setpoint", "value": 21}
{"seq": 818, "ms": 90000, "kind": "image", "event": "load", "project": "example-house", "revision": 7, "source": "api"}
{"kind": "gap", "seq": 700, "count": 12}
```

- `knx`: a group telegram, `dir` `rx` (from the bus) or `tx` (sent by the controller, reported when
  the gateway confirmed it). `src` the individual address, `dst` the group address, `service`
  `write`, `response`, `read` (or `other`), `data` the application data in hex octets separated by
  spaces (for a short telegram one octet with the 6-bit value, `short: true`; `""` for a read),
  `prio` 0 (system) to 3 (low), `hops` the routing counter, `repeated: true` for a repetition,
  `origin: "monitor"` for a telegram a monitor client sent. `dp`/`name`/`value`: the first datapoint
  listening on the address and the value it took (for a received telegram the decoded value, absent
  when the data did not decode; for a sent one the datapoint's value).
- `zigbee`: `event` `report` (an attribute report or read response record; `attr` the attribute),
  `action` (a cluster command the device sent, a button or a zone change; `cmd` the ZCL command id)
  or `command` (a command the controller sent for a datapoint; `cmd` the ZCL command id). `ieee`
  (16 hex digits), `short` (the network address, when known), `ep` the endpoint, `cluster`. `value`
  is the datapoint's value, or for an attribute no datapoint maps its decoded number or string.
- `rule`: a rule commanded a datapoint; `rule` is its id (cut to 27 bytes). With `"action": "read"`
  the rule asked the technology for the datapoint's value (spec/rules.md 7.12); the resulting
  telegram follows as its own event, and no `value` is given.
- `command`: a datapoint was commanded from outside the rules; `origin` `ws` (a WebSocket client's
  `command`), `api` (`POST /api/datapoints/<id>/command`) or `app` (the controller itself: the
  console, a component); `via` `session` or `token` for `ws` and `api`. A command reaches the
  technology first: a Zigbee `command` event precedes the `command` event that caused it.
- `image`: `event` `load` (an image was applied), `reload` (the model was rebuilt for the providers'
  datapoints, e.g. a Zigbee device joined) or `unload`; `project`, `revision`, `source` (as in
  /api/status) when an image is loaded.
- `gap` (only in a reply, never stored): the events from `seq` on, `count` of them, were overwritten
  before the reader got them.

### Over the WebSocket

After authentication (see "WebSocket"):

- `{"type": "monitor", "subscribe": true, "since": <n>}` turns the stream on for this connection.
  Without `since` it starts with the next event; with `since` it starts at that event (a catch-up
  after a reconnect: the client sends the `next` of the last batch it got); a `since` beyond the
  newest event (a sequence of an earlier run of the device) starts at the oldest event held and the
  answer says `reset: true`. Answer: `{"type": "monitor", "subscribed": true, "since", "first",
  "next", "capacity": 128}` (`first` the oldest event held, `next` the sequence the next event
  gets). `{"type": "monitor", "subscribe": false}` turns it off (answer `"subscribed": false`).
  Both echo a `ref` (number or string) if given. The datapoint subscription (`subscribe`) is
  independent.
- The events follow as `{"type": "monitor", "now_ms", "events": [...], "next": <n>}`: what happened
  in one event-loop turn, in messages of at most 1400 bytes (up to 4 messages per client and turn;
  the rest follows in the next turns). `next` is the sequence after the last event of the message.
  A client whose connection cannot take messages (send buffers full) is not queued for: the batch
  is retried, and once the ring has overtaken the client's position the next batch starts with a
  `gap` marker.
- `{"type": "monitor", "send": {"knx": {"address": "1/2/3", "dpt": "9.001", "value": 21.5}}, "ref": 7}`
  sends a group write of the value encoded as the DPT (the value in the JSON form of the DPT's
  value type). `"service": "response"` sends a group response instead. Instead of `dpt` and
  `value`, `"data": "0C 33"` sends the octets after the APCI as they are (1 to 14), and
  `"data": "01", "short": true` a 6-bit value in the APCI octet (what E-Home's own encoder
  produces).
- `{"type": "monitor", "read": {"knx": {"address": "1/2/3"}}}` sends a group read; the response comes
  as an `rx` event with `service: "response"`.
- Both answer `{"type": "monitor", "result": "send" | "read", "ok": true, "ref"}` when the tunnel took
  the telegram (its `tx` event, marked `origin: "monitor"`, follows when the gateway confirms), or
  `ok: false` with `error` and `code`: `monitor.tunnel_down` (the KNX tunnel is not up),
  `monitor.queue_full` (try again), `monitor.knx_unavailable` (the controller has no KNX tunnel),
  `monitor.bad_address`, `monitor.bad_dpt` (unknown or unsupported DPT), `monitor.value_missing`,
  `monitor.bad_value` (with the codec's reason), `monitor.bad_request` (not `{"knx": {...}}`, or a
  service other than write and response), `monitor.unsupported` (a Zigbee target: command the
  datapoint instead). Sending needs the same authentication as a command; there is no confirmation
  of the bus devices beyond the gateway's.
- A malformed `monitor` message (none of `subscribe`, `send`, `read`; `since` not a sequence number)
  is answered with an `error` message `ws.bad_message`.

### `GET /api/monitor[?since=<n>][&limit=<m>]`

The same events for tools without a WebSocket. Without `since` it starts with the oldest event held;
`limit` is 1 to 128 (default 50). The page holds as many events as fit the response buffer:

```json
{"first": 700, "next": 828, "since": 812, "events": [...], "next_since": 828, "more": false}
```

Continue with `since=<next_since>` while `more` is true. A `since` beyond `next` starts at `first`
with `reset: true`; one below `first` gets a `gap` marker first. Refusals: 400 `monitor.bad_query`,
404 `monitor.unavailable` (a server without the live feed), 405 `api.method_not_allowed`.

## Examples

```sh
DEV=http://homecontrol-a1b2c3.local
TOKEN=hct_...   # from the console: web token create scripts
curl -s -H "Authorization: Bearer $TOKEN" $DEV/api/status
curl -s -H "Authorization: Bearer $TOKEN" $DEV/api/datapoints
curl -s -H "Authorization: Bearer $TOKEN" -X POST -d '{"value": true}' $DEV/api/datapoints/0/command
curl -s -H "Authorization: Bearer $TOKEN" -X PUT -d '{"value": "pool.ntp.org"}' $DEV/api/settings/ntp.server
curl -s -H "Authorization: Bearer $TOKEN" "$DEV/api/log?after=0"
curl -s -H "Authorization: Bearer $TOKEN" -X PUT --data-binary @image.json $DEV/api/image
curl -s -H "Authorization: Bearer $TOKEN" $DEV/api/image -o deployed.json
curl -s -H "Authorization: Bearer $TOKEN" -X DELETE $DEV/api/image
curl -s -H "Authorization: Bearer $TOKEN" "$DEV/api/model"
curl -s -H "Authorization: Bearer $TOKEN" "$DEV/api/history?ids=4&points=24"
curl -s -H "Authorization: Bearer $TOKEN" -X PUT -d '{"value": 600000}' $DEV/api/rules/hall-presence-light/parameters/timeout
curl -s -H "Authorization: Bearer $TOKEN" -X PUT -d '{"enabled": false}' $DEV/api/schedules/workdayMorning
curl -s -H "Authorization: Bearer $TOKEN" "$DEV/api/monitor?since=0&limit=20"
curl -s -H "Authorization: Bearer $TOKEN" -X POST -d '{"seconds": 254}' $DEV/api/zigbee/permit
curl -s -H "Authorization: Bearer $TOKEN" -X PUT -d '{"tag": "hc.knx", "level": "debug"}' $DEV/api/system/log-level
curl -s -H "Authorization: Bearer $TOKEN" -X POST $DEV/api/system/update/check
curl -s -H "Authorization: Bearer $TOKEN" -X POST $DEV/api/system/restart
```

## Diagnostics

- `GET /api/status` carries a `boot` member: `{"reason": "<reset reason in words>", "count": <boots>, "brownouts": <n>, "previous_uptime_s": <s>, "coredump": <bool>}` (doc/diagnostics.md; a POSIX build reports its PID, start time and the signal that stopped the previous run instead).
- `GET /api/coredump` streams the stored core dump (`application/octet-stream`, the ELF the IDF wrote) for `esp-coredump info_corefile -c core.bin -t raw <homecontrol.elf>`; `404 coredump.none` without one. Session or token as for every route. `DELETE /api/coredump` erases it (see "System").
