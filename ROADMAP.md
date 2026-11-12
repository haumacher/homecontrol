# HomeControl — Direction and roadmap

*Adopted 2026-10-17. This document records where the project is going and why. The GitHub issue
tracker at `haumacher/homecontrol` holds the concrete work queue; each roadmap phase names the
issues that implement it through its `phase:*` label. Reversals of a decision recorded here are
made by editing this file in the same commit.*

## Where we are going

**HomeControl is the runtime of a smart home: a small always-on controller that executes what the
engineering tool E-Home deploys.** Devices on the KNX bus and Zigbee network do what devices do;
the controller does what they cannot: keep time and distribute it, run schedules and rules that
span devices and technologies, show the house to its inhabitants in a web UI, and bring in the
outside world (weather, external services).

1. **One portable core.** Datapoints, datapoint-type codecs, the runtime image, the scheduler,
   astronomy and the rule interpreter are plain C++17 with no platform includes. The same core runs
   on the ESP32-C6 and as a Linux executable (tests, simulation, a single-board-computer fallback).
2. **Everything is a datapoint.** KNX group objects, Zigbee cluster attributes, schedule outputs,
   weather values and rule parameters share one typed value model. Rules, the UI and logging see
   only datapoints; the technology adapters translate.
3. **Engineering in E-Home, operation on the device.** E-Home (TopLogic, Java, `../ehome`) owns the
   project: datapoints, device bindings, rules, schedules, visualization pages. It exports a
   versioned *runtime image* and pushes it to the controller, which validates it and swaps it in.
   The device web UI serves the inhabitants: live state, operation, and the parameters a rule
   declares adjustable (times, setpoints, enable switches). It is not an engineering tool.
4. **Declarative rules with one semantics in two places.** Rules are data (triggers, conditions,
   actions, timers, small state machines, a tiny expression language), not scripts. The C++ core
   interprets them on the device; a Java reference interpreter in E-Home simulates them against
   E-Home's virtual installation before deployment. A conformance corpus in `spec/` keeps the two
   interpreters identical.
5. **Zigbee is the goal of the chip choice.** The ESP32-C6 was chosen for its 802.15.4 radio; the
   controller becomes the Zigbee coordinator, and Zigbee devices appear as datapoints like KNX
   group objects. KNX over KNXnet/IP comes first because it is reachable from day one.

## Phases

- **Phase 0 — Foundation** (`phase:0-foundation`): repository skeleton with the portable core,
  POSIX build and ESP-IDF build; WiFi, mDNS, SNTP; OTA with asymmetric partitions; diagnostics;
  the web server base.
- **Phase 1 — KNX** (`phase:1-knx`): the datapoint model and DPT codecs; KNXnet/IP tunneling; the
  runtime image format and its loader; the E-Home push.
- **Phase 2 — Time** (`phase:2-time`): time service with time zone and DST, KNX time master,
  sunrise and sunset; schedules as rule triggers.
- **Phase 3 — Rules** (`phase:3-rules`): the rule model and expression language; the C++
  interpreter; the conformance corpus; the Java counterpart in E-Home.
- **Phase 4 — Visualization** (`phase:4-visualization`): the device web UI generated from the
  runtime image, live values over WebSocket, controls and adjustable parameters.
- **Phase 5 — Zigbee** (`phase:5-zigbee`): coordinator role, commissioning, device database,
  cluster-to-datapoint mapping, the first device classes, bindings maintained in E-Home.
- **Phase 6 — Services and interoperability** (`phase:6-services`): weather, MQTT (Home Assistant
  discovery, history export), the monitor stream for E-Home's group monitor.
- **Cross-cutting** (`cross-cutting`): security, state persistence and history, hardware.

## Decisions log

- **2026-10-17 — Hardware: ESP32-C6 alone, WiFi, KNX through the existing KNXnet/IP router.**
  Alternatives considered: ESP32-P4 plus C6 co-processor (more RAM, Ethernet) and a thin C6 bridge
  with the logic on a Linux box in Java. Rejected for now: the point is an embedded controller;
  the portable core keeps both doors open.
