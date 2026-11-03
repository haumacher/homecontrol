# The web server and the device UI

The controller serves its own UI and a JSON API (issue #5): onboarding, login, the dashboard of the
deployed image's rooms and functions with live values (issue #14), the rules and schedules with
what the image lets the inhabitant adjust, the settings, the log. The model is Home Assistant's dashboard (a dark
sidebar layout with tiles, phone first) without its bulk: no add-ons, no user management, no
history database, no frameworks. The API contract is `spec/api.md`.

## Pieces

| Where | What |
| --- | --- |
| `core/` `WebApi` | the REST API: routing, authentication, CSRF check, JSON in and out; the dashboard's endpoints (model, history, rules, schedules) |
| `core/` `image::ModelView` | what the UI needs of the loaded image beyond the components: locations, functions with roles, datapoint labels, units and bindings, parameter steps, fixed schedule switches; built at every load |
| `core/` `LiveFeed` | the WebSocket feed: clients, snapshots, batched updates, commands, status pushes |
| `core/` `Auth` | password (PBKDF2), sessions, tokens, rate limiting |
| `core/` `websocket` | the RFC 6455 codec (handshake key, frame decoder, close payload, UTF-8 check) |
| `core/` `JsonWriter`, `json_parse`, `value_json` | JSON without allocation; datapoint values in JSON |
| `core/` `LogRing` | the last ~4 KB of log lines for `GET /api/log` |
| `core/` `DemoTechnology` | eight simulated datapoints for UI development (POSIX `--demo`, console `web demo`) |
| `platform/esp/components/hc_web` | esp_http_server, the job hand-over to the loop, the WebSocket send pool, LittleFS, the `web` console command |
| `platform/posix/` `WebServer` | cpp-httplib for HTTP, the core codec on a raw socket for the WebSocket |
| `web/ui/dist/` | the UI: plain ES modules, one stylesheet, no build step |
| `platform/esp/tools/gen_ui_image.py` | gzip and content hash for the LittleFS image |

Everything that decides something is in the core and has host tests; the platforms carry bytes.
Both servers call the core only on the event loop's thread: the ESP server copies each request into
a static job slot and posts it through the `LoopQueue` (the httpd task waits up to 5 s, then answers
503 `api.busy`); the POSIX server runs it through a `LoopExecutor` the main loop drains.

## Decisions

### Transport: plain HTTP

The device speaks HTTP on port 80, no TLS. The session cookie and the password cross the LAN in
clear; anyone on the same network who can sniff traffic (an open or shared WiFi, a compromised
device) can take over a session. Accepted for now because the device lives in a home LAN behind the
router, and because TLS on a device that has no public name has no good answer yet: a self-signed
certificate makes every browser warn (training users to click through), and a certificate for a
local name needs a public domain and a DNS-01 renewal path. TLS also costs about 40 KB of RAM per
connection with mbedTLS. It stays issue #20's open decision. Remote access goes through a VPN to
the home network, not by exposing the device.

### One device password, set by onboarding

A new device has no password. It serves the UI, and the UI shows only the onboarding page; the API
refuses everything except the session query and onboarding with 403 `auth.onboarding_required`
(a visible message in the UI, a JSON reason for scripts). The first person to open the page sets
the password (8 to 64 characters) and is logged in. The window is real (someone else on the LAN
could be first), and it is as short as the owner makes it; the alternative, a printed default
password, would be the same for every device or need a per-device label. A forgotten password is
reset on the serial console (`web password clear`), which needs physical access.

There is a single password and no users: the controller has one owner. Rejected: user accounts
(roles, management pages, all for a household that shares one panel).

### Password storage: PBKDF2-HMAC-SHA256, 10 000 iterations

The setting `web.pw` holds `pbkdf2-sha256$<iterations>$<salt hex>$<key hex>` with a 16-byte random
salt and a 32-byte key. The format carries its iteration count, so a later build can raise it and
still verify old records (a persisted format, frozen as such). The hash runs outside the event loop
(two passes through `WebApi::handle()`: the first validates and asks for a hash, the platform
computes it in its server thread, the second verifies), so a login never stalls the controller.

