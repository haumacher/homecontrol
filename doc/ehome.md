# The E-Home connection

*Operation of issue #41. The protocol (settings, enrollment, the socket, requests, status) is
specified in `spec/api.md` "E-Home connection"; this page says how the controller implements it,
what to expect in the log and on the Settings page, and what it costs on the device.*

## What it is for

E-Home runs on the public internet and cannot reach a controller behind a home router. The
controller therefore connects out to E-Home over a WebSocket (TLS) while its owner has switched the
connection on. Over it E-Home sees the live feed (status, datapoints, Zigbee, the monitor) like a
browser does and calls the local JSON API (deploy an image, read the Zigbee export, change
settings), one request at a time.

## Switching it on

1. In E-Home, add the controller to the installation: E-Home shows a one-time code (15 minutes).
2. On the controller's Settings page, card **E-Home**: enter the E-Home address (the form
   proposes the public installation `https://apps.haumacher.de/ehome/`; a trailing slash is
   dropped; another installation, e.g. `https://ehome.example`, replaces it) and the code, **Connect to E-Home**. The controller calls E-Home's
   enrollment, stores what E-Home answers (`ehome.url`, `ehome.id`, `ehome.name`, `ehome.secret`,
   `ehome.socket`) with `ehome.enabled` true and connects. A refusal (unknown or expired code,
   enrolled elsewhere, E-Home unreachable) appears under the form with its reason.
3. The switch on the card turns the connection off and on (`ehome.enabled`); off closes it at once.
   **Disconnect** (after a confirmation) erases the enrollment; connecting again needs a new code.

## The state machine

`EhomeLink` (core/include/hc/ehome_link.h) on the event loop, over the platform's `WebSocketClient`
(core/include/hc/platform/websocket_client.h) and `HttpFetcher` (its `post()`, for the enrollment):

| Phase | State shown | Leaves when |
|---|---|---|
| idle | `off` | enrolled and switched on |
| waiting | `connecting` | the network is up |
| opening | `connecting` | the socket opens (or fails: backoff) |
| challenge | `connecting` | E-Home's `challenge` arrives; the controller answers with `auth` |
| authenticating | `connecting` | `ready` (connected) or `refused` (failed, the reason shown) |
| connected | `connected` | the socket closes, a pong is late, the owner switches off |
| backoff | `failed` | the retry timer: 1 s, doubling to 60 s; reset by `ready` |

The handshake must complete within 30 s. Connected, the controller pings every 30 s and closes the
socket when the pong is 10 s late; the backoff reconnects. After `ready` the socket joins the
`LiveFeed` as one more client (an extra slot beside the four browser slots, `LiveFeed::attach_ehome`),
already authenticated; its commands show in the monitor with `"via": "ehome"`.

Requests go into `WebApi::handle` with `WebRequest::ehome` set: the "E-Home" principal has a
token's rights minus what is decided on site only. Refused 403 `ehome.forbidden` and logged:
onboarding, login, password (`/api/auth/*`), API tokens, WiFi, factory reset, the firmware upload,
the enrollment and disconnect (`/api/ehome*`), writing `update.channel` (or `update.url`) and any `ehome.*` setting.
Firmware never travels over this connection.

The enrollment's HTTP answer waits for E-Home's (`WebResponse::deferred`: the web servers poll
`WebApi::poll_deferred()` on the loop every 100 ms; the core gives up after 30 s with 502).

## Security

- The secret is a setting marked secret (`setting_is_secret`): never listed, never in a log line,
  never sent; only the MAC over E-Home's fresh nonce travels. The working copies are wiped after
  the MAC is computed. A test checks that no log line of a full exchange carries it.
