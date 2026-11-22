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

A bus session (spec/api.md "Bus session": E-Home programs devices through the controller's KNX
tunnel) logs its begin and its end, every refusal and every frame whose outcome is not `confirmed`:

```
I hc.ehome: bus session opened by E-Home: tunnel address 1.1.250
I hc.ehome: bus session: frame 11 negative confirmation
W hc.ehome: bus send 32 refused: the frame has 265 octets, more than the 264 forwarded (bus.bad_frame)
I hc.ehome: bus session ended (closed by E-Home): 40 frames sent, 38 received, 0 dropped, after 95 s
```

A failure is a warning the first time and info while it repeats unchanged (the backoff retries a
dead E-Home every minute); details of the moment the platform appends in brackets do not make it a
different failure. The `hc.ws` line is the ESP platform's: the client task's stack reserve after the
TLS handshake and the heap at that moment.

On the device the reason names the layer that ended the connection (homecontrol #112), after the
phase the core puts in front (`cannot connect` while opening, `closed during the authentication`,
`connection lost` once connected), and ends with the heap of that moment in brackets (HP: the
DMA-capable HP SRAM, LP: the LP SRAM, each with its largest free block; doc/diagnostics.md "Heap low
and failed allocations"):

```
W hc.ehome: connection lost: TLS ESP_ERR_MBEDTLS_SSL_WRITE_FAILED (mbedTLS -0x0001, errno 0) [heap 1840 B: HP 0 B (block 0 B), LP 1840 B (block 1840 B)]; retry in 1 s (failure 1)
W hc.ehome: cannot connect: TLS connect ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME (mbedTLS -0x0000, errno 0) [heap ...]; retry in 2 s (failure 2)
W hc.ehome: connection lost: closed by the server (1001: going away) [heap ...]; retry in 1 s (failure 1)
W hc.ehome: connection lost: socket: Connection reset by peer (errno 104) [heap ...]; retry in 1 s (failure 1)
W hc.ehome: connection lost: esp_transport_read() failed with -2, transport_error=ESP_OK, tls_error_code=0, tls_flags=0, errno=11 [heap ...]; retry in 1 s (failure 1)
W hc.ehome: connection lost: a message could not be sent within 10 s [heap ...]; retry in 1 s (failure 1)
```

- `TLS` / `TLS connect`: an esp-tls error after or before the connection opened, with mbedTLS's own
  code (`-0x7F00` is mbedTLS's allocation failure, `-0x0050` a connection reset, `-0x004E` a failed
  send; `-0x0001` is not mbedTLS's: on the C6 the AES engine's driver returns it when a DMA-capable
  allocation for a record's encryption fails) and the socket's errno. Before homecontrol #112 the
  line said `cannot connect: <error> (0x<code>)` in every phase, with the code's sign flipped:
  `0xffffffff` was `-0x0001`.
- `closed by the server (<code>: <reason>)`: E-Home or a proxy sent a close frame.
- `socket: ...`: the TCP socket failed with that errno.
- A read, poll or write that failed without an error code is told in the library's own words (its
  `WEBSOCKET_EVENT_ERROR`): `esp_transport_read() failed with -2 ... errno=11` is a frame or a TLS
  record whose rest did not arrive within the client's network timeout (10 s, `kNetworkTimeoutMs`),
  `esp_transport_poll_read() returned -1 ... errno=<n>` a socket error the poll saw. Without even
  that: `the transport failed without an error code (type <n>)`, `<n>` the library's error type (0:
  none, 1: the TCP transport, 3: the upgrade). Before homecontrol #112 all of these read `connection
  lost: connection lost`.

## The platforms

- **ESP** (`platform/esp/components/hc_ehome`): `EspWebSocketClient` on the managed component
  `espressif/esp_websocket_client` (1.8.x) with the certificate bundle; `EhomeService` owns it, an
  the `enrollment` lane of the one fetch worker (hc_external, doc/external.md "One fetch worker";
  the fetcher exists only during the POST, the
  answer streams into `EhomeLink`'s own 2 KB buffer, `kMaxEnrollAnswer`, refused beyond it)
  and the `EhomeLink`. The client task reassembles a text message (at most 4096 bytes) and hands
  binary pieces (1 KB, straight from the client's receive buffer) to the loop through the
  `LoopQueue`, waiting until the loop consumed each, so one slot carries everything in order.
  Sending (issue #55): the loop copies each message whole into the session's 7 KB send queue (a
  ring, a 4-byte header per message) or the send is refused (`send_room()`); the session's sender
  task `hc_ehome_tx` (priority 3, below the loop and the network tasks) drains it in order under
  the client's lock with a 10 s timeout for the lock and the socket (2 s before #66, which ended
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
  connection), a streamed answer's next piece and a ping wait for room the same way. No write is
  in flight when the transport is closed (#106: at a planned restart the WiFi stop broke the TCP
  connection, the client task closed the transport and freed the TLS session under the sender's
  write, a panic in `hc_ehome_tx`). The library closes the transport under its lock (after a
  failed read, write or connect, the lost network), at the end of its task without a lock, and in
  the teardown. The client's separate send lock (`CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK`, on since
  #41, when the loop itself sent) is off, so every send holds that lock; the end of the task
  (`WEBSOCKET_EVENT_FINISH`) waits for the sender's write in flight and lets none start after it;
  the teardown runs on the sender after its writes. The sender may now wait for the lock while
  the client task reads a frame and hands it to the loop; a send that gets neither the lock nor
  the socket within 10 s ends the session (`a message could not be sent within 10 s`). Closing
  hands the session to its sender task, which drops what is still queued, sends the close frame
  (within 2 s), stops the client task after its current read (in which the server's answer to the
  close frame usually arrives) and frees everything, so the loop never waits for the network. Until that
  teardown has ended a new connection is refused (`the previous connection is still closing`, the
  core backs off and retries): E-Home replaces the old session of a controller that connects again
  (`ehome.replaced`), and two TLS sessions at once cost about 30 KB and fragmented the heap so that
  a 32 KB image no longer found a block (#66). A body whose frames stop arriving for
  `EhomeLink::kBodyStallMs` (5 s) is reported once with the bytes received (`request … the body
  stalls at N of M bytes`); the pong decides when the connection ends. Once, on the first deploy
  after a boot, a deploy's frames stopped right after the announcement: the client task delivered
  nothing more, pongs included, while the loop ran on; the cause is not known. The library's own
  ping and reconnect are off: the core pings and backs off.
- **POSIX** (`platform/posix`, the Linux package, doc/linux.md): `PosixWebSocketClient` on the
  core's WebSocket codec (client role) over a TCP socket with a reader thread; `wss://` through
  OpenSSL (homecontrol #95: TLS 1.2 or newer, the server's certificate checked against the system's
  trust store, OpenSSL's default paths, and its name), `ws://` as before. A build without OpenSSL's
  development files refuses `wss://` with that reason; the package requires them. A development CA
  (E-Home's `bin/dev-tls.sh`) is trusted on Linux by adding it to the system's store
  (`/usr/local/share/ca-certificates/`, `update-ca-certificates`). The enrollment's POST through
  cpp-httplib (`HttplibFetcher::post`), which checks the certificate against the same store.

## Memory on the device

Nothing is allocated while the controller is not enrolled or the connection is off, beyond the
`EhomeLink` object (about 1.5 KB of static state). While a session runs:

| Item | Bytes |
|---|---|
| Response envelope (`EhomeLink`, heap per session) | 4194 |
| Session: the text reassembly buffer and the hand-over slot (heap) | about 4400 |
| Client receive and send buffers (`buffer_size` 1024 each) | 2048 |
| Client task stack (issue #45: 2612 B used against the development CA) | 5120 |
| Send queue (issue #55, heap per session; 6144 before homecontrol #92) | 7168 |
| Sender task stack (issue #55: 1572 B used, a session with the snapshot) and its TCB | 3072 + about 350 |
| Client structure, configuration, its event loop, locks | about 1500 |
| TLS steady state with dynamic buffers (context, session, record buffers per record) | 3000 to 5000 |
| **Connected, total** | **about 30 500 to 32 500** |
| A bus session (`BusSession`, heap from `open` until it ended and its last frame left the tunnel): 4 frames of 264 octets for the tunnel and the 2 KB outbox | about 3400 |
| A request body announced with `length` (heap, freed after the answer) | its length; a `PUT /api/image` body goes into the image slot as its frames arrive (issue #87: 32 bytes of the header on the heap, the flash erased and written per frame on the loop) |

The TLS handshake is the peak: with `CONFIG_MBEDTLS_DYNAMIC_BUFFER` the incoming record buffer is
allocated at the size of the record being read, the largest being E-Home's certificate message
(typically 3 to 6 KB for a chain of two or three certificates; at most 16.7 KB, the largest single
block the handshake can ask for), plus the parsed certificate chain (about 2 KB per certificate,
freed after the handshake since `CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE` is off), the ECDHE
context and the outgoing record buffer while a record is written: about 10 to 16 KB above the
steady state for a second, with the largest block typically 6 KB and 17 KB in the worst case.
`CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA` frees the configuration's certificate data after the
handshake. These options are set for the whole application (`sdkconfig.defaults.app`): the OTA
check, the HTTP fetcher and MQTT over TLS allocate their record buffers per record too, instead of
16 KB + 4 KB for the whole of each connection.

**The record buffers and the two kinds of RAM** (homecontrol #110, #112). With the dynamic buffers
every `mbedtls_ssl_write()` allocates a whole outgoing record buffer, `OUT_CONTENT_LEN` plus 341
bytes (header, IV, MAC, CBC padding), whatever it writes, and frees it afterwards; every received
record a buffer of the record's size (up to 16.7 KB: a server decides its records' size). The
client writes a frame's header and its payload in pieces of at most `kBufferSize` (1024 B), so
`CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN` is 2048 (`sdkconfig.defaults.app`): 2389 B per write instead of
4437 B with the IDF's 4096, for every TLS client of the application. The heap places such a buffer in
the DMA-capable HP SRAM while a block there is large enough, else in the 16 KB LP SRAM, which DMA
does not reach. The C6 encrypts every record (AES-GCM) with the AES engine over DMA: a record in LP
RAM is copied through two bounce buffers of up to 1600 B each, allocated in HP RAM for each record,
and a record in HP RAM needs a few dozen bytes of DMA descriptors. When that allocation fails the
driver returns -1 and the write fails with `ESP_ERR_MBEDTLS_SSL_WRITE_FAILED (mbedTLS -0x0001)`: the
E-Home session ended so about 21 s after every boot on the house controller, while the free heap's
minimum was at 2 to 4 KB (#112). A further defect of the IDF 5.5.6 driver: when the second bounce
buffer cannot be allocated the first is not freed (1600 B lost per failure;
`esp_aes_process_dma_ext_ram`; not reported to Espressif yet).

The enrollment adds, for its duration only, the fetcher (3 KB with its 2 KB piece buffer), the
answer's 2 KB buffer, an 8 KB task stack and a TLS handshake.
