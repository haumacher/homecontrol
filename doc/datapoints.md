# Datapoints and DPT codecs

Everything the controller knows about the house is a datapoint (ROADMAP item 2): KNX group objects,
Zigbee attributes, schedule outputs, weather values, rule parameters. Rules, the UI and logging see
only datapoints; technology adapters translate between their bus and the model. The model is in
`core/include/hc/datapoint.h`, the adapter interface in `hc/technology.h`, the KNX datapoint type
codecs in `hc/dpt.h`.

## Values

`hc::Value` is a typed value of 32 bytes, stored inline (no heap):

| `ValueType` | C++ | Used for (KNX) |
|---|---|---|
| `boolean` | `bool` | 1.x |
| `unsigned_int` | `uint32_t` | 5.x (unscaled), 7.x, 12.x, 17.x |
| `signed_int` | `int32_t` | 6.x, 8.x, 13.x |
| `real` | `double` | 5.001, 5.003, 8.010, 9.x, 14.x |
| `string` | UTF-8, at most 28 bytes | 4.x, 16.x (14 Latin-1 characters take up to 28 bytes) |
| `time_of_day` | `TimeOfDay` (day of week 0..7, h, min, s) | 10.x |
| `date` | `Date` | 11.x |
| `date_time` | `DateTime` (fields plus the DPT 19 flag bits) | 19.x |
| `enumeration` | `uint32_t` element number | 20.x |
| `controlled` | `Controlled` (control bit, small number) | 2.x, 3.x, 18.x |
| `color` | `Color` (red, green, blue, white levels 0..255, validity bits) | 232.600, 251.600 |
| `raw` | at most 14 bytes | a value without a type |

A value of type `none` is empty (the value of a datapoint that never had one). NaN and infinities
are not values. `Value::convert()` converts exactly between `unsigned_int`, `signed_int`, `real`
and `enumeration` (a real only when it holds an integer); nothing else converts.

## The model

