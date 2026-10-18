# KNX over KNXnet/IP

How the controller talks to the KNX bus (issue #7): a KNXnet/IP tunnelling connection on the link
layer to the installation's IP gateway, the KNX adapter that translates group telegrams into
datapoints (`doc/datapoints.md`), and the telegram log. The protocol is implemented in the portable
core (namespace `hc::knx`), tested under the fake clock against a fake gateway; the platforms only
move UDP datagrams.

| Piece | Where |
|---|---|
| Group and individual addresses | `core/include/hc/knx/address.h` |
| cEMI L_Data frames, group services | `core/include/hc/knx/cemi.h` |
| KNXnet/IP frames (header, HPAI, CRI/CRD, connect, heartbeat, disconnect, tunnelling, search response) | `core/include/hc/knx/knxnetip.h` |
| The tunnel's state machine | `core/include/hc/knx/tunnel.h` |
| The adapter (group addresses to datapoints) | `core/include/hc/knx/adapter.h` |
| The telegram log | `core/include/hc/knx/telegram_log.h` |
| The time master (DPT 10, 11, 19 broadcasts) | `core/include/hc/knx/time_master.h` |
| Group values: encode/decode with a DPT, the text forms of the console | `core/include/hc/knx/group_value.h` |
| The settings `knx.gateway`, `knx.nat` | `core/include/hc/knx/knx_settings.h` |
| The UDP socket interface | `core/include/hc/platform/udp_socket.h` |
| BSD socket calls shared by POSIX and lwIP | `platform/common/src/bsd_udp.cpp` |
| POSIX socket, `homecontrol` wiring, `knxtool` | `platform/posix/src/udp_socket.cpp`, `src/main.cpp`, `tools/knxtool.cpp` |
| ESP socket (reader task), `KnxService`, the `knx` console command | `platform/esp/components/hc_knx` |

## Decision: our own tunnelling client

The controller implements KNXnet/IP tunnelling itself, not through the thelsing/knx library. The
client side of tunnelling is small (connect, heartbeat, tunnelling request/acknowledgement with
sequence counters, disconnect), the core must run and be tested under the fake clock on the host,
and the library carries a complete KNX device stack (group objects, properties, programming mode)
the controller does not need. Routing (multicast 224.0.23.12) is not implemented: it needs no
connection but is unreliable over WiFi and makes the controller a bus participant without
acknowledgements. The cEMI layer is shared, so a routing transport can be added beside the tunnel
later (a follow-up issue).

## The tunnel

`KnxTunnel` has three observable states, `down`, `connecting` and `up`, with listeners
(`KnxTunnelListener`); every transition is logged under the tag `hc.knx` with its reason.

```
            start, gateway set, network up
   down ─────────────────────────────────────▶ connecting ── CONNECT_RESPONSE (E_NO_ERROR) ──▶ up
    ▲  ▲                                          │                                          │
    │  └── retry after the backoff ◀── refused, ──┘                                          │
    │                                  10 s timeout, resolve failed                           │
    └──────── 3 heartbeats unanswered, heartbeat error, no TUNNELING_ACK after a repetition, ─┘
              the gateway's DISCONNECT_REQUEST  (then: reconnect after the backoff)
              network down, stop(), gateway changed or cleared  (no reconnect until the
              network is back, start(), or at once to the new gateway)
```

- **Preconditions.** The tunnel connects only while it is started, a gateway is set and
  `Connectivity` reports the network up. It is a `ConnectivityListener`: when the link goes down it
  tears the tunnel down (no DISCONNECT_REQUEST: it would not get out) and connects again as soon as
  the link is back. While it waits it logs why once (`knx tunnel waiting: network not up`).
- **Connecting.** A host name is resolved first (`UdpSocket::resolve()`, never on the loop's thread
  on the ESP; an IPv4 literal needs none). Then the socket is opened on an ephemeral port and a
  CONNECT_REQUEST goes to the gateway, with the local endpoint (the address of the interface towards
  the gateway) as control and data endpoint, or 0.0.0.0:0 in NAT mode. The CRI asks for a tunnel
  connection on the link layer (04 04 02 00). Resolving and connecting together have 10 s
  (`kConnectTimeoutMs`). The CONNECT_RESPONSE assigns the channel, the tunnel's individual address
  (the CRD) and the gateway's data endpoint; one of 0.0.0.0:0 (NAT) means "where the response came
  from".
- **Backoff.** A failed attempt or a broken connection is retried after 1 s, 2 s, 4 s, ... 60 s
  (`hc::Backoff`); the backoff is reset when the tunnel comes up and when the network comes up.
  Every attempt opens a new socket (a new port), so nothing of the old connection arrives.
- **Heartbeat.** Every 60 s (`kHeartbeatPeriodMs`) a CONNECTIONSTATE_REQUEST; without a response
  within 10 s (`kHeartbeatTimeoutMs`) it is repeated at once; the third unanswered one in a row, or
  a response with an error status (E_CONNECTION_ID, E_DATA_CONNECTION, E_KNX_CONNECTION), ends the
  connection: DISCONNECT_REQUEST, then reconnect.
- **Sending.** `send()` queues an L_Data.req (at most 32, `kQueueCapacity`; refused while the tunnel
  is not up) and returns a token. One TUNNELING_REQUEST is in flight at a time, with the send
  sequence counter (0 after every connect, wrapping at 256). Without a TUNNELING_ACK with that
  sequence within 1 s (`kAckTimeoutMs`) the same request is repeated once; a second miss ends the
  connection. An ACK with an error status counts as none. After the ACK the next frame waits for
  the gateway's L_Data.con of this one (at most 3 s, `kConfirmTimeoutMs`): this paces the queue to
  the bus (TP1 carries some 20 to 50 telegrams per second) and gives the sender the outcome
  (`tunnel_sent()`: `confirmed`, `negative` — the con's error bit —, `no confirmation`). A
  confirmation that overtakes its ACK is kept until the ACK arrives. Frames still queued when the
  connection ends are reported `lost` and never sent later: a light switched a minute ago must not
  switch when the gateway comes back.
- **Receiving.** A TUNNELING_REQUEST on our channel with the expected receive sequence is
  acknowledged and delivered (`tunnel_frame()` for an L_Data.ind, the confirmation matching for an
  L_Data.con); the sequence advances. One with the previous sequence is the gateway repeating a
  request whose ACK it missed: acknowledged again, not delivered. Any other sequence is ignored
  without an ACK (logged). A request whose cEMI frame does not decode is acknowledged (so the
  gateway does not repeat it) and dropped with a log line. Datagrams from another address than the
  gateway, for another channel or of other services are ignored.
- **Closing.** The gateway's DISCONNECT_REQUEST is answered with a DISCONNECT_RESPONSE and the tunnel
  reconnects. `stop()` sends a DISCONNECT_REQUEST and stays down (`homecontrol` and `knxtool` stop
  this way, so the gateway frees its tunnel slot at once). `set_gateway()` closes the connection to
  the old gateway and connects to the new one.

| Timer | Value |
|---|---|
| Connect (resolve + CONNECT_RESPONSE) | 10 s |
| Heartbeat period | 60 s |
| Heartbeat response | 10 s, 3 attempts |
| TUNNELING_ACK | 1 s, 1 repetition |
| L_Data.con | 3 s |
| Reconnect backoff | 1 s doubling to 60 s |
| Read-on-start spacing (adapter) | 100 ms |

The tunnel uses up to six event-loop timers, the adapter one; the tunnel takes one of
`Connectivity`'s four listener slots.

## Frames

cEMI L_Data frames (`cemi.h`) are encoded without additional information, as a standard frame for
up to 15 octets after the TPCI (an extended frame above), with priority low, "repeat on error"
(control field 1 bit 5 set), broadcast, no acknowledge request, group address type and hop count 6:
the switch telegram "on" to 1/0/1 is `11 00 BC E0 00 00 08 01 01 00 81`. Received frames keep the
fields E-Home's group monitor shows (`BusTelegram`): source, destination, priority, repetition, hop
count. The group services are A_GroupValue_Read (APCI 0x000), A_GroupValue_Response (0x040) and
A_GroupValue_Write (0x080); a value of the DPTs whose `Dpt::embedded()` is true (main types 1, 2, 3)
travels in the low 6 bits of the APCI octet (a TPDU of 2 octets), every other value in the octets
after it. A frame to an individual address, another transport or application service, or a read
carrying data is "other": decoded as a frame, its payload not interpreted. Refusals speak:
`wrong_length`, `unsupported`, `too_long`, `invalid`, each with a reason text.

KNXnet/IP (`knxnetip.h`) builds CONNECT_REQUEST, CONNECTIONSTATE_REQUEST, DISCONNECT_REQUEST and
_RESPONSE, TUNNELING_REQUEST and _ACK and SEARCH_REQUEST, and parses the header (06 10, the total
length must equal the datagram), CONNECT_RESPONSE, CONNECTIONSTATE_RESPONSE, DISCONNECT_REQUEST and
_RESPONSE, TUNNELING_REQUEST and _ACK, and SEARCH_RESPONSE with the device information DIB (medium,
status, individual address, installation id, serial number, routing multicast address, MAC, friendly
name) and the supported service families. The gateway search itself is a follow-up: a gateway is
never chosen from a discovery automatically.

## The adapter

`KnxAdapter` is the `hc::Technology` "knx". `configure(mappings, count)` gives it one `KnxMapping`
per datapoint:

| Field | Meaning |
|---|---|
| `datapoint` | The datapoint id in the model. |
| `dpt` | The KNX datapoint type its telegrams carry (`hc::Dpt`). |
| `has_send`, `send` | The sending group address (optional): where commands go and which reads it answers. |
| `listen`, `listen_count` | Further group addresses whose writes and responses update it (up to 8); the sending address listens without being repeated. |
| `transmit_only` | The datapoint belongs to another component (the time master); the adapter is only its transmitter (below). |

The flags come from the datapoint: `kTransmit` sends a command as A_GroupValue_Write to the sending
address; `kReadable` answers an A_GroupValue_Read of the sending address with an A_GroupValue_Response
of the current value — only while its quality is good (otherwise the read stays unanswered, logged);
`kReadOnStart` reads the sending address (or the first listening one) when the tunnel comes up.

`configure()` checks every mapping before anything changes: the datapoint exists, is mapped once and
is not owned by another technology; the DPT is supported and produces the datapoint's value type
(or both are numbers: the model converts where exact); at most 8 listening addresses and at least
one address; a transmitting or readable datapoint has a sending address. A refusal keeps the
previous mapping and logs which mapping failed. It binds the datapoints to the adapter and allocates
two tables (by datapoint id, by group address); nothing is allocated afterwards. A model load
unbinds every datapoint: configure again after it. The mapping is programmatic now; the runtime
image loader (issue #8) will feed it from E-Home's export. `configure()` replaces the whole mapping,
so its caller merges the mappings of every source into one list: the image's group objects and the
time master's `mappings()`. `sender_of(address)` names the datapoint that sends on an address, so a
component can refuse an address that is taken.

**Transmit-only mappings.** A datapoint owned by an application component and sent to the bus by
the adapter (`DatapointModel::set_transmitter()`, doc/datapoints.md): `configure()` makes the adapter
its transmitter instead of binding it. The owner publishes (`DatapointModel::publish()`), the model
hands the value to the adapter's `command()`, which sends the group write to the sending address
(refused while the tunnel is down: the publish fails and the value stays). A group read of the
sending address is answered from the model's value while its quality is good. Received telegrams on
the address never update it (logged at debug level; other datapoints listening there do update),
the adapter never marks it stale (the owner does) and never reads it on start. Additional checks: the
datapoint has an owner other than the adapter and no other transmitter, the `kTransmit` flag, a
sending address that no other mapping sends on, and no listening addresses.

Behavior:

- **Received write or response** on a listening address: decoded with the mapping's DPT and reported
  (`DatapointModel::report()`). A refusal is logged with address, sender, DPT, octets and reason —
  `1/2/3 from 1.1.10: 9.001 refuses 7F FF (invalid: 0x7FFF is the invalid value); 'temperature'
  unchanged` — and the datapoint is not touched. An embedded type needs the short form and every
  other type the long form; a mismatch is a `wrong_length` refusal. Repeated frames are delivered
  like originals (a report of the same value; `DatapointUpdate::changed()` is false).
- **Command** of a transmitting datapoint: encoded and queued as a group write; refused (the value
  stays, the command fails with `refused`) while the tunnel is not up, when the value does not
  encode, or when the queue is full. When the gateway confirms the write, the other datapoints
  listening on that address take the value as well — the bus devices do the same; the gateway does
  not echo our own telegram as an indication. The sender is not reported again.
- **Tunnel down**: every mapped datapoint becomes `stale` (`report_stale()`; an unknown one stays
  unknown), except the transmit-only ones, whose owner decides. **Tunnel up**: the read-on-start datapoints are read, one every 100 ms
  (`kReadSpacingMs`), so a hundred reads take ten seconds instead of flooding the gateway; the tunnel
  queue additionally paces them to the bus.
- **Reads on request**: a rule's `read` action (`spec/rules.md` 7.12) reaches the adapter through
  `Technology::read`; the datapoint joins the same read queue (one read per 100 ms, duplicates
  merged), so a rule reading twenty datapoints at once keeps the spacing. Refused, with a log line
  and the rule's `readRefused`, while the tunnel is down; the pending flags are dropped then.

## Time master

`hc::knx::TimeMaster` (issue #9) broadcasts the controller's local time so KNX devices
(thermostats, displays) stay in sync.

**Configuration** (`TimeMasterConfig`, programmatic now; the runtime image of #8 feeds it):

| Field | Default | Meaning |
|---|---|---|
| `time` | off | DPT 10.001 (time of day with the day of the week): `enabled`, the group address, the datapoint (default `knx.time`) |
| `date` | off | DPT 11.001 (date); datapoint default `knx.date` |
| `date_time` | off | DPT 19.001 (date and time with the flags); datapoint default `knx.datetime` |
| `interval_s` | 600 | 60..86400 seconds between broadcasts |
| `at_midnight` | true | also broadcast at local midnight |
| `on_synchronized` | true | also broadcast as soon as the clock is synchronized (and right after `configure()` when it already is) |

**Datapoints.** Each configured address has a datapoint of the image (the DPT's value type and the
`T` flag; `R` to answer reads). The time master owns it (it is a `Technology` that refuses every
command, logged) and publishes each broadcast to it; the adapter is its transmitter through a
transmit-only mapping. So the UI shows what was last sent (origin `publish`), a GroupValueRead of
the address is answered by the adapter from that value, and there is one way to the bus.
`configure()` checks everything before it changes anything: the interval range, each datapoint
(exists, value type, `T` flag, not owned by another technology, no transmitter but the adapter), no
address twice, no address another datapoint of the adapter sends on (`sender_of()`). A refusal keeps
the previous configuration and is logged (`refusing the time master configuration: 10.001 on 1/0/1:
the address is the sending address of 'light'`). An empty configuration switches it off.

**Wiring** (the image loader of #8): load the model; configure the adapter with the image's
mappings; `configure()` the time master; configure the adapter again with the image's mappings plus
`mappings()`; `time().add_listener(master, TimeMaster::kTimeEvents)`. The first broadcast after a
`configure()` follows on the next loop run, after the adapter has the merged mapping.

**When it sends.** Nothing while the clock is not synchronized. Then:

- on synchronization (`on_synchronized`) and after a `configure()` with a synchronized clock;
- at every interval tick, aligned to the local wall clock: when the local time counted in seconds
  since the epoch is a multiple of the interval, so every 10 minutes at :00, :10, ... and an hourly
  interval on the hour (also across DST, for intervals dividing an hour); the time sent is that
  instant, `12:10:00`. The tick timer waits at most 10 minutes (`kMaxTimerMs`) and re-reads the wall
  clock; a tick is up to a second late, never early;
- at local midnight (`at_midnight`), unless a tick already sent on the new date;
- after a clock jump (`kClockJumped`: the devices are off by more than a minute), re-aligning the
  grid;
- after a UTC offset change (a DST transition, a new zone) that no broadcast has shown yet: the local
  time sent is the new one (`03:00:00` with summer time on the spring-forward night).

Each broadcast publishes the configured datapoints in the order time, date, date-time; the adapter
queues the writes and the tunnel paces them. A publish refused while the tunnel is down is logged by
the adapter and the model; the datapoint keeps the last sent value.

**Values.** DPT 10: the day of the week (1 Monday .. 7 Sunday) from the local date, hour, minute,
second. DPT 11: the local date. DPT 19: the same fields plus the flags: summer time (SUTI) from the
zone's DST flag; working day unknown (NWD set, WD clear); fault clear; CLQ set (only synchronized
time is sent, so the clock has an external sync signal); SRC (reliable synchronization source) set,
because the only synchronized sources are SNTP on the device and the host's clock on Linux (a
battery RTC as source would clear it). Wednesday 2026-10-14 12:10:00 CEST is `7E 0A 0E 6C 0A 00 21
C0`; in winter the seventh octet is `20`.

**Synchronization lost** (`kSynchronizationLost`): the broadcasts stop (logged as a warning) and the
datapoints turn stale, so reads are no longer answered. A new synchronization broadcasts at once.

The time master lives in its object (about 100 bytes, two event-loop timers, one of the time
service's four listener slots); nothing is allocated.

## The telegram log

`TelegramLog` keeps the last 64 group telegrams (`kCapacity`) in a ring inside the object (no heap):
time (monotonic and wall clock with its synchronized flag), direction (received, or sent and
confirmed), source, destination, service, priority, repetition, hop count, the data (one octet for a
short telegram, none for a read) and the mapped datapoint. Every record has a sequence number, so a
reader (the console) can ask for what it has not seen (`first_sequence()`, `total()`, `find()`). A
new mapping clears the datapoint references (the ids may mean other datapoints). Its one
`TelegramListener` is told about every record as it is stored: the monitor stream (`MonitorFeed`,
issue #19, spec/api.md "Monitor") is that listener, so nothing polls the log; it adds the decoded
value of the mapped datapoint and keeps its own ring with KNX, Zigbee and the controller's actions
for E-Home's group monitor. The console prints a record as

```
12.345 rx 1.1.10 -> 1/2/3 write 0C 1A [living.temperature]
```

## Settings

| Key | Type | Default | Meaning |
|---|---|---|---|
| `knx.gateway` | string | none: the tunnel stays down | the gateway, `host[:port]`: a host name or IPv4 literal (`host_name_valid()`), port 1..65535, default 3671; IPv6 is refused |
| `knx.nat` | boolean | false | NAT mode: endpoints 0.0.0.0:0, the gateway answers to where the request came from |

These are device settings so that an installation can be moved to another gateway without
redeploying; E-Home's runtime image may provide them later. An invalid stored value is logged and
treated as not set. Persisted names: never rename.

## Platforms

`hc::UdpSocket` (`core/include/hc/platform/udp_socket.h`): open on an ephemeral port, send to an
IPv4 endpoint, the local endpoint towards a peer, close, resolve a host name; datagrams, receive
errors and resolutions reach the `UdpReceiver` on the loop's thread, never from within a call. The
socket calls themselves are the same on Linux and lwIP and live once in `platform/common/src/
bsd_udp.cpp`, compiled by both builds.

- **POSIX** (`hc::posix::PosixUdpSocket`): non-blocking; the main loop sleeps in
  `PosixUdpSocket::wait()` (poll on the descriptor, the sleep otherwise) and the datagrams are
  delivered between two `run_once()` calls; `resolve()` calls `getaddrinfo` (blocking the loop while
  the name server answers, acceptable on the host) and delivers through `EventLoop::post()`.
  `homecontrol` runs the tunnel when `knx.gateway` is set in its settings file
  (`knx.gateway=s:<host>:3671`, `knx.nat=b:1`).
- **ESP** (`hc_knx`, `hc::esp::EspUdpSocket`): the reader task `hc_udp` (4 KB stack, priority 4)
  waits on the socket with `select` (100 ms), copies datagrams into a ring of 8 slots and posts one
  LoopQueue message, whose handler hands them to the tunnel on the loop's task; it also runs
  `getaddrinfo`. The loop retires a closed descriptor to the reader, which closes it (never one it
  may be waiting on); datagrams of a closed socket are dropped by a generation count.
  `hc::esp::KnxService` owns the socket, the tunnel, the adapter and the log, reads the settings and
  starts the tunnel.

### Console (ESP)

| Command | Effect |
|---|---|
| `knx status` | tunnel state and reason, gateway and NAT mode, channel, address and data endpoint, queue, next attempt, mapped datapoints, telegrams seen |
| `knx gateway set <host[:port]>` | stores `knx.gateway` and connects to it |
| `knx gateway clear` | erases `knx.gateway`, closes the tunnel |
| `knx nat on\|off` | stores `knx.nat` and reconnects in that mode |
| `knx log [<n>]` | the last n (all 64) group telegrams |
| `knx write <ga> <dpt> <value>` | one group write; the confirmation follows in the log |
| `knx read <ga> <dpt>` | one group read; the decoded response (or "no response within 5 s") follows in the log |

Values are written in the text form of the DPT's value type: `on`/`off`/`1`/`0`, decimal numbers,
strings, `<control>,<value>` (DPT 2, 3, 18), `HH:MM[:SS]`, `YYYY-MM-DD`, `YYYY-MM-DDTHH:MM:SS`,
`R,G,B[,W]`.

### knxtool (POSIX)

`build/knxtool` talks to a gateway through the same tunnel code:

```
knxtool --gateway <host[:port]> [--nat] monitor                  # every group telegram, until Ctrl-C
knxtool --gateway <host[:port]> [--nat] write <ga> <dpt> <value> # one group write, waits for the con
knxtool --gateway <host[:port]> [--nat] read <ga> <dpt>          # one group read, prints the response
```

The monitor prints `1.1.10 -> 1/2/3 write 0C 1A  ~ 9.001 21` with a guess from the data length (6
bits: 1.001; 1 octet: 5.010 and 5.001; 2 octets: 9.001, else 7.001; 3 octets: 10.001, 11.001,
232.600; 4 octets: 14.000 and 12.001; 14 octets: 16.001). Telegrams go to stdout, the tunnel's log
to stderr. Exit codes: 0 done (monitor: ended with Ctrl-C), 1 the write was not confirmed or the
read not answered within 5 s, 2 a usage error or a refused argument, 3 no tunnel within 15 s (or it
broke). Use it only against the test system (CLAUDE.md, "KNX during development").

## Follow-ups

- The time master could broadcast when the tunnel comes back up (it does not watch the tunnel; the
  next tick sends within the interval).

- Routing (KNXnet/IP multicast) as a second transport beside the tunnel.
- Gateway search (SEARCH_REQUEST on 224.0.23.12:3671; the response decoding exists), offered to the
  user, never chosen automatically.
