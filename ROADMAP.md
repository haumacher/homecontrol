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
