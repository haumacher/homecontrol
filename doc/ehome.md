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
2. On the controller's Settings page, card **E-Home**: enter the E-Home address
   (`https://ehome.example`) and the code, **Connect to E-Home**. The controller calls E-Home's
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
the enrollment and disconnect (`/api/ehome*`), writing `update.url` and any `ehome.*` setting.
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

## In the log

Every state change is one line under the tag `hc.ehome`:

```
I hc.ehome: enrolling with https://ehome.example (the code from E-Home, entered by 192.0.2.10)
I hc.ehome: enrolled with https://ehome.example as controller c-17, installation "Haus"; the connection is switched on
I hc.ehome: connecting to wss://ehome.example/api/controllers/c-17/socket
I hc.ws: WebSocket open: client task stack 1800 of 6144 B unused, free heap 41000 B (minimum 30000 B)
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
  `OnDemandFetcher` for the enrollment (hc_external; the fetcher exists only during the POST, the
  answer streams into `EhomeLink`'s own 2 KB buffer, `kMaxEnrollAnswer`, refused beyond it)
  and the `EhomeLink`. The client task reassembles a text message (at most 4096 bytes) and hands
  binary pieces (1 KB, straight from the client's receive buffer) to the loop through the
  `LoopQueue`, waiting until the loop consumed each, so one slot carries everything in order. The
  loop sends under the client's separate send lock (`CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK`) with
  a 2 s timeout. Closing hands the session to a short-lived task that sends the close frame, stops
  the client task and frees everything, so the loop never waits for the network. The library's own
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
| Client task stack | 6144 |
| Client structure, configuration, its event loop, locks | about 1500 |
| TLS steady state with dynamic buffers (context, session, record buffers per record) | 3000 to 5000 |
| **Connected, total** | **about 21 000 to 23 000** |
| A request body announced with `length` (heap, freed after the answer) | its length (the image: up to 256 KB, 507 when no block is that large) |

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
