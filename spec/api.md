# The device API (REST and WebSocket)

The controller's web server (doc/web.md) offers a JSON API under `/api/` and a live feed at `/ws`.
The device's own UI (`web/ui/dist/`) uses nothing else; machines (scripts, E-Home, a home server)
use the same endpoints with a bearer token. The core implements both (`hc::WebApi`,
`hc::LiveFeed`); the ESP and POSIX servers only carry HTTP. This document is the contract; a change
that breaks a client needs a new path or field, not a changed meaning.

## Conventions

- Bodies are UTF-8 JSON objects (`Content-Type: application/json`), at most 1024 bytes in a
  request (413 `api.body_too_large` otherwise). Responses carry `Cache-Control: no-store`.
- Every refusal is a 4xx/5xx status with the body `{"error": "<English reason>", "code": "<key>"}`.
  The `code` is stable and is the key a UI translates (doc/web.md, "Texts"); the `error` text is
  for logs and humans and may change. Some refusals add fields (listed below).
- Times: `*_ms` are milliseconds of the controller's monotonic clock (since boot; compare with
  `now_ms` of the same response), `*_s` are seconds, `unix` is seconds since the epoch.
- The ESP device serves everything on port 80. The POSIX build serves HTTP on `--http <port>`
  (default 8080) and the WebSocket on the port above it; `GET /api/auth/session` says which
  (`ws_port`, 0 = the page's port).

## Authentication

A device without a password is **not onboarded**: it answers only `GET /api/auth/session` and
`POST /api/auth/onboard`; everything else is refused with 403 `auth.onboarding_required`.

Once onboarded, every endpoint except the four under `/api/auth/` needs either

- the **session cookie** `hc_session` (set by onboarding and login: `HttpOnly; SameSite=Strict;
  Path=/`, no `Max-Age`, i.e. a browser-session cookie; the device forgets all sessions at reboot
  and after 24 h without use), or
- a **bearer token**: `Authorization: Bearer hct_<32 hex digits>`, created on the serial console
  with `web token create <name>` (shown once; doc/web.md).

Without either: 401 `auth.required`; with an unknown or revoked token: 401 `auth.token_invalid`.

**CSRF**: a POST, PUT or DELETE authenticated by the cookie must carry the header
`X-Requested-With: HomeControl` (403 `auth.csrf` otherwise). A cross-site form cannot set it, and a
cross-site script cannot send it without a CORS preflight, which the device never grants. Requests
with a bearer token need no such header.

### `GET /api/auth/session` (open)

```json
{"onboarded": true, "authenticated": true, "via": "session", "device": "homecontrol-a1b2c3",
 "version": "0.1.0-dev", "ws_port": 0, "min_password": 8}
```

`via` is `"session"`, `"token"` or `null`.

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
 "datapoints": 8}
```

`update` is platform-specific: always `available` (whether the device updates itself), the ESP
build adds `partition`, `confirmed`, `checking`, `source` (update.url, `""` if none), `last`,
`last_ago_s` and, with a source, `next_check_s` (doc/ota.md).

## Datapoints

A datapoint (doc/datapoints.md) in the API:

```json
{"id": 0, "name": "Living room light", "type": "boolean", "flags": 3, "writable": true,
 "value": true, "quality": "good", "updated_ms": 51230}
```

`type` is the value type's name (`boolean`, `unsigned`, `signed`, `real`, `enumeration`,
`string`, `raw`, `time of day`, `date`, `date-time`, `controlled`, `color`; doc/datapoints.md);
`flags` are the datapoint's flag bits; `quality` is `unknown` (no value yet), `good` or `stale`.
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
refused; the log says why), 503 `dp.busy`.

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
              {"key": "wifi.ssid", "value": "...", "default": "", "editable": false}]}
```

`value` is `null` when the setting is not stored (the default applies). Secrets (`wifi.psk`, the
password and token hashes `web.*`) never appear.

### `PUT /api/settings/<key>`

Body `{"value": "<string, at most 127 bytes>"}`; `""` erases `update.url`. Validated as on the
console (`tz` a POSIX TZ string, `ntp.server` a host name, `update.url` an http(s) URL; doc/ota.md)
and applied at once. 200 with the settings list as for GET. Refusals: 404 `settings.unknown`, 403
`settings.not_editable`, 400 `settings.value_missing`, 422 `settings.invalid` (with the reason),
500 `settings.storage_error`.

## Other refusals

404 `api.not_found` (no such endpoint), 405 `api.method_not_allowed`, 400 `api.bad_json` (with the
parser's reason), 500 `api.response_too_large`, 503 `api.busy` (the event loop did not take the
request within 5 s, ESP).

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
| `error` | `error`, `code`: `ws.bad_message`, `ws.unknown_type`, and the refusals before a close |

Client to server:

| type | content |
| --- | --- |
| `auth` | `token` (see above) |
| `subscribe` | start the snapshot and the updates |
| `command` | `id`, `value`, optional `ref` (any JSON value, echoed in the result) |
| `ping` | keep-alive; the UI sends one every 20 s (the ESP server purges the least recently used connection when its sockets run out) |

A client that misses a message (the server's send buffers were full) gets a fresh snapshot. Close
codes: 1007 invalid UTF-8, 1008 policy (authentication, origin), 1009 message longer than 1024
bytes, 1003 binary or fragmented messages, 1013 too many clients or the controller busy.

## Examples

```sh
DEV=http://homecontrol-a1b2c3.local
TOKEN=hct_...   # from the console: web token create scripts
curl -s -H "Authorization: Bearer $TOKEN" $DEV/api/status
curl -s -H "Authorization: Bearer $TOKEN" $DEV/api/datapoints
curl -s -H "Authorization: Bearer $TOKEN" -X POST -d '{"value": true}' $DEV/api/datapoints/0/command
curl -s -H "Authorization: Bearer $TOKEN" -X PUT -d '{"value": "pool.ntp.org"}' $DEV/api/settings/ntp.server
curl -s -H "Authorization: Bearer $TOKEN" "$DEV/api/log?after=0"
```