`hc::DatapointModel` holds the table of the loaded runtime image. `load(specs, count)` declares
each datapoint by name (1..63 printable bytes, unique), value type and flags; the id is the index in
the spec array (`uint16_t`, at most 65535 datapoints). Every spec is validated before anything
changes: a refused load leaves the previous model in place and logs the reason. After a load all
values are unknown and no technology is bound; `bind(id, technology)` assigns the owner (the image
loader of issue #8 does this for each adapter). `get(id)` is O(1), `find(name)` a binary search.

Flags (`DatapointFlags`):

| Flag | Meaning |
|---|---|
| `kReadable` | The technology answers read requests from its side with the current value (KNX R). |
| `kWritable` | The application may command the datapoint; commands to others are refused. |
| `kTransmit` | Commands go to the owning technology to transmit (KNX T); without it a command only changes the model. |
| `kReadOnStart` | The technology reads the value when it starts (KNX I). |

The model enforces `kWritable` and `kTransmit`; `kReadable` and `kReadOnStart` are instructions to
the adapter.

Each datapoint carries its current value, the monotonic time of its last update (`updated_ms`, 0
before the first) and a quality:

- `unknown`: nothing reported or commanded since the load;
- `good`: the last update came from the technology or the application;
- `stale`: the technology lost contact (`report_stale()`: bus down, device gone); the value is the
  last known one and stays until the next report.

### Owner and transmitter

Every datapoint has at most one owner (`bind()`): the technology or component that reports it,
marks it stale and receives the application's commands. A datapoint can additionally have one
**transmitter** (`set_transmitter(id, technology, error)`): the technology that sends what the owner
publishes to its bus. This is for values an application component produces and a bus carries: the
KNX time master owns `knx.time` and the KNX adapter transmits it (doc/knx.md); later weather or
virtual values published to KNX, or the scheduler's outputs mirrored to the bus.

- `set_transmitter()` is refused (`not_owner`, logged, the reason in `error`) while the datapoint
  has no owner and when another transmitter is set; the owner itself is accepted and changes nothing
  (an owner that transmits does so in its own code). `load()` and `clear()` drop it like the binding.
- The transmitter receives only what the owner publishes; it may not report the datapoint or mark it
  stale (`not_owner`), and the application's commands still go to the owner, which decides.

### The write directions

- **`report(technology, id, value)`** — the technology says what the value is (a telegram arrived,
  a device reported). Only the owning technology may report; the value is converted to the
  datapoint's type where exact, otherwise refused.
- **`publish(owner, id, value)`** — the owner says what it produced (the time master's broadcast).
  Only the owner may (`not_owner`); the value converts like a report. If the datapoint transmits
  (`kTransmit`) and has a transmitter, the transmitter's `Technology::command()` receives it first:
  when it refuses (the tunnel is down), `publish()` fails with `refused` and nothing is stored, the
  rule of `command()`. Otherwise the value is stored and announced with the origin `publish`, so the
  log and the UI can tell "we sent this" from "the bus said this". `kWritable` is not needed: it is
  about the application's commands.
- **`command(id, value)`** — the application (a rule, the UI) wants a value. Refused if the
  datapoint is not writable or the value does not convert. If the datapoint transmits and has an
  owner, the owner's `Technology::command()` receives the converted value first: it returns true
  when it accepted the value for transmission (queued or sent), false to refuse (the command fails
  with `refused`, the value stays). On acceptance, or without a transmitting owner (a rule
  parameter, an internal value), the model stores the value at once.

Both store with quality `good` and the current time. A technology must not call back into the model
from inside `command()` (refused as `busy`); it reports later through the event loop. A KNX adapter
does not see its own telegram again, so the model's optimistic store is the bus state; a Zigbee
adapter's attribute report later confirms or corrects it.

### Listeners

`add_listener()` registers up to `kListenerCapacity` (8) `DatapointListener`s for the whole model
(the rule engine, the UI's live feed, logging, an adapter watching other datapoints). After every
update each listener's `datapoint_updated()` runs synchronously on the event loop's thread, in
registration order, with the datapoint (new value, quality, time), the old value, the old quality
and the origin (`report`, `command`, `stale`, `publish`). Every report is an update, also one repeating the
same value (a push button sending "on" again matters to a rule); `DatapointUpdate::changed()` tells
whether value or quality differ.

While listeners run the model refuses every change — `report`, `report_stale`, `command`,
`publish`, `load`, `clear`, `bind`, `set_transmitter` and listener (un)registration — with `busy` and a log line. A listener that wants to
react with a change posts it to the event loop; it then runs in the next `run_once`, after all
listeners have seen the current update. This keeps the order of notifications equal to the order of
updates and bounds the work of one update.

Every refusal returns a `DatapointStatus` and logs the reason with the tag `hc.dp`.

## Technology adapters

`hc::Technology` is the interface an adapter implements: `name()` for the log and `command()` for
the commands of its transmitting datapoints. It reports with `DatapointModel::report()` and
`report_stale()`. The KNX adapter (issue #7) decodes a received group write with `hc::decode()` and
reports it, and encodes a commanded value with `hc::encode()`. The tests use a `LoopbackTechnology`
(`core/test/datapoint_test.cpp`) whose commands come back as reports.

## Memory

Allocated once per `load()`, nothing in steady state:

| Part | Size per datapoint |
|---|---|
| `Datapoint` table entry (value 32, time 8, name, owner and transmitter pointers, id, type, flags, quality) | 64 bytes on the ESP32 (32-bit), 72 on a 64-bit host |
| Name index (ids sorted by name) | 2 bytes |
| Name | its length + 1 byte |

So a datapoint with a 20-character name takes 87 bytes on the ESP32; 1000 such datapoints about
85 KB. (The transmitter pointer added 8 bytes per entry on both: 4 bytes of pointer and 4 of
padding to the 8-byte alignment of the time.) The model object itself (listeners, pointers) takes about 60 bytes on the ESP32. Two blocks are allocated
per load (the table, and the names with the index); during a reload the old and the new tables
exist at the same time until the new one is validated. Allocation failure refuses the load
(`out_of_memory`), it never aborts.

## DPT codecs

`hc::Dpt` identifies a KNX datapoint type (`main.sub`; `Dpt::parse()` reads "9.001", "9",
"DPST-9-1", "DPT-9"). `hc::encode(dpt, value, bytes)` and `hc::decode(dpt, data, length, value)`
return a `DptResult` with a status (`ok`, `unsupported`, `wrong_length`, `out_of_range`, `invalid`,
`wrong_type`) and a reason text.

- `Dpt::embedded()` is true for the types of up to 6 bits (main types 1, 2, 3): their value
  travels in the APCI octet. The codec writes and reads one octet holding those bits; the KNX
  adapter merges it into the APCI octet (data length 0). DPT 18 (scene control) has 8 bits and is a
  normal one-octet type.
- `Dpt::size()` is the number of data octets, `Dpt::value_type()` the value type of the type.
- Out-of-range values are refused, never clamped. Scaled types round to the nearest code, a half
  up (5.001, 5.003, 7.003, 7.004, 8.003, 8.004, 8.010, 9.x; 14.x rounds to the nearest single).
  An integer type refuses a fractional value (`wrong_type`).
- Decoding ignores reserved bits but refuses field values outside their range (hour 24 in DPT 10,
  month 13, a year above 99 in DPT 11) and the codes a type declares invalid (0x7FFF in 9.x, NaN
  and infinities in 14.x, non-ASCII bytes in 4.001 and 16.000, unknown elements of 20.102 and
  20.105, tariff 255 in 5.006).
- A sub number the codec does not distinguish uses the rules of its main type (9.999 is a 9.x
  float with the full range); the main type alone ("5") is the unscaled number.

Implemented: 1, 2, 3, 4 (4.001 ASCII, 4.002 Latin-1), 5 (5.001 percent, 5.003 angle, 5.006
tariff, others unscaled), 6 (except 6.020 status with mode, refused as unsupported), 7 (7.003 and
7.004 scaled to ms), 8 (8.003, 8.004 scaled to ms, 8.010 percent), 9 (with the lower limits of
9.001, 9.004..9.008, 9.027..9.030), 10, 11, 12, 13, 14, 16 (16.000 ASCII, 16.001 Latin-1), 17, 18,
19, 20 (value sets of 20.102 and 20.105; other subtypes accept every element), 232.600, 251.600.

The behavior is pinned by the conformance corpus `spec/fixtures/dpt/` (format and the known
differences to E-Home's Calimero-based codec in `spec/dpt-corpus.md`), run by
`core/test/dpt_corpus_test.cpp`.