The ESP build uses mbedTLS (`mbedtls_pkcs5_pbkdf2_hmac_ext`, SHA on the hardware accelerator), the
POSIX build the core's software implementation. The cost on the host is 33 ms (Debug build). The
cost on the C6 is logged on every login and onboarding (`PBKDF2-HMAC-SHA256, 10000 iterations: N
ms`, tag `hc.web`); it was not measured on the board in this change (no board access) and is
expected between 0.2 and 0.6 s (four SHA-256 blocks per iteration through the driver). 10 000
iterations is below OWASP's 600 000 for PBKDF2-SHA256, which would take minutes on the C6; with
the rate limiting below, an online attacker gets a few guesses per hour, and an offline attacker
needs the flash contents first. Rejected: bcrypt/scrypt/Argon2 (memory-hard by design, which is
what a 512 KB device cannot afford; no hardware help).

### Sessions

A login creates a session: a random 128-bit id (hardware RNG), sent as the cookie
`hc_session=<32 hex>; Path=/; HttpOnly; SameSite=Strict` without an expiry (it ends with the
browser session). The device keeps 8 session slots in RAM; a session ends after 24 h without a
request, at logout, when the password is reset, and at every reboot (nothing is persisted, so a
stolen cookie dies with the next restart). A ninth login replaces the least recently used session.
Each slot has a generation counter, so a WebSocket bound to an ended session notices within a second
and closes. Rejected: signed stateless tokens (JWT; they cannot be revoked without state, and the
state is 8 slots anyway); persisting sessions in NVS (flash wear on every login, and a reboot as a
cheap "log everybody out" is a feature).

### Tokens for machines

