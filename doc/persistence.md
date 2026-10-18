# State persistence and short-term history

Two core mechanisms keep datapoint state on flash (issue #21):

- **`hc::ValueStore`** keeps the last good value of every datapoint flagged `kPersist`, with its
  wall-clock time, across a reboot. After a power cut the UI shows "last known 20.5 °C, 3 h ago"
  instead of nothing, and rules see "no value" only for datapoints that never had one.
- **`hc::History`** keeps a ring of recent (time, value) samples for every datapoint flagged
  `kHistory`, for the UI's sparklines (issue #14). The rings live in RAM and are saved on a coarse
  interval, so a reboot does not wipe the last day. Long-term history is not the device's job: the
  MQTT export (issue #18) publishes the live changes to a broker or InfluxDB.

Both are `DatapointListener`s on the event loop's thread and write through the platform's
`BlobStore`. Settings (NVS keys of 15 characters, strings of 127 bytes, `doc/network.md`) are too
small for either, and sharing NVS's wear with the WiFi credentials would be a bad trade.

## Who calls what

The image loader (issue #8) drives both after it has loaded the model:

```
model.load(specs, count);                     // all values unknown
value_store.load();                           // restores the kPersist datapoints (stale)
history.load(history_specs, n, budget_bytes); // allocates and restores the rings
// ... the rule engine starts (spec/rules.md 7.10: reason "boot")
```

On an orderly shutdown (and before a reboot into the updater) the platform calls
`value_store.flush()` and `history.flush()`. An image swap without a reboot just calls the same
`load()` functions again: each first writes what is pending from the previous image (both keep their
own copies of the names, values and samples), so nothing is lost across a swap.

The UI and the API (issues #5, #14) read `ValueStore::wall_time(id)` for "3 h ago" and
`History::range()` for the sparklines.

The runtime image format (issue #8, `spec/`) has to carry, per datapoint, the `persist` flag and
an optional `history` object with `retention` and `spacing` in seconds, and per image the history
budget in bytes. Not specified in this package.

## The BlobStore contract

`core/include/hc/platform/blob_store.h`. Named binary blobs up to `kMaxBlobSize` (20 KB):

| Operation | Contract |
|---|---|
| `write(name, chunks, count)` | Atomic replace with the concatenation of the chunks. After a power cut at any moment the blob holds the complete old or the complete new content (or does not exist if it did not), never a mix. Durable when it returns `ok`. Larger than `kMaxBlobSize`: `too_large`, nothing changed; full store: `no_space`, old content kept. |
| `read(name, offset, buffer, size, length)` | The bytes at `offset`, fewer at the end, 0 at or past the end. |
| `size(name, size)` | The size, `not_found` if there is none. |
| `remove(name)` | Deletes; `not_found` if there is none. |
| `visit(visitor)` | Every blob with its size; the visitor must not modify the store. |
| `open_writer(name, expected, status)` | A streaming write (issue #43): a `BlobWriter` with `append(data, size)`, `commit()` (the same atomic replace as `write()`) and `discard()` (the old content stays), for content that arrives piece by piece and is never held as a whole (the language packs). Bounded by the free space, not by `kMaxBlobSize`: `expected` above the free space is refused (`no_space`) before anything is written, an `append()` that finds the store full returns `no_space` and the caller discards. One writer per store at a time (`busy`); the writer belongs to the store and lives until `commit()` or `discard()`. A store without streaming refuses (`unsupported`, the default: the Zigbee NVS store). |
| `free_space(bytes)` | The bytes a new blob can take now; false if the store cannot tell. |

Names are 1..31 characters of `[A-Za-z0-9._-]`, not starting with a dot (`check_name()`), in a flat
namespace of the store's own (a directory nobody else writes); a name is never a path. Failures
return a status and are logged by the implementation, never abort. Not thread-safe: the core calls
it on the event loop's thread. Writes may take milliseconds; the callers rate-limit them.

## The two implementations

Both write the same way: a blob is the file `<name>` in a directory of the store's own; `write()`
writes `.<name>.tmp` (a dot name, which no blob can have, so a blob named `x.tmp` is an ordinary
blob), syncs and closes it, and renames it over `<name>`. A power cut leaves the old or the new
blob plus, at worst, an orphaned temporary file, which the store deletes when it opens the
directory (logged: "deleted N unfinished write(s)"). `visit()` lists only names that pass
`check_name()` and are regular files: dot files, subdirectories and foreign names are ignored.
The streaming write uses the same temporary file: `open_writer()` creates `.<name>.tmp`, each
`append()` writes to it, `commit()` syncs, closes and renames it exactly as `write()` does,
`discard()` closes and deletes it. Until the rename the blob's name shows the old content to every
reader (the web server serves the old language pack meanwhile), and the temporary file never shows
under the blob's name; a power cut mid-stream leaves only the orphaned temporary file. The free
space is the partition's (`esp_littlefs_info`) on the ESP and the file system's (`statvfs`) on
POSIX. `MemoryBlobStore` collects into a heap buffer (sized from `expected`, doubled when it is
exceeded) that `commit()` installs as the blob; the tests' `FakeBlobStore` keeps the temporary
content under the LittleFS name in `temporaries`, which `visit()` does not list.

The directory is opened by `start()` or the first call. If that fails (the partition is not
mounted, the directory cannot be created or opened), every call refuses with `storage_error` and
the cause is logged once ("blob store unusable: ..."); the value store and the history then log
their write failures and keep running from RAM. Error mapping: `ENOENT` on read, size and remove is
`not_found`; `ENOSPC` (and `EDQUOT` on POSIX) is `no_space`, the temporary file is deleted and the
old blob kept; every other error is `storage_error`, logged with the path and `strerror`. Invalid
names and oversize writes are refused (logged) before the store is touched.

| | ESP: `hc::esp::LittleFsBlobStore` | POSIX: `hc::posix::FileBlobStore` |
|---|---|---|
| Where | `platform/esp/components/hc_storage` | `platform/posix/src/file_blob_store.cpp` |
| Directory | `/storage/state` on the LittleFS partition `storage` | given at construction; `FileBlobStore::directory_for_settings()` gives `homecontrol.state` beside `homecontrol.settings` (created with mode 0700; the parent must exist) |
| Durability | `fsync` + `close` commit the file; LittleFS's `rename` is atomic and commits the directory | `fsync` of the file, `rename`, `fsync` of the directory (also after `remove()`) |
| Status | the `storage status` console command; the mount and the blob count with the free space in the log | the blob count in the log |

### The storage partition

`hc_storage` owns the one mount of the LittleFS partition `storage` (204 KB on the 4 MB board: 312 KB before the three Zigbee partitions, 268 KB before the core dump partition;
`doc/ota.md`) at `/storage`: `storage_mount()` mounts it read-write on the first call and returns
the first result on every later one; the web server (for the UI) and the blob store both call it,
in either order. It never formats: a partition without a file system (no `storage.bin` flashed) or
a damaged one stays unmounted, the web server answers 503 and the blob store refuses, instead of an
empty file system hiding the fault. Layout:

| Path | Content | Written by |
|---|---|---|
| `/storage/ui` | the web UI image (`doc/web.md`) | flashing `storage.bin` |
| `/storage/state` | the BlobStore's blobs | the device |
| `/storage/image` | the runtime image (issue #8, later) | the device |

Flashing `storage.bin` (a full USB flash with `flash_args`, or the web flasher) replaces the whole
partition and with it the saved values and history; an OTA update of the application keeps them.
`storage status` on the console prints the mount, used and free bytes, and the blob count and
bytes in `/storage/state`.

## Last values: `hc::ValueStore`

### What is written when

A *report* or *command* of a `kPersist` datapoint makes its record *pending* when the value
changed. A repeated report of the same value only matters for the time: it makes the record
pending when the saved time is at least `refresh_s` old (so "last known 3 h ago" stays true for a
value that is re-sent every ten minutes). A stale report changes nothing (the record holds the last
*good* value).

A pending record is written when both hold:

1. **Per datapoint**: its last write is at least `min_interval_s` old. Changes in between are
   coalesced: the latest value wins. The first write of a datapoint after a load is immediate.
2. **Global budget**: a token bucket of `writes_per_hour` tokens that refills continuously
   (`writes_per_hour` per hour) and starts full; each write takes one. A write that finds the
   bucket empty waits (logged once: "write budget of N per hour exhausted: M values wait"; and
   "write budget recovered" when it is over).

If both hold at the update, the write is posted to the event loop at once; otherwise the store's
tick (every 5 s) writes it as soon as they do. `flush()` writes everything pending immediately,
ignoring interval and budget. A failed write is logged once per failure streak and retried after
the interval (it costs a token, so a broken store is not hammered).

| Limit (`ValueStoreLimits`) | Default |
|---|---|
| `min_interval_s` | 60 s |
| `writes_per_hour` | 60 (also the burst) |
| `refresh_s` | 3600 s |

A crash loop cannot burn the flash through the refresh rule: after a reboot the bus usually reports
the same values again, which are written only if their saved time is older than an hour.

### Restore

`load()` reads the record of every `kPersist` datapoint and calls `DatapointModel::restore()`:
the value (converted where exact; a type changed by the image so that it does not convert is
refused and logged) with quality **stale**, `updated_ms` 0 and no listener notification (the
restore is part of the load; rules start afterwards, `lastKnown()` returns it). The record's wall
time is what `wall_time(id)` returns until the next update. Records of datapoints no longer
flagged (or no longer in the image) are deleted at load; a record whose name field does not match
the datapoint (a hash collision) is ignored.

The wall time of a value is the wall clock at its update when the clock is synchronized. If it is
not (no SNTP yet), the record is written without a time and `wall_time()` derives the time from the
monotonic update time once the clock is known; the next write then carries it.

### Record format (frozen)

One blob per datapoint, named `v.` + the 8 lower-case hex digits of the FNV-1a (32 bit) hash of the
datapoint name (`v.1a2b3c4d`). Little-endian throughout:

| Bytes | Content |
|---|---|
| 0..1 | Magic `H` `V` |
| 2 | Format generation, 1. A reader refuses a higher one ("written by a newer firmware") and ignores the record. |
| 3.. | Fields: tag (1 byte), length (1 byte), data. A reader skips tags it does not know. |
| last 4 | CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320) of all bytes before it |

| Tag | Field | Data |
|---|---|---|
| 1 | name (required) | the datapoint name, 1..63 bytes |
| 2 | value (required) | `ValueType` number (1 byte), then the payload below |
| 3 | wall time (optional) | UTC seconds, int64; absent: unknown |

Value payloads: boolean 1 byte (0/1); unsigned, signed, enumeration 4 bytes; real 8 bytes (IEEE
754 double, finite); string 0..28 bytes UTF-8 without NUL; raw 0..14 bytes; time of day 4 bytes
(day of week, hour, minute, second); date 4 bytes (year u16, month, day); date-time 10 bytes (year
u16, month, day, day of week, hour, minute, second, flags u16); controlled 2 bytes (control 0/1,
value); color 5 bytes (red, green, blue, white, valid bits). The `ValueType` numbers are those of
`hc/datapoint.h` and frozen with this format.

A record is at most 113 bytes; the reader accepts up to 512 (fields of a newer firmware). New
information is added as a new tag within generation 1; the generation changes only for a change an
old reader would misread.

### Memory

One slot per `kPersist` datapoint, allocated at load (two blocks: the slots and the names), nothing
in steady state: 80 bytes per slot on the ESP32 (the last value 32, monotonic and wall times, the
name pointer, the hash, flags; 88 on a 64-bit host) plus the name and its NUL. 200 persisted
datapoints with 20-character names take about 20 KB. Each write uses a 128-byte buffer on the
stack, each restore 512 bytes.

## Short-term history: `hc::History`

### Specs and the budget

The image declares per `kHistory` datapoint a `HistorySpec`: `retention_s` (how far back) and
`spacing_s` (minimum distance of two samples, also the keepalive period). A ring holds
`ceil(retention / spacing) + 1` samples, so it spans its retention: 24 h at 1 min is 1441 samples
(11,528 bytes), 7 days at 10 min 1009 samples (8072 bytes), 7 days at 5 min 2017.

| Limit | Value |
|---|---|
| spacing | 10 s .. 86400 s |
| retention | spacing .. 31 days |
| samples per ring | at most 2048 (a ring fits one blob) |
| history budget | stated by the image; at most `HistoryLimits::cap_bytes` (64 KB default, the C6) |

The budget counts the sample memory, 8 bytes per sample, which E-Home computes the same way on its
side. `load()` refuses with the reason in the log, keeping the previous rings, when:

- the image's budget exceeds the cap ("the image asks for 70000 bytes, the device allows at most
  65536"), or the rings exceed the budget ("the rings need 1456 bytes, the image's budget is 976")
  — `budget_exceeded`;
- a spec names a datapoint without `kHistory`, or a `kHistory` datapoint has no spec —
  `flag_mismatch`;
- the datapoint is not boolean, unsigned, signed or real — `type_error`;
- spacing, retention or the sample count is out of range, or a datapoint has two specs —
  `malformed`;
- a datapoint id does not exist — `unknown_datapoint`.

### Sampling

A sample is (UTC seconds as uint32, the value's 32 bits): a boolean as 0/1, unsigned and signed
integers as they are, a real as an IEEE 754 single (7 significant digits, enough for a sparkline;
KNX 9.x and 14.x values are no more precise). Samples are taken only while the datapoint's quality
is good (an outage is a gap) and the wall clock is synchronized (a sample needs a real time).

- A good update whose value differs from the last sample is sampled at once if the last sample is
  at least `spacing_s` old.
- The tick (every 5 s) samples the current value of every ring whose last sample is at least
  `spacing_s` old: a change that came too early is taken then, and a flat line gets a *keepalive*
  sample every `spacing_s`, so it has points.

So samples are never closer than `spacing_s` and, while the value is good, at most
`spacing_s` + 5 s apart. Changes faster than the spacing are not all recorded: the spacing is the
resolution the image asked for.

When the ring is full the oldest sample is overwritten. Samples older than `retention_s` are
dropped when a new sample is added and never returned by a query. When the wall clock goes back
(a resync), samples newer than the new time are dropped (logged), so a ring is always strictly
increasing in time.

### Queries and decimation

`range(id, from, to, out, max_points, count)` returns the samples with `from <= t < to` that are
within the retention. If there are at most `max_points`, each sample is one point (`t` = `last_t`
= its time, min = max = last = its value). Otherwise the range is split into `max_points` buckets
of equal width (`ceil((to - from) / max_points)` seconds), and each non-empty bucket gives one
point: `t` the bucket's start, `last_t` the time of its last sample, the minimum, the maximum and
the last value, the sample count. A sparkline of 100 points over 24 h is one call; drawing min..max
as a band and `last` as the line shows peaks that a plain subsample would miss. A boolean's `max`
says "on at some time in the bucket". The call is O(log n + samples in range), allocates nothing,
and is refused with `unknown_datapoint` (no history for the id) or `malformed` (`to <= from`,
`max_points` 0).

### Saving and restoring

Rings with new samples are saved every `save_interval_s()` and by `flush()`; one blob per ring,
named `h.` + 8 hex digits of the FNV-1a hash of the datapoint name. The interval is
`max(min_save_interval_s, sample bytes * 3600 / save_bytes_per_hour)`: 15 minutes by default, longer
for a large history, so at most 64 KB of samples are written per hour (the wear budget below). A
power cut loses at most the samples of one interval; an orderly shutdown loses nothing.

`load()` restores each ring from its blob and merges it into the series: the newest samples that
fit the (possibly smaller) ring, strictly increasing in time, and — when the wall clock is known —
none from the future and none older than the retention; samples taken after the restore continue
the series. A blob of another datapoint (hash collision), of a changed type, with a bad checksum or
of a newer format generation is ignored with the reason in the log. Blobs of rings no longer in the
image are deleted.

### Ring blob format (frozen)

Little-endian:

| Bytes | Content |
|---|---|
| 0..1 | Magic `H` `H` |
| 2 | Format generation, 1 (a reader refuses a higher one) |
| 3..4 | Header length H (u16): the bytes of header fields that follow |
| 5..5+H | Header fields: tag (1 byte), length (1 byte), data; unknown tags are skipped |
| then | `count` samples of `sample size` bytes each, oldest first: UTC seconds u32, value bits u32, and any further bytes (a newer firmware's) which a reader skips |
| last 4 | CRC-32 (as in the value record) of all bytes before it |

| Tag | Field | Data |
|---|---|---|
| 1 | name | the datapoint name |
| 2 | type | `ValueType` number, 1 byte |
| 3 | spacing | u32 seconds (informative) |
| 4 | retention | u32 seconds (informative) |
| 5 | sample size | 1 byte, at least 8 |
| 6 | count | u32 |

The blob's size must equal 5 + H + count × sample size + 4. A ring of 2048 samples is 16,384 bytes
of samples plus at most about 110 bytes of header and checksum.

### Memory

Allocated at load in three blocks (samples, ring table, names), nothing in steady state:

| Part | Size |
|---|---|
| Sample | 8 bytes |
| Ring | 36 bytes on the ESP32 (48 on a 64-bit host) + the name and its NUL + 8 × samples |
| 24 h at 1 min | 11,528 bytes of samples per ring |
| C6 cap | 64 KB of samples: five 24 h rings at 1 min, or eight 7-day rings at 10 min |

A save writes from the ring in place (header, the two halves of the ring, the checksum as four
chunks); a restore reads straight into the ring. The stack use is a 256-byte header buffer.

## The flash wear budget

The limits are chosen so that the flash outlives the device even when they are used to the full,
all the time. Assumptions:

- **Endurance**: 100,000 erase cycles per 4 KB sector (the SPI NOR flash of the ESP32-C6 modules).
- **Life**: 10 years, 87,600 hours.
- **Wear leveling**: the 204 KB `storage` partition of the 4 MB board (268 KB before the core dump
  partition of issue #4) has 51 blocks; the web UI's files (issue #14 limits them to about 50 KB
  gzipped) and LittleFS's own blocks leave about 35 free. LittleFS allocates new data blocks across the free blocks and relocates metadata pairs
  after a number of erases (`block_cycles`), so the wear spreads; assumed: over **35 blocks** (48
  before the core dump partition took 64 KB of the storage).
- **A value record write** (at most 113 bytes) is an inline file in the directory's metadata log;
  write plus rename append about 256 bytes, so a 4 KB metadata block takes about 16 writes before
  it is compacted (one erase). Assumed with a margin of two: **one erase per 8 writes**.
- **A ring save** rewrites the ring's data blocks (copy on write): one erase per 4 KB of samples
  plus a metadata commit; assumed: **bytes / 4096 + 1 erases per ring**.

At the limits:

| Writer | Per hour | Erases per hour |
|---|---|---|
| Value records: 60 writes | 60 × 1/8 | 7.5 |
| History: 64 KB of samples (e.g. five 24 h rings) | 16 + 5 | 21 |
| **Total** | | **28.5** |

28.5 erases per hour × 87,600 hours = 2.5 million erases over 35 blocks = about **71,000 cycles
per block, 71 % of the endurance** (52,000 over the 48 blocks before the core dump partition) after
ten years of continuous worst case. A real house writes far
less: setpoints and states change a few times a day, and the history budget of a typical image is
a fraction of the cap.

For comparison, the same budget on NVS (if a platform backed the store with NVS blobs): the
24 KB `nvs` partition has 6 pages of 126 entries of 32 bytes, one page kept free for compaction; a
113-byte record is a blob of 5 entries, so a page is erased every 25 writes: 60 writes per hour
are 2.4 page erases per hour, 210,000 over ten years on 5 pages, 42,000 cycles each — the same
order, but the history would not fit at all, and the WiFi credentials would share the wear.

The bigger boards (8 and 16 MB, `storage` of 3 or 9 MB) have ten times the blocks; a platform may
raise `writes_per_hour`, `save_bytes_per_hour` and `cap_bytes` there.

## Tests

`core/test/value_store_test.cpp` and `core/test/history_test.cpp` run under the fake clock with an
in-memory `FakeBlobStore` (`core/test/fake_blob_store.h`) that counts writes: coalescing and the
budget (counted writes), the refresh rule, restore with stale quality and the saved time, a
datapoint without a value staying unknown, a record with an unknown field restoring, a newer
generation and a bad checksum ignored, every value type through encode and decode, a changed type,
orphan deletion, flush and the image swap, failing stores; the ring capacity, the budget and cap
refusals, spec validation, sampling with spacing and keepalives, gaps, the ring wrap, retention,
decimation (min, max, last per bucket, never more than `max_points`), the save interval, save and
restore across a reboot and an image swap, a blob with unknown header fields and larger samples, a
damaged blob, a clock going back.

`platform/posix/test/file_blob_store_test.cpp` runs the POSIX store against a temporary directory:
the round trip with chunks and reads at offsets, replace and remove, `visit()` skipping foreign
files, a crash's temporary files ignored and deleted while the old blob is still served, a blob
named `x.tmp`, the name and size refusals before anything is touched, an unusable directory refusing
every call (logged once), the default directory, the streaming write (a 50 KB blob in 1 KB pieces:
the temporary file is not listed and the old content is served until the commit, `busy`, `discard()`
keeps the old blob, the free-space refusal). `core/test/blob_writer_test.cpp` runs the same for
`MemoryBlobStore` and the fake. `no_space` from the file system is not tested (it needs a full file
system). The ESP store shares the algorithm; it is verified on the board (`storage status`, a
reboot keeps the values).