- **2026-10-17 — Language: C++17 on ESP-IDF.** The Zigbee SDK is a C library and the mature open
  KNX stack for ESP32 (thelsing/knx, OpenKNX) is C++. Rust (esp-rs) would mean binding both by
  hand.
- **2026-10-17 — Portable core with a Linux build from the start.** Unit tests, CI and
  simulation run on the host; ESP-IDF's Linux target is the fallback if the C6 runs out of room.
- **2026-10-17 — All engineering happens in E-Home, at least for now.** The device executes; its
  UI exposes only what rules declare adjustable.
- **2026-10-17 — Rule interpreter twice, semantics once.** A Java reference interpreter in E-Home
  for simulation, a shared conformance corpus in `spec/`. Accepted cost: roughly double the rule
  engine work.
- **2026-10-17 — OTA with asymmetric partitions.** One small *updater* app partition and one large
  *application* partition, instead of two equal OTA slots. The application checks for an update,
  then boots into the updater; the updater overwrites the application partition and boots back.
  This maximizes the space for the application on a 4 or 8 MB module.
- **2026-10-17 — One controller per house.** Cooperation between controllers may come later; no
  design effort is spent on it now.
- **2026-10-17 — Zigbee integration is the goal; no version is scheduled yet.**
- **2026-10-17 — The GitHub issue tracker is the work queue; direction lives here.**
- **2026-10-17 — The name stays HomeControl.** Generic, but it says what the project is and pairs
  with E-Home; "homedriver" (the local checkout's directory name) was the alternative considered,
  a rename would be a single commit plus the GitHub repository and must precede the first release.
- **2026-10-17 — Flash at 40 MHz, transmit power 11 dBm on the development board.** The C6's
  in-package flash misreads at 80 MHz and the USB supply browns out at 20 dBm; both are
  `sdkconfig.defaults` settings a production board may raise (doc/hardware.md).
- **2026-10-17 — KNXnet/IP tunnelling implemented in the core, not through thelsing/knx.** Small
  protocol, testable under the fake clock, no device stack needed; routing is a later transport.
- **2026-10-17 — The runtime image is JSON.** Readable and diffable, parsed on the device with the
  core's own reader; a binary form can come later behind the same loader (spec/runtime-image.md).
- **2026-10-17 — A datapoint has an owner and, optionally, a transmitter.** An application component
  (the time master, published values) owns and publishes; a technology transmits. Chosen over an
  adapter-only flag because the shape recurs (doc/datapoints.md).
- **2026-10-17 — The model is composed from the image plus dynamic spec providers.** Zigbee devices
  join at run time; the image service reloads the model keeping values and re-binds every component
  without restarting the rules (#29, doc/image.md).
- **2026-10-17 — Plain HTTP on the LAN, tokens for machines, signatures later.** Recorded on #20.
- **2026-10-17 — Zigbee with Espressif's ZBOSS line (esp-zigbee-lib 1.6.x) on ESP-IDF 5.3.** No IDF
  upgrade; three small partitions for the stack; coexistence with WiFi on channels 15/20/25.
- **2026-10-17 — The 4 MB module is for development only.** With Zigbee the application fills 76 %
  of its partition and the heap sits near 90 KB; the production board is an 8 or 16 MB module or
  the P4 + C6 pairing (#22).
- **2026-10-18 — The monitor stream is the controller's view for E-Home's group monitor.** A ring of
  128 events (KNX, Zigbee, rule writes, commands with their origin, image loads) over the WebSocket
  and `GET /api/monitor`, with sending and reading on KNX from the monitor; E-Home attaches to it
  instead of a tunnel of its own (ehome #41).
- **2026-10-18 — I18N as on the phoneblock dongle, with the base language in the UI bundle.** Keys
  everywhere, English bundled, German bundled and complete, further languages as packs per release
  from the update server; a regional tag falls back to its base language (doc/i18n.md).
- **2026-10-18 — WiFi power save stays off; `wifi.ps` is a per-household setting.** Measured on the
  C6 against a consumer access point: minimum modem sleep stalls the link for seconds and the KNX
  tunnel misses status telegrams, so the Zigbee coexistence gain (Espressif's recommendation) does
  not justify it as the default (doc/zigbee.md, the table). Revisited with a paired device (#15).
- **2026-10-18 — Reversed the same day: WiFi power save `min` is the default.** The first paired
  device (a Hue motion sensor) lost its parent within seconds under `none`: bindings and reads timed
  out, it rejoined twice and signalled "no network"; under `min` it stayed and reported, and the KNX
  status reply measured 217 to 357 ms. A lost device outweighs latency; `none` remains the choice
  for households without Zigbee (doc/zigbee.md, doc/network.md).
- **2026-10-18 — MQTT is the integration path to Home Assistant and to long-term storage.** States,
  commands, discovery configs per function and the history rings' export over the broker the image
  names; the broker password is a device setting (never in the image, write-only on the Settings
  page). Chosen over a native Home Assistant integration (nothing to install on the HA side) and
  over an InfluxDB client on the device (one outbound protocol, the collector is the subscriber's
  choice); spec/mqtt.md, doc/mqtt.md.
- **2026-10-18 — The 4 MB C6 is the minimum requirement, not a development-only board.** The author
  overrules the entry of 2026-10-17: the controller must keep fitting the 4 MB layout and the C6's
  RAM; larger modules only gain storage (doc/hardware.md, #22).
- **2026-10-18 — The security story of #20 is confirmed for the first versions.** Plain HTTP on the
  LAN, PBKDF2 device password, hashed revocable tokens, SHA-256 in the update manifest, no secure
  boot or flash encryption; Ed25519 key pairing with signed images and manifests is #35.
- **2026-10-18 — The rule-specification questions are decided (#25–#28, #33, #34).** Confirmed as
  specified: a condition without value runs neither branch, the cascade loop guard of 32 (the log
  names the culprit), inclusive `between` and half-open `inWindow`, no time in `match`, timers
  100 ms..30 d, disabling cancels timers, `start` fires for every rule at every swap. Changed: a
  parameter set to its default stores no override; rule states survive a reboot (coalesced writes),
  timers do not; `dateTime` arithmetic is exact elapsed time (UTC, converted back); the canonical
  text is defined on normalized trees and the AST stays numeric for enumerations (E-Home prints the
  names); rule parameters are mirrored as datapoints `rule/<ruleId>/<parameterId>`; labels are a
  string or a language map; a `read` action and a `silence` trigger are added; every limit the core
  enforces is named in the spec; rule text names datapoints, never function roles.
- **2026-10-18 — Function roles follow the KNX inputs E-Home binds.** A dimmer gets the optional
  `step` role for relative dimming (DPT 3.007); the device sends the scene code `scene − 1` (#30,
  #31, from ehome #50).
- **2026-10-18 — E-Home is a service on the public internet; the controller connects to it, not
  the other way round (#41).** The author: "ehome can never reach a device in the private LAN of an
  installation. The device must actively connect to ehome to push detected zigbee devices, or request
  a new version of the project layout. We think of ehome as a SaaS software provided for managing
  multiple installations." Decided: an outbound WebSocket over TLS carrying the existing live feed
  and a request envelope for the existing API, authenticated with a per-controller secret obtained
  through an enrollment code E-Home shows for the project; the connection is not permanent: the owner
  switches it on from the device's Settings page for configuration and deployment and off again;
  firmware never travels over it ("a CDN is a much better solution for that - see the phoneblock
  dongle": the release server of `update.url`, plus the browser upload). spec/api.md "E-Home
  connection". The device's local UI and API stay the installer's on-site tools.
- **2026-10-18 — Push updates start with the upload page in the updater app (#39).** The author:
  start with the upload page in the updater app; there is no E-Home installation with a stable URL
  yet. The application stores an *await upload* instruction with a one-time token and restarts into
  the updater, which serves `PUT /firmware` on the device's address and boots the old application
  after 10 minutes without an upload (doc/ota.md "Upload from the browser"). E-Home as the update
  server stays possible later through the existing manifest pull.
- **2026-10-18 — Signed runtime images and update manifests: Ed25519 on vendored Monocypher (#35).**
  The installation's public key comes with the E-Home enrollment answer (trusted by the one-time code
  and TLS) and is stored as `ehome.key`; a rotation offered over the socket stays pending until the
  owner accepts its fingerprint on the device; only the factory reset clears it and only a new
  enrollment's answer replaces it, while forgetting E-Home keeps it (else a token alone could forget
  E-Home and then push unsigned images). A device with
  a key refuses every unsigned or mis-signed image (API, socket, console, file) before anything is
  parsed or written. The image construction is plain Ed25519 over `"homecontrol-image-v1" ||
  SHA-256(image)`: the device hashes the bytes it already holds and verifies a fixed 52-byte message,
  every platform (JDK `Ed25519`, Python, OpenSSL) signs it without a pre-hash mode, and the prefix
  keeps an image signature from meaning anything else; rejected: Ed25519ph (not in PyNaCl or
  `cryptography`, and SHA-512 streaming buys nothing when the image is in RAM anyway) and plain
  Ed25519 over the bare digest (no domain separation). The OTA manifest carries a detached signature
  (`manifest.sig`, over the exact bytes served) by the release key, whose public half the firmware
  carries (`platform/esp/release-key.pub`, `HC_RELEASE_KEY` for development); the binary stays bound
  by the manifest's SHA-256, the updater unchanged. Ed25519 is Monocypher 4.0.2 (BSD-2/CC0, audited,
  two C files, verify and sign) in `third_party/`: the core keeps no mbedTLS. spec/api.md "Signed
  runtime images", doc/ota.md "Manifest signature".
- **2026-11-03 — ESP-IDF 5.5.6 instead of 5.3 (#49).** Supersedes "No IDF upgrade" of 2026-10-17.
  IDF 5.3's WebSocket transport keeps the bytes that arrive with the handshake answer in its own
  buffer, but `ws_poll_read` asks only the socket, so a challenge E-Home sends right after the 101
  waited for the next TCP segment and the authentication timed out on every reconnect. The fix is
  upstream in 5.5.5 and later (no 5.3.x or 5.4.x has it); upgrading beats patching the IDF. The
  Zigbee SDK stays on the 1.6 line (ZBOSS), which declares `idf >= 5.0` and builds unchanged; the
  application grew by 38 KB (doc/ota.md).
- **2026-11-03 — Adjustable schedule members (#58).** The image lists the members of a schedule
  entry the inhabitant may change (`adjustable`) with optional bounds and a step (`bounds`); the
  device keeps the values as overrides by schedule id and member, kept by a new image while they
  fit it, like the rule parameters. A change is no gap and a handled day stays handled. The
  adjustable time is also the datapoint `schedule/<id>/time`, so control pages show it and rules
  read it. Alternatives: a rule parameter compared with `time()` every minute (no DST handling, no
  missed firings, invisible in the schedule view); everything adjustable (E-Home could not keep an
  installation's timing). They are members of schema 1.2 (with the history archives of #56): a 1.1
  firmware refuses an image that uses them, and a 1.2 firmware names the versions when a newer image
  meets its strict objects; the device reports the newest schema it reads (`schema`), so E-Home
  warns before a deploy.
- **2026-11-03 — Loaded blocks as WebAssembly: go, with the WAMR interpreter (recommendation of
  the spike #59 for the author; not yet adopted).** Measured on the 4 MB C6 (doc/blocks-wasm.md).
  - Recommended: WAMR 2.4.5, unpatched, with the classic interpreter. Its flash cost is 66.6 KB of
    the about 260 KB free in ota_0.
  - Every call has a budget through instruction metering. An endless loop stops with its reason
    and the controller keeps running.
  - Every trap tested (out of bounds, stack overflow, `unreachable`, division by zero) stops only
    its block.
  - Steady state allocates nothing.
  - A module is shrunk to its data plus its stack (custom page sizes are supported by neither WAMR
    nor Chicory). There is one module instance per block type, and its blocks are state records
    in it: 8 room controllers cost 12 KB.
  - The same `.wasm` from C, Rust or Zig gives bit-identical outputs on the device, on the host and
    in Chicory (pure Java, for E-Home's simulation).
  - Rejected: AOT. It is faster, but it cannot be stopped in a loop (no metering, terminate
    ignored), and it needs an executable flash region in the partition table.
  - Alternative: the fast interpreter, at about 25 % less time per call, with 4 times slower loads
    and more RAM for larger modules.
  - Conditions:
    - the event loop becomes a pthread, which WAMR requires on ESP-IDF;
    - the native stack boundary is set once (without it every call costs 200 µs more);
    - a fault stops every block of its type, and restart instantiates again.
  - Limit: heap, the same on every C6 board. About 30 KB for loaded blocks of the controller's
    110 KB free: two or three small types; a 20 KB module costs about 38 KB.
  - The 4 MB board is not ruled out, and an 8 MB board would not add RAM.
- **2026-11-03 — Blocks: the contract (issue #60, `spec/blocks.md`).** Virtual devices with typed
  ports, parameters and a state record, run inside the rules' cascades after the rules, built-in C++
  types first (the room controller 1.0), WebAssembly types later against the same interface (the
  five calls, the state record selected by `self`). Image schema 1.3 with E-Home as the guard (a
  1.2 firmware would ignore `blocks`); one namespace `block/<id>/<x>`; aggregation per instance;
  one record per instance paced to 30 writes an hour; a corpus run by the core and E-Home's
  simulation, its expected values from an independent model of the specification.
- **2026-11-04 — Commands while a technology is not connected are held, not refused (#72).** The
  KNX adapter (tunnel down) and the Zigbee adapter (network not up) accept a command, the model
  stores it and marks the datapoint `pending`; one value per datapoint, no queue. On connect the
  held values go out once, in datapoint order, before the reads on start; a report before that
  drops the held value (the bus is newer). An image swap keeps the mark with the value by name.
  Refusals for other reasons stay (not writable, no conversion, not encodable, a device that has
  not joined). Not held: the KNX time master's transmit-only datapoints (an old clock), and writes
  lost with a tunnel that was up (unchanged: not replayed).
- **2026-11-04 — "Stay logged in": remembered sessions survive reboots (#53).** Reverses the
  2026-10 decision in `doc/web.md` "Sessions" not to persist sessions: logging in again after every
  firmware upload and deployment is a burden without a security gain on a home network. A login or
  onboarding with "stay logged in" (unchecked by default) gets a cookie with a 10-year `Max-Age`
  and one of 4 slots of its own, stored as the SHA-256 of the session id in `web.sess.0` ..
  `web.sess.3` (`<creation order>$<hash hex>`), valid until logout with no idle expiry and no
  wall-clock dependency; a fifth replaces the oldest. Flash wear stays bounded because the settings
  are written only when such a session is created or ended, never on use. The reboot as the way to
  log everybody out is replaced by an explicit "log out everywhere" (Settings, System; `POST
  /api/auth/sessions/clear`; console `web session clear`), which ends the remembered sessions too,
  as does `web password clear`. Plain sessions are unchanged (RAM, 24 h idle, end at reboot).
- **2026-11-04 — Releases: built and signed in GitHub Actions on a tag, served by GitHub Pages
  (`doc/ota.md` "Releases and the web flasher").** Pushing the tag `vX.Y.Z` = `hc::kVersion` runs
  `release.yml`: the host tests, the updater and the application for esp32c6 from fresh build
  directories (4 MB table, the committed release key, no development CA), the assets
  (`scripts/release-assets.py`), the update manifest signed with the release key and verified
  against `platform/esp/release-key.pub`, the GitHub release, then the Pages site. The key is the
  secret `HC_RELEASE_KEY` of the environment `release`, which admits only tags `v*`, used by the
  signing job alone. This replaces the first decision of the same day (sign only on the owner's
  machine, which published v1.0.0): E-Home's apt signing key already lives in CI the same way, a
  tag-only environment protects the key as well, and the binaries now have a public build log.
  `scripts/release.sh` stays as the local check of the same build and signature and publishes
  nothing. The site https://haumacher.github.io/homecontrol/ holds the web flasher (English,
  German), `stable/` as the update source (`update.url`) and `firmware/<version>/` for the newest
  five releases (the language packs of all); rollback by pointing `stable` at an older release by
  hand. The web flasher writes everything but NVS, the core dump and the Zigbee partitions,
  including the empty storage image; it asks before erasing (unticked by default) a board that does
  not answer Improv as HomeControl and updates one that does without erasing, so a reinstall keeps
  WiFi, password, enrollment and the Zigbee network but loses the deployed image, the values and the
  history. Only the 4 MB layout is offered.
- **2026-11-07 — Defaults for the update source and the E-Home address; 4 KB TLS records where the
  server allows.** A controller without a stored `update.url` updates from the GitHub releases as
  the release workflow publishes them on the Pages site (`https://haumacher.github.io/homecontrol/stable`,
  `hc::setting::kDefaultUpdateUrl`); `off` switches the source off, `update url clear` returns to the
  default. Pages rather than github.com's release downloads: those answer with two redirects to URLs
  longer than an image URL may be, have no `<version>/i18n/` beside the manifest, and `stable` could
  no longer be pointed back. The E-Home card proposes `https://apps.haumacher.de/ehome/`. Every TLS
  client asks for 4 KB records (Max Fragment Length, as the PhoneBlock dongle does); E-Home's server
  and github.com honour it, GitHub Pages and the release assets send 16 KB records regardless, so the
  receive buffer stays at 16 KB (doc/ota.md "TLS records").
- **2026-11-07 — The update source is a channel, not a URL (the author: "provide a selector for
  stable or beta - no update url input").** Replaces the default update.url of the same day: a
  controller that had `…/homecontrol/stable.` stored (copied with a full stop) fetched 404s. Settings
  offers the update channel `stable` (default), `beta` (the newest release, pre-releases included;
  the Pages site publishes `beta/` beside `stable/`) or `off`; the source is
  `https://haumacher.github.io/homecontrol/<channel>`. update.url stays only as the console's
  development server; an update.url of an older firmware into the release site (or `off`, empty)
  becomes the channel at the next start. "Check" and "Update now" show their outcome on the card
  instead of "the result shows in the log".
- **2026-11-11 — Image schema 1.6: a function binds what its devices report; `writable` is the
  user's (haumacher/ehome #134, #136, #138, the author's decisions of the day).** A blind takes the
  1-bit end positions `atTop` and `atBottom` beside a percentage `position`: with a position the
  device shows it and uses the end bits only as a cross-check, without one it shows open / closed /
  in between and never a percentage. A climate takes the actuating value as `heatingLevel` (percent,
  flames in three steps) and as `heating` (a number heats while above 0). A dimmer's main role is
  its switching `command`, `levelCommand` and `step` are additional; it needs one of the two.
  Rules write every value of the application (`virtual`, or no technology member), writable or not;
  `writable` says only whether the user may change it (spec/rules.md decision 16). Compatibility:
  the new roles are unknown to a 1.5 firmware and ignored (I5); a dimmer without `levelCommand`, a
  heating of a number and a rule writing a non-writable application datapoint are refused by it, so
  E-Home deploys an image with one of those only to a controller reporting 1.6 (I4), and an image
  without them still loads on 1.5. Alternatives: `position` taking a 1-bit address (0 → 0 %, 1 →
  100 %) — rejected, a single bit cannot say "in between"; a separate "rule-writable" flag in the
  image — rejected, the owner's rule is simpler: every application value is the rules'.
- **2026-11-12 — E-Home programs through the controller's tunnel: a bus session forwards frames
  (homecontrol #85, haumacher/ehome#148, the specification round of the day).** The controller
  forwards cEMI frames between E-Home and its KNX tunnel (`{"type": "bus"}` messages on the E-Home
  connection, spec/api.md "Bus session"); transport connections and load procedures stay in E-Home.
  One session at a time, ended by E-Home, the connection, the tunnel going down or 60 s idle; the
  session's frames share the tunnel's queue with the controller's own telegrams; frames are forwarded
  as they are up to an APDU of 254 octets (the UDP datagram limit grew to 320 octets for it); the
  session's 3.4 KB are heap while it runs. The capability is the presence of `ehome.bus` in the
  status. Not on the local `/ws`. Rejected: the controller executing E-Home's download plan; a device
  setting allowing it; the monitor's `send` to individual addresses.