Scripts and other systems use bearer tokens: `Authorization: Bearer hct_<32 hex>`. They are created
in the System section of Settings (`POST /api/tokens`) or on the serial console with
`web token create <name>`; either shows the token once, and the device stores only its SHA-256
(`web.tok.N` = `<name>$<hash hex>`, 8 slots), so a flash dump does not reveal tokens. The list
shows the names, revoking deletes one (its WebSocket connections close within a second). Minting
tokens used to be console-only; an installed device has no reachable console (issue #38), so the
UI does it behind the logged-in session (and the CSRF header), which already allows deploying an
image.

### Rate limiting

Failed logins are counted per source address (8 slots, the oldest is reused). The first 5 failures
are free; each further one blocks the address for 30 s, doubling up to 15 min; a block refuses even
the right password, and the API says how long (`retry_after_s`, `Retry-After`; the UI shows a
countdown). A success clears the address. The counters are in RAM: a reboot clears them, which an
attacker cannot trigger from the network.

### Refusals and CSRF

An unauthenticated request gets 401 with `{"error": "...", "code": "auth.required"}`, never a
redirect: the UI decides to show the login page, scripts get a reason. State-changing requests with
the cookie must carry `X-Requested-With: HomeControl`; `SameSite=Strict` already keeps the cookie
off cross-site requests in current browsers, and the header covers older ones (a form cannot set
it, a script cannot without a preflight the device never grants). The WebSocket checks that the
`Origin` of the handshake names the `Host` it was sent to.

### The API

REST under `/api/` for everything that is a request (status, datapoints, commands, log, settings,
authentication, the image's model, history, rules and schedules) and one WebSocket for what changes by itself (datapoint values, the status). The
settings the UI may change are a whitelist: `tz`, `ntp.server`, `update.url`, `ui.lang`; `wifi.ssid` is shown
read-only; secrets never appear. `knx.gateway` joins the whitelist with the KNX package (issue #7),
which is not on this branch. Rejected: everything over the WebSocket (no curl, no caching
semantics, no status codes); server-sent events (a second mechanism for commands would still be
needed); MQTT (a broker dependency for a local UI).

### Live values: one WebSocket, batched per loop turn

`/ws` accepts at most 4 clients (the fifth gets an error message and close code 1013, and the UI
says so). A client authenticates with the cookie of the handshake or with a token in its first
message, then subscribes: the feed sends a snapshot of all datapoints in chunks of up to 1400 bytes
(4 chunks per client and loop turn, so a large model does not hog the loop), then the updates. The
`DatapointListener` callback only records the id of a changed datapoint (each once, up to 32 per
turn) and posts one flush; the flush sends one `datapoints` message per client with the current
values. A burst of updates (a KNX scene) becomes one message, and a datapoint that changes three
times in a turn is sent once, with its latest value. The status goes out when one of its discrete
fields changes and every 10 s as a heartbeat. When a send fails (the platform's buffers are full),
the client is marked for a fresh snapshot instead of queuing without bound: memory stays fixed, and
a slow client converges. The UI pings every 20 s and treats 45 s of silence as a dead connection.

### The monitor stream: one ring, a cursor per client

E-Home's group monitor (issue #19) attaches over the same WebSocket (spec/api.md, "Monitor"). The
core's `MonitorFeed` keeps the last 128 events in a ring inside the live feed (about 12 KB, no
heap): the KNX telegrams (the telegram log's listener, with the value the adapter decoded and the
datapoint's name), the Zigbee reports, button actions and commands (the adapter's traffic
listener), the rules' commands (the engine's observer), the commands from the UI and the API with
their origin (the model's listener), and the image loads and unloads. The live feed attaches it to
these sources through the image service at start and after every model change, so the platforms
wire nothing. A client turns it on with `{"type":"monitor","subscribe":true,"since":n}`; the feed
keeps only a flag and a cursor per client and, once per loop turn, sends what lies between the
cursor and the newest event in messages of up to 1400 bytes (4 per client and turn). A client that
falls behind does not queue: when the ring has overtaken its cursor it gets a `gap` marker with the
number of lost events and continues with the oldest held. `GET /api/monitor?since=n` reads the same
ring for tools without a socket. The monitor also sends a typed (DPT and value) or raw group
write, response or read to the tunnel, as the console's `knx write` does, behind the same
authentication as the commands; its telegrams come back as `tx` events marked `"origin": "monitor"`.

### The field round trip (issue #40)

The Settings page's project card installs a runtime image from a file ("Install project file":
`PUT /api/image` with the file's JSON, the loader's refusals listed), and the Zigbee page exports
the device list for E-Home's importer (`GET /api/zigbee/export`, spec/zigbee.md "Export"). Together
they close the loop for an installation without an E-Home on its network: pair on the device,
export, engineer in E-Home, export the image, install it here.

## Console parity: what the console does, the API does

An installed device's serial console is out of reach, so every console command that operates the
device has a route (issue #38; the mapping is in doc/diagnostics.md, "The console"). The routes
live in the core's `WebApi` (`web_api_system.cpp`), so the POSIX build serves them too; what only a
platform can do goes through `WebHost`'s hooks: `system_action_possible`/`system_action` (restart,
factory reset), `start_update`, `start_upload` (the firmware upload, doc/ota.md "Upload from the
browser"), `clear_coredump`, `set_platform_log_level`/`has_platform_log` and `storage_usage`. Each defaults to "unavailable" with a reason, which the route answers as 501
`system.unavailable`. The services themselves come in through the same setters as before
(`set_knx`, `set_zigbee`, `set_external`, `set_blob_store` next to `set_mqtt` and
`set_language_packs`).

A restart, a factory reset and a WiFi change answer first and act 750 ms later from an event-loop
timer, so the HTTP response leaves the device; a second request in that time gets 409
`system.busy`. Console-only by design: `web password clear` (needs physical access, the recovery
from a lost password), `crash` (a test of the dump pipeline) and `web demo` (a development aid).

### The UI: static files from LittleFS, no framework

`web/ui/dist/` is the UI as it is served: `index.html`, one stylesheet and ES modules (router,
pages, the tiles, the detail sheet, the charts, the live connection, the text lookup) and the
language packs, about 148 KB, 49 KB gzip-compressed (42 KB before the language packs, 19 KB before
the dashboard; the budget is 120 KB). No framework and
no bundler: browsers load ES modules natively, the code stays readable in the device's own files,
and there is no node toolchain in the build. Rejected: Preact/Lit (another dependency and a build
step for a handful of pages); server-rendered pages (state on the device, a reload per action);
files compiled into the firmware (every UI change would be a firmware build and an OTA of 1.3 MB).

The firmware build turns it into `storage.bin`: `gen_ui_image.py` writes each file gzip-compressed
(level 9, no time stamps: the output depends on the content only) plus `VERSION`, the first 16 hex
digits of a SHA-256 over all paths and contents; joltwallet/littlefs's
`littlefs_create_partition_image` packs that into a LittleFS image of the storage partition
(0x3b2000, 268 KB (312 KB before the three Zigbee partitions) on the 4 MB layout), and `flash_args`, `flasher_args.json`, the web flasher's
manifest (sixth part) and `scripts/flasher-local.sh` carry it. The image tool is littlefs-python,
which the component installs into a venv in the build directory on the first build (needs pip
access once). The device mounts the partition read-only at `/storage` (never formats it: a missing
image is reported, not hidden behind an empty file system), serves `/ui/<path>.gz` with
`Content-Encoding: gzip`, `ETag: "<VERSION>"` and `Cache-Control: no-cache` (the browser revalidates
and gets 304 until the UI changes), routes `/api/*` and `/ws` first, and answers every
extensionless path with `index.html` so that `/settings` can be reloaded. Without an image every
page is a 503 that says to flash `storage.bin`; the API works regardless. One hash for all files is
coarser than per-file ETags (a UI change refetches 19 KB), and simpler on the device.

**Gap**: the OTA update replaces the application, not the storage partition. A firmware update
that needs a newer UI must come with a UI image flashed over USB or the web flasher, until the
update manifest grows a UI part (to do with the release workflow).

### Pages

Dark by default, light when the browser prefers it (`prefers-color-scheme`), or either one chosen in
Settings (remembered in the browser's local storage, a convenience only); reduced motion respected
(no pulsing, a dashed outline marks a pending tile instead). A sidebar with Overview, Rules,
Schedules, Zigbee, Log, System and Settings that becomes a drawer below 760 px; a top bar with the device name and
the live state. Every control is a native button, input or select (keyboard operable, focus
visible) with a touch target of at least 44 px. No external resources (fonts, icons and scripts
are all local).

- **Overview**: the dashboard (below). Without an image, in a build without images (POSIX `--demo`)
  or for an image without functions: one plain tile per datapoint with its live value, a writable
  boolean toggles on a tap, and a line says why there is no dashboard.
- **Rules**: one card per rule: the enable switch (or a badge when the image fixes it), the state,
  the last trigger and the last action with their age, running timers, the problem count with the
  last error; then the adjustable parameters: a slider within min..max (step as declared, arrow
  keys send once the value rests), a switch for booleans, a select for choices, a time input for a
  time of day, a number input without a range; durations in the parameter's unit (`min`, `s`, `h`).
  A changed parameter shows its default and a "Reset" button; a refusal shows its reason under
  the parameter. Polled every 10 s; the card being edited is not redrawn.
- **Schedules**: each entry with its kind, its next firing (weekday and time and how far ahead),
  the last firing and its switch (disabled where the image fixes it). Polled every 30 s.
- **Settings**: the deployed project (name, revision, generated when and by what, deployed when and
  how, whether it survives a restart) with **Forget project** (`DELETE /api/image` after a
  confirmation), the **E-Home** card (`ehome.js`, issue #41, doc/ehome.md: the connection's state
  line from the live status's `ehome` member with the reason of a failure; not enrolled: **Connect
  to E-Home** with the address and the code E-Home shows, `POST /api/ehome/enroll`, the answer's
  installation or the refusal under the form; enrolled: the installation, the address, the switch
  `ehome.enabled` and **Disconnect** after a confirmation, `DELETE /api/ehome`; the `ehome.*` keys
  are left out of the generic settings list), the theme, the device status (network, time, memory, update, web) and the
  editable settings with their validation messages (`knx.gateway`, `knx.nat` among them), then the
  **System** section (`system.js`, what the serial console offers, issue #38): firmware update
  (check, update now, and **Install firmware file** (issue #39): reads the chosen file's app
  descriptor and refuses anything but `homecontrol`, confirms with the version, calls
  `POST /api/system/update/upload`, keeps the token in `sessionStorage`, stops the WebSocket, waits
  for the updater's `GET /status`, uploads with progress, waits for the application's open session
  answer and shows the new version and build; offered only on a page opened by the device's IP
  address, otherwise it links there, doc/ota.md "Upload from the browser"), restart, factory reset (the button unlocks only once the device name is
  typed), the core dump (present, download, clear), storage usage, the connections (KNX state and
  gateway, MQTT with "publish discovery", each external source with "fetch", the language pack with
  "sync"), log levels per tag, API tokens (list, create with the token shown once, revoke) and the
  WiFi credentials behind a warning and a confirmation that names the `.local` address to use
  afterwards; logout. Every refusal shows the device's reason under the card.
- **Zigbee** (`zigbee.js`): the network (state, channel, devices, whether joining is open with a
  countdown) with **Pair** (opens for 254 s; again extends, **Close** ends it) and a list of the
  latest joins and interviews from the WebSocket `zigbee` events; one card per device with its
  state (interviewing, new, left, silent, ok), manufacturer and model, IEEE, last seen and the live
  values of its datapoints; **Identify**, **Rename**, **Interview again**, **Remove** (after a
  confirmation). The list is read again on every event and after a reconnect. Without a
  coordinator the page says why (`system.unavailable`).
- **Log**: the ring's lines, polled every 3 s.
- **System** (`diagnostics.js`, issue #42): `GET /api/system/diagnostics` every 5 s while open, in
  the Settings page's card style: memory (free, lowest, heap, largest block) with the die
  temperature, the event loop (missed timer periods, longest turn and when, the loop task's stack
  reserve, timers in use), chip and firmware (SoC, CPU, flash, IDF, partition and OTA state, reset
  reason, starts, brownouts, uptime, core dump), the network (state, address, time connected,
  reconnects, SSID, channel, RSSI, transmit power limit, power save, MAC, gateway, DNS), storage
  (LittleFS, NVS entries), the connections (KNX state, channel, telegrams, reconnects; the Zigbee
  network and its devices), the console's status lines (LED, time, Improv) and every task (name,
  state, priority, stack never used, CPU share). What the platform lacks shows its reason.

### The dashboard

The image's locations become the sections, in the image's order, each titled with the room's label
and its parent's (the floor); the functions without a room come last under "Elsewhere". Each
function is a tile of its kind (`tiles.js`); the image's labels are resolved to the browser's
language (then English, then the first given). The tile's icon is its quick action, its name opens
the detail sheet, the controls below operate the rest:

| Kind | Quick action (icon) | Shown | Controls |
| --- | --- | --- | --- |
| light, switch | toggles `command` | on/off from `state`, else from `command` | — |
| dimmer | toggles `command`; without it `levelCommand` 0 / 100 | the level from `level` (else `levelCommand`), "Off" at 0 | a 0..100 slider that sends `levelCommand` on release; with a writable `step`: − / + sending a 25 % relative step (`{"control": brighter, "value": 3}`, DPT 3.007) |
| blind | — | `position` | up / stop / down (`move` false / `stop` true / `move` true), a slider for `positionCommand` |
| sensor | — | `value` with the function's unit (else the datapoint's) | a 24 h sparkline when the datapoint keeps a history (refreshed every 5 min) |
| climate | — | `temperature`, "heating" from `heating` | − / setpoint / + in 0.5 steps on `setpointCommand` (from `setpoint`, else the last command) |
| scene | sends the KNX scene code `scene − 1` to `command` (`{"control": false, "value": scene − 1}` for a controlled datapoint; scenes count from 1 in the image, from 0 on the bus) | "Scene n" | — |
| button | sends true (or `{"control": true, "value": 1}`) to a writable `press` | `state`, else "Pressed 3 min ago" | — |

A stale value dims the tile's head and says "last known 3 h ago" (the age from the datapoint's
`updated_ms` against the feed's `now_ms`); an unknown one shows "—". A command marks the tile
pending until the datapoint of the reporting role (`state`, `level`, `position`, `setpoint`) has
reported, at most 5 s; a refusal shows its reason on the tile for 8 s (for `dp.refused` it names
the technology: the technology's own reason is in the log, `Technology::command()` returns only a
boolean). The tiles update from the WebSocket feed; a status with another image revision reloads
the model.

The **detail sheet** (a modal `<dialog>`: Escape and the backdrop close it, a bottom sheet on a
phone) lists the function's datapoints by role with their value, quality and age (live), a chart
over 24 h or 7 days for each datapoint that keeps a history (the line of the buckets' last values,
the min/max band, a read-out under the pointer), and the technology binding of each datapoint
(KNX: DPT, sending and listening addresses).

The dashboard reads `GET /api/model` once (paged), the live values from the WebSocket, the history
from `GET /api/history`; nothing about the image is parsed in the browser. Rejected: E-Home's
explicit visualization pages for now (the image's `ui` member stays reserved); a layout editor on
the device (engineering is E-Home's job).

### Texts

Every user-visible string goes through one lookup, `t(key)` in `web/ui/dist/i18n.js`; static HTML
marks its texts with `data-i18n` (`data-i18n-label` for an aria label). The base table is
`web/ui/dist/lang/en.json`, part of the UI bundle and loaded before anything renders; the device's
`ui.lang` (read from `GET /api/auth/session`, chosen on the Settings page) selects a language pack:
the one bundled with the UI (`/lang/<code>.json`, committed in `web/ui/lang/`, German first) and the
one the device downloaded from its release's update server (`/i18n/<code>.json`, preferred key by
key). Number and date formats follow the UI language. The API answers refusals with a stable `code`
that is a key of the same table and the English reason alongside, which the UI shows only for codes
it does not know. Pack strings are untrusted: only strings are taken, and they are only ever set as
`textContent` or attribute values, never parsed as HTML. The mechanism, what is translated and what
is not, and how to add a language are in `doc/i18n.md`.

### POSIX: the same API, the WebSocket on its own port

`homecontrol --http <port>` (default 8080, 0 = off) serves the API and `web/ui/dist` with
cpp-httplib (vendored in `third_party/cpp-httplib`, MIT), and `--demo` loads the demo datapoints.
cpp-httplib 0.61 has a WebSocket of its own, but its handler API does not let the core's feed own
the connection, and the raw socket cannot be taken over from a request handler. The POSIX build
therefore listens for the WebSocket on the HTTP port + 1 with a small server around the core's
`websocket` codec (handshake, `FrameDecoder`, ping/pong, close), a thread per connection;
`GET /api/auth/session` tells the UI the port (`ws_port`), and `GET /ws` on the HTTP port answers
426 with the port. The codec has host tests, so the framing the POSIX build relies on is tested
code, not glue.

### The model view

`GET /api/model` needs the locations and functions, which the loader validates but no component
keeps. `ImageService` builds an `image::ModelView` from the validated text after every successful
load (`ModelView::build()`, one arena; cleared by an unload): the labels and bindings are copied as
JSON text, the roles resolved to the model's datapoint ids. It stays in RAM while the image runs:
4 KB for the example house (one arena block, measured in the host test), roughly 100 bytes per
labelled datapoint with a KNX binding and per function, so about 40 KB for a house of 300
datapoints and 100 functions and up to some 150 KB at the image limits (1024 datapoints, 512
functions), which the C6 cannot afford beside everything else; the bindings are the largest part
and the first thing to drop (or to fetch on demand for the detail sheet) if real images come close.
When the heap is exhausted the image still runs and `GET /api/model` refuses with
`model.no_memory`.

## ESP specifics

- esp_http_server on port 80, 10 sockets (4 for the live feed's WebSocket clients, 6 for a
  browser's parallel page loads; 7 purged live-feed sockets during a page load, issue #45) with
  least-recently-used purging, a 4.5 KB task stack,
  wildcard URI matching. `CONFIG_HTTPD_WS_SUPPORT`, request headers up to 1 KB
  (`CONFIG_HTTPD_MAX_REQ_HDR_LEN`; browsers exceed the default 512 B), `CONFIG_LWIP_MAX_SOCKETS=16`.
- The httpd task never touches the core: requests, WebSocket opens and messages go through a static
  job slot to the event loop; closes and "writable again" are posted. Outgoing WebSocket messages are
  copied into a pool of 8 buffers of 1400 bytes and sent by work queued to the httpd task
  (`httpd_queue_work`); a send checks that the socket still belongs to the same connection (socket
  number plus a generation), a failed send closes the connection. The loop never blocks on a socket.
- The `web` console command: `web status`, `web token create|list|revoke`, `web password clear`,
  `web demo` (the demo datapoints until the next restart).
- Static RAM of this package on the C6 (`idf.py size`, .bss): +38 KB, of which the web server object
  (API, feed, auth) 3.6 KB, the send pool 11 KB, the response buffer 8 KB (files are sent in
  chunks from its front), the job slot 2.3 KB (the API body and the WebSocket text share it), the
  controller's log ring 4 KB. `GET /api/log` reads that ring in place under its busy flag (no copy;
  lines other tasks log meanwhile are dropped and counted in `dropped`); issue #44. The heap the server takes
  at run time (httpd task stack, sockets, lwIP buffers) is logged by the firmware: `free heap` lines
  before and after the start and on every WebSocket connect and disconnect (tag `hc.web`).