- The socket must be `wss://` (the IDF certificate bundle checks E-Home's certificate). `ws://` is
  accepted only together with an `http://` E-Home address, for development.
- The connection exists only while the owner switches it on; E-Home cannot switch it on.

## TLS during development

A development E-Home behind a local TLS proxy (E-Home's `bin/dev-tls.sh`, its `doc/controllers.md`
"TLS during development") has a certificate of a local CA that the IDF bundle does not know. A build
with the CMake variable `HC_EXTRA_CA` (a PEM file of CA certificates; `platform/esp/hc_project.cmake`)
trusts it in addition to the bundle, through the IDF's `CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE`:

```
idf.py -B build-esp32c6-devtls -DHC_EXTRA_CA=$HOME/.config/ehome/dev-tls/ca.pem set-target esp32c6
idf.py -B build-esp32c6-devtls build
```

The configure step prints "Development build: trusts the CA certificates of …". Like the flash
size, the variable applies when the build directory's sdkconfig is created, and the configure step
refuses an sdkconfig that does not match it in either direction, so a build directory configured
without `HC_EXTRA_CA` never trusts a development CA. The extra trust covers every TLS client of the
application that uses the bundle (E-Home, the update check, MQTT, image downloads), not the
updater. A release is never built with it.

## Installation key

Signed runtime images (issue #35, spec/api.md "Signed runtime images"): E-Home holds an Ed25519 key
pair per installation and signs every image it deploys (plain Ed25519 over `"homecontrol-image-v1"
|| SHA-256(image)`).

- **Pairing.** The enrollment answer's `public_key` is stored as `ehome.key` (64 hex digits, masked
  like the secret, never listed; the status's `ehome.key` and the Settings page show its fingerprint,
  the first 8 bytes of SHA-256 over the key in hex groups: `ce7d 4a41 a476 9ee3`). The enrollment
  itself authenticates the key (the one-time code, TLS), so it is in force at once. The `auth`
  message names the fingerprint, so E-Home knows which key a device holds.
- **Enforcement** lives in the image service, not in the link: `ImageService::load()` refuses every
  image from the API, the socket, the console or a file without a valid signature while `ehome.key`
  is stored (`image.unsigned`, `image.bad_signature`, the expected key's fingerprint in the reason),
  after the size limit and before the loader; the boot load of the stored image is not checked
  again. Without a key nothing changes.
- **Rotation.** A `key` message over the authenticated socket makes the offered key *pending* (RAM
  only, `pending_key` in the status, the Settings page shows its fingerprint with "Accept key"); the
  old key stays in force until the owner accepts on the device (`POST /api/ehome/key/accept` with the
  fingerprint the page showed, or the console's `ehome key accept`). E-Home cannot accept for the
  owner: `/api/ehome*` is forbidden to its principal. The pending key goes when the socket ends and
  at an enrollment. The device confirms with `key_accepted`.
- **What clears the key.** Only the factory reset (erases it) and a new enrollment's answer
  (replaces it, or removes it when the answer carries no `public_key`; a failed enrollment keeps
  it). "Disconnect" (`DELETE /api/ehome`) erases the enrollment and a pending key but keeps
  `ehome.key` in force, so a token alone cannot forget E-Home and then push unsigned images; the
  Settings page keeps showing the key's fingerprint while not enrolled.

Console (`ehome`): `ehome status` (state, enrollment, key), `ehome key` (the key in force and an
offered one, as fingerprints), `ehome key accept`.

## In the log

Every state change is one line under the tag `hc.ehome`:

```
I hc.ehome: enrolling with https://ehome.example (the code from E-Home, entered by 192.0.2.10)
I hc.ehome: enrolled with https://ehome.example as controller c-17, installation "Haus"; the connection is switched on
I hc.ehome: connecting to wss://ehome.example/api/controllers/c-17/socket
I hc.ws: WebSocket open: client task stack 2500 of 5120 B unused, free heap 41000 B (minimum 30000 B)
I hc.ehome: socket open, awaiting E-Home's challenge
I hc.ehome: challenge answered as controller c-17
I hc.ehome: connected to E-Home (installation "Haus")
I hc.ehome: request 8: /api/image → 200
W hc.ehome: E-Home refused the connection: unknown controller (ehome.unknown); retry in 2 s (failure 2)
I hc.ehome: connection closed: switched off by the owner
I hc.ehome: disconnected from E-Home at https://ehome.example: the enrollment is erased
```

A failure is a warning the first time and info while it repeats unchanged (the backoff retries a
dead E-Home every minute). The `hc.ws` line is the ESP platform's: the client task's stack reserve
after the TLS handshake and the heap at that moment.

## The platforms

- **ESP** (`platform/esp/components/hc_ehome`): `EspWebSocketClient` on the managed component
  `espressif/esp_websocket_client` (1.8.x) with the certificate bundle; `EhomeService` owns it, an
  the `enrollment` lane of the one fetch worker (hc_external, doc/external.md "One fetch worker";
  the fetcher exists only during the POST, the
  answer streams into `EhomeLink`'s own 2 KB buffer, `kMaxEnrollAnswer`, refused beyond it)
  and the `EhomeLink`. The client task reassembles a text message (at most 4096 bytes) and hands
  binary pieces (1 KB, straight from the client's receive buffer) to the loop through the
  `LoopQueue`, waiting until the loop consumed each, so one slot carries everything in order.
  Sending (issue #55): the loop copies each message whole into the session's 6 KB send queue (a
  ring, a 4-byte header per message) or the send is refused (`send_room()`); the session's sender
  task `hc_ehome_tx` (priority 3, below the loop and the network tasks) drains it in order under
  the client's separate send lock (`CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK`) with a 10 s timeout (2 s before #66, which ended
  the session at a single lost TCP segment while a 32 KB image streamed to E-Home), a
  message that wraps around the ring's end as two fragments of one message, and posts
  `ws_client_writable()` after a message when the core asked (`want_writable()`). The write is
  what the loop must not wait for: on the C6 it costs about 6 ms of CPU per 1.3 KB message (the
  TLS record and the frame mask), and a message that finds lwIP's send buffer below its low-water
  mark (`TCP_SNDLOWAT`, 2881 of 5760 B) waits about 60 ms in `esp_transport_poll_write` for the
  server's ACK. The live feed's bulk messages (the snapshot's chunks, the monitor's batches) are
  admitted only while they leave `EhomeLink::kBulkReserve` (a whole answer and 256 B) free; an
  answer that finds no room is held in the response envelope and sent first on writable or the
  next tick (the feed waits behind it; after `kHoldMs`, 10 s, the link gives up on the
  connection), a streamed answer's next piece and a ping wait for room the same way. Closing hands
  the session to its sender task, which drops what is still queued, sends the close frame, stops
  the client task and frees everything, so the loop never waits for the network. Until that
  teardown has ended a new connection is refused (`the previous connection is still closing`, the
  core backs off and retries): E-Home replaces the old session of a controller that connects again
  (`ehome.replaced`), and two TLS sessions at once cost about 30 KB and fragmented the heap so that
  a 32 KB image no longer found a block (#66). A body whose frames stop arriving for
  `EhomeLink::kBodyStallMs` (5 s) is reported once with the bytes received (`request … the body
  stalls at N of M bytes`); the pong decides when the connection ends. Once, on the first deploy
  after a boot, a deploy's frames stopped right after the announcement: the client task delivered
  nothing more, pongs included, while the loop ran on; the cause is not known. The library's own
  ping and reconnect are off: the core pings and backs off.
- **POSIX** (`platform/posix`): `PosixWebSocketClient` on the core's WebSocket codec (client role)
  over a TCP socket with a reader thread, plain `ws://` only (there is no TLS in the host build;
  `wss://` is refused with that reason), enough to run against an E-Home on the developer's
  machine; the enrollment's POST through cpp-httplib (`HttplibFetcher::post`).

## Memory on the device

Nothing is allocated while the controller is not enrolled or the connection is off, beyond the
`EhomeLink` object (about 1.5 KB of static state). While a session runs:

| Item | Bytes |
|---|---|
| Response envelope (`EhomeLink`, heap per session) | 4194 |
| Session: the text reassembly buffer and the hand-over slot (heap) | about 4400 |
| Client receive and send buffers (`buffer_size` 1024 each) | 2048 |
| Client task stack (issue #45: 2612 B used against the development CA) | 5120 |
| Send queue (issue #55, heap per session) | 6144 |
| Sender task stack (issue #55: 1572 B used, a session with the snapshot) and its TCB | 3072 + about 350 |
| Client structure, configuration, its event loop, locks | about 1500 |
| TLS steady state with dynamic buffers (context, session, record buffers per record) | 3000 to 5000 |
| **Connected, total** | **about 29 500 to 31 500** |
| A request body announced with `length` (heap, freed after the answer) | its length; a `PUT /api/image` body goes to the staging area behind the updater's image instead (#66, doc/ota.md "The staging area": 0 B of heap, up to 148 KB on the C6), to the heap when the area declines (logged), 507 when no block is that large either |

The TLS handshake is the peak: with `CONFIG_MBEDTLS_DYNAMIC_BUFFER` the incoming record buffer is
allocated at the size of the record being read, the largest being E-Home's certificate message
(typically 3 to 6 KB for a chain of two or three certificates; at most 16.7 KB, the largest single
block the handshake can ask for), plus the parsed certificate chain (about 2 KB per certificate,
freed after the handshake since `CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE` is off), the ECDHE
context and the 4 KB outgoing record buffer while a record is written: about 12 to 18 KB above
the steady state for a second, with the largest block typically 6 KB and 17 KB in the worst case.
`CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA` frees the configuration's certificate data after the
handshake. These options are set for the whole application (`sdkconfig.defaults.app`): the OTA
check, the HTTP fetcher and MQTT over TLS allocate their record buffers per record too, instead of
16 KB + 4 KB for the whole of each connection.

The enrollment adds, for its duration only, the fetcher (3 KB with its 2 KB piece buffer), the
answer's 2 KB buffer, an 8 KB task stack and a TLS handshake.
