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
value_store.load();                           // restores the kPersist datapoints (good)
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
| `open_writer(name, expected, status)` | A streaming write (issue #43): a `BlobWriter` with `append(data, size)`, `commit()` (the same atomic replace as `write()`) and `discard()` (the old content stays), for content that arrives piece by piece and is never held as a whole (the language packs). Bounded by the free space, not by `kMaxBlobSize`: `expected` above the free space is refused (`no_space`) before anything is written, an `append()` that finds the store full returns `no_space` and the caller discards. One writer per store at a time (`busy`); the writer belongs to the store and lives until `commit()` or `discard()`. A store without streaming refuses (`unsupported`, the default: the NVS store of `hc_zigbee`). |
| `patch(name, offset, chunks, count)` | Overwrites bytes of an existing blob in place (issue #56: the history's archive segments, slot by slot). The blob keeps its size (`too_large` if the chunks would end past it, nothing changed); `not_found` without the blob. Atomic like `write()`: after a power cut the blob holds the complete old or the complete new content. A store that cannot patch refuses (`unsupported`, the default); the history then keeps its archives' slots pending and logs the failure. |
| `write_from(name, source)` | `write()` of the content a `BlobSource` produces (homecontrol #111: the Zigbee device database): `content_size()`, then `content(buffer, size)` into a buffer of `size` + 1 bytes. The default asks at once, writes and frees the buffer; the `BlobQueue` asks when its worker is about to write (below). The source stays valid until the write is done or `forget_source(source)` was called, which makes a store that holds a waiting `write_from()` take its content at once. |
| `free_space(bytes)` | The bytes a new blob can take now; false if the store cannot tell. |
| `has_room(bytes)` | Whether a write or patch of `bytes` (0: a removal) would be taken now without writing the flash on the caller's thread (homecontrol #118). The default (a store that writes in place): true; the `BlobQueue`: an entry and room in its arena are free. |
| `settled(name)` | Whether no accepted change of the blob still waits to reach the backing store (performed, or dropped after its attempts): a read then answers what the store holds. The default: true. |

Names are 1..31 characters of `[A-Za-z0-9._-]`, not starting with a dot (`check_name()`), in a flat
namespace of the store's own (a directory nobody else writes); a name is never a path. Failures
return a status and are logged by the implementation, never abort. Writes may take milliseconds;
the callers rate-limit them.

**Threads.** The loop's writers (the value store, the history and its archives, the blocks'
managed state, the inhabitant's timelines, the language packs, the Zigbee device database) reach the store through the write
queue (`BlobQueue`, below, homecontrol #101), whose worker performs their changes while the loop
goes on reading; the storage figures' measurement (below) walks the store from a worker of its own
(`visit()`, `free_space()`, `usage()`). So an implementation takes calls from several threads at
once and keeps no per-call state in the object. Calls on the same blob do not overlap, with one
exception: the loop may read a blob while the queue's worker patches or writes it, and gets the
complete old or the complete new content (the queue overlays the patch it has in flight, so the
loop reads the new). LittleFS (`LittleFsBlobStore`: paths on the stack, the file system's own lock
around each call, an atomic rename for a write, a patch committed at the file's sync; littlefs
fixes up open directories when another call commits, so `visit()` beside a write is safe) and POSIX
files (`FileBlobStore`: writes and patches through a temporary file renamed over the blob) do; the
tests' `FakeBlobStore` and `MemoryBlobStore` are not thread-safe and are only used with the stepped
`ManualWorker`, on one thread.

**Writing off the loop (#101).** On the house controller (firmware 1.5.0, 2026-11-20) the flash
writers held the loop 26 to 182 ms: the blocks' records (`BlockRuntime::on_persist`, 26 to 44 ms
about every minute, 143 to 182 ms every 8 to 12 minutes, LittleFS erasing and compacting behind
the write), the value store (`ValueStore::on_post`, `on_tick`, 35 to 41 ms) and the uptime record in
NVS (43 ms); the 20 ms timers missed periods for each. #102 removed the periodic writers (no block
state but the managed one, no uptime record); what remains is written on change, and #101 takes it
off the loop. `BlobQueue` (`core/include/hc/blob_queue.h`) is a `BlobStore` in front of the
platform's: a write, patch or removal is copied into its arena and returns at once; a worker of its
own (`hc_flash` on the ESP, 4 KB stack, priority 2 below the loop's 5, its job logged at debug
level of `hc.worker`; a thread on POSIX) performs the changes one after the other, in the order they
came. A read, size or visit answers from the queued changes where there are any (read your
writes: a deploy's load reads the records its flush has just queued); a new write or removal of a
blob replaces its waiting changes (not the one in flight), a patch of a waiting write goes into its
copy. A change the store refuses is tried again after 30 s (the other blobs' changes go first, the
same blob's wait behind it), at most five times, then dropped with an error line. When the arena
(8 KB on the C6, 64 KB on POSIX) or its 24 entries (64) are full, or a blob is larger than the
arena, the change goes to the store at once on the loop as before, with a warning once per streak
(`write queue full ...`) — the history's rings above 8 KB (about 1000 samples) are such writes,
and they show as `slow ... handler history.on_tick`; the house controller has no history. A writer
that must not write on the loop asks `has_room()` first and waits (the history's carry-over, #118);
one that deletes what its write replaces waits for `settled()` and reads the write back. A planned
restart waits for the queue (a `Shutdown` participant, at most 1 s); the POSIX build also drains
it before it exits. The arena costs 8 KB of RAM and the entry table 1.5 KB, allocated at start.

A `write_from()` (homecontrol #111) goes into the queue without a copy: the entry holds the source,
and the queue asks it for the content on the loop when the worker is about to write the blob, into a
buffer of exactly the content's size that the worker writes from and the loop frees when it is done.
So a blob larger than the arena (the Zigbee device database: about 15 KB for 30 devices) is written
off the loop too, a later `write_from()` of the same blob replaces the waiting one at no cost (at most
one buffer per blob: the one in flight), and a failed write frees its buffer and asks the source
again at its retry, for the content of that moment. A read, size or visit of such a blob while it
waits asks the source (a read into a temporary buffer). A source that refuses (its own log line)
counts as a failed attempt.

What the worker cannot take away: on the single-core C6 a flash erase or program runs with the
cache off and the scheduler suspended (`CONFIG_SPI_FLASH_AUTO_SUSPEND` is not set), so every task
waits for one sector erase (tens of ms; the IDF yields between sectors,
`CONFIG_SPI_FLASH_YIELD_DURING_ERASE`). Since the worker runs only when the loop sleeps, such a
pause delays the loop's next wakeup by at most one erase and is not a turn of the loop: no slow
handler line, and no missed period for a timer whose period is longer than the erase. The file
system's lock is shared: a read of the loop beside the worker's write waits for the write's current
call (LittleFS's mutex, with priority inheritance).

Not through the queue: the streaming write of a language pack (`open_writer()`, a download, rare;
refused with `busy` while a queued change of the same blob waits), the one move of the Zigbee device
database out of NVS at the start (doc/zigbee.md "Partitions": written directly to the store, read back
and compared before the old copy goes), and the settings in NVS (`Settings`: the
rules' states, the overrides, written on a change; a write is a few ms, an NVS page erase more;
their `visit()` beside a write of another task is not safe on the IDF's NVS iterator).

**The storage figures (#74).** What the status and the System page show of the store (the
partition's size, use and free space, the files and their bytes) and the free space of the image's
`storable` are walks of the file system on LittleFS: `esp_littlefs_info()` follows every file's
blocks (`lfs_fs_size`), and `visit()` reads the directory and stats each file, which fetches the
directory's metadata log twice per file (`lfs_stat` and the mtime attribute of
`CONFIG_LITTLEFS_USE_MTIME`). On the C6 with 8 blobs a `GET /api/status` (two such walks and a visit)
and a `GET /api/system/diagnostics` (one and a visit) held the loop 49 to 124 ms each, every 5 s while
the System page was open; the cost grows with the directory's log until LittleFS compacts it (a host
model of the partition with littlefs 2.11, esp_littlefs's sizes and the board's 8 files: a visit
reads 70 to 185 KB of flash in 300 to 1000 reads, `lfs_fs_size` 8 to 24 KB, a saw tooth over the
history's patches). `StorageStats` (`core/include/hc/storage_stats.h`) measures them once at start
and again after a change: every implementation counts its changes (`BlobStore::changed()` after
each write, patch, removal and commit; `changes()`, atomic), the first request after a change starts
a measurement on a worker of its own (`hc_stat` on the ESP, 3 KB stack, priority 2, its job logged
at debug level of `hc.worker`; a thread on POSIX) and gets the previous figures, the next one the
new. The image service asks for a measurement right after a store or a removal. A store that
answers `usage()` gives the free space as `total - used`, so a measurement is one `lfs_fs_size` and
one visit. What must be exact still asks the store (a streaming write's free-space check, the
archives' check at a load). The walk holds the file system's lock for one `lfs_fs_size` at most
(8 to 24 KB of reads), so a write of the loop's beside it waits for that, not for the loop.

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
| Patch | `open(O_WRONLY)`, `lseek`, write, `fsync`, `close`: LittleFS copies the changed block on write and commits the file at `close` (or `fsync`), so a power cut leaves the old or the new content | the blob is read, patched in memory, written to the temporary file and renamed over the blob, as `write()` |
| Status | the `storage status` console command; the mount and the blob count with the free space in the log | the blob count in the log |

### The storage partition

`hc_storage` owns the one mount of the LittleFS partition `storage` (204 KB on the 4 MB board: 312 KB before the three Zigbee partitions, 268 KB before the core dump partition;
`doc/ota.md`) at `/storage`: `storage_mount()` mounts it read-write on the first call and returns
the first result on every later one; the web server (removing an old UI image) and the blob store
both call it, in either order. It never formats: a partition without a file system (no `storage.bin`
flashed, an empty LittleFS image) or a damaged one stays unmounted and the blob store refuses,
instead of an empty file system hiding the fault. Layout:

| Path | Content | Written by |
|---|---|---|
| `/storage/ui` | the web UI of firmware before 2026-11; removed at start (the UI is in the firmware, `doc/web.md`) | flashing an old `storage.bin` |
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
refused and logged) with quality **good**, `updated_ms` 0 and no listener notification (the
restore is part of the load; rules start afterwards and read it like any good value). `persist`
states that the last value counts, for every datapoint whatever its technology (homecontrol #100):
an application value or a group address only the controller writes is never confirmed by anybody,
and a stale restore made each of them *no value* for the rules until someone wrote it again. Where
the bus knows better, the datapoint has `readOnStart`: the technology reads it when it starts (KNX
when the tunnel comes up, Zigbee when the device is refreshed), and the answer replaces the restored
value. A value the model already has when the store loads (kept across an image swap, or reported
before) is never overwritten: the record then gives only its wall time. `stale` keeps its own
meaning, a technology that lost contact (`report_stale()`): a reboot is none. The record's wall
time is what `wall_time(id)` returns until the next update. Records of datapoints no longer
flagged (or no longer in the image) are deleted at load; a record whose name field does not match
the datapoint (a hash collision) is ignored.

**Initial values** (homecontrol #107, `spec/runtime-image.md` 3.3, format 1.2). After the store has
loaded, `ImageService::apply_initial_values()` gives every application datapoint with an initial
value that still has none (no value kept across a swap, no record, or a record whose type no longer
converts) that value through the same `DatapointModel::restore()`: quality good, `updated_ms` 0, no
listener told. So the store does not write it either; its first record is the first real change. A
persisted value therefore wins over the initial value at every restart, and a datapoint without
`persist` starts with its initial value again at each one.

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
| history budget (RAM) | stated by the image; at most `HistoryLimits::cap_bytes` (64 KB default, the C6) |
| archive budget (flash) | stated by the image; at most `HistoryLimits::archive_cap_bytes` (64 KB default, the C6) |

The budget counts the RAM, 8 bytes per sample and the archives' open and pending slots ("Archives"
below), which E-Home computes the same way on its side. `load()` refuses with the reason in the
log, keeping the previous rings, when:

- the image's budget exceeds the cap ("the image asks for 70000 bytes of RAM, the device allows at
  most 65536", "... bytes of flash for archives ..."), or the rings and archives exceed the budget
  ("the rings and archives need 1456 bytes of RAM, the image's budget is 976", "the archives need
  49152 bytes of flash, the image's archive budget is 45056") — `budget_exceeded`;
- a spec names a datapoint without `kHistory`, or a `kHistory` datapoint has no spec —
  `flag_mismatch`;
- the datapoint is not boolean, unsigned, signed or real — `type_error`;
- spacing, retention or the sample count is out of range, or a datapoint has two specs, or an
  archive is out of its limits ("Archives") — `malformed`;
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

`query(id, from, to, points, sink, answer)` (issue #56) is what the API's history answers with: it
chooses the store (`store_for()`: of the raw ring and the archives whose resolution is at most the
bucket width, the finest that reaches back to `from`, else the one that reaches furthest, else the
ring) and gives each non-empty bucket to the sink: last, min, max, count and the time-weighted mean
(a raw bucket also counts the value held into it from the sample before), a boolean's seconds on.
An archive slot belongs to the bucket of its middle; one that reaches into the range with its
middle outside (the open slot of a range up to now) to the nearest bucket.

### Saving and restoring

Rings with new samples are saved every `save_interval_s()` and by `flush()`; one blob per ring,
named `h.` + 8 hex digits of the FNV-1a hash of the datapoint name. The interval is
`max(min_save_interval_s, sample bytes * 3600 / save_bytes_per_hour)`: 15 minutes by default, longer
for a large history, so at most 64 KB of samples are written per hour (the wear budget below). A
power cut loses at most the samples of one interval; an orderly shutdown loses nothing.

`load()` restores each ring from its blob and merges it into the series: the newest samples that
fit the (possibly smaller) ring, strictly increasing in time, at least the ring's spacing apart, and
— when the wall clock is known — none from the future and none older than the retention; samples
taken after the restore continue the series. A ring of the same spacing keeps every sample (they are
never closer); a coarser ring of a new image keeps one sample per spacing over its whole retention:
24 h at 1 min restored into 24 h at 5 min keeps the day, not its last 289 minutes (homecontrol #115). A blob of another datapoint (hash collision), of a changed type, with a bad checksum or
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

## Consolidated archives (issue #56)

Besides its raw ring, a datapoint's history has up to 4 archives (`spec/runtime-image.md` 3.3): slots
of `resolution` seconds kept for `retention` seconds, `ceil(retention / resolution) + 1` slots (at
most 2048), the resolution 60 s .. 31 days and above the ring's spacing, the retention up to 10
years. They serve the long ranges of the charts (`spec/api.md` "history").

### The slot

32 bytes, no padding (`hc::ArchiveSlot`):

| Bytes | Field | |
|---|---|---|
| 0..7 | `double sum` | Σ value × seconds held; the mean is `sum / held_s`, a boolean's seconds on are `sum` |
| 8..11 | `uint32 start` | the slot's start, UTC seconds: the slot describes itself |
| 12..15 | `float min` | |
| 16..19 | `float max` | |
| 20..23 | `float last` | |
| 24..27 | `uint32 count` | samples fed, keep-alives included |
| 28..31 | `uint32 held_s` | seconds of held value in the slot |

A slot with `count` and `held_s` 0 is empty (no sample, not zero) and is never returned.

**Feeding.** Every sample the ring takes (a change or a keep-alive) feeds the open slot of each
archive: min, max, last, count; the previous sample's value counts as held from its time up to the
new sample, at most its spacing plus 10 s (`kHoldSlackS`), split at slot boundaries. The tick (5 s)
credits the held value up to now and closes the slots that ended. A value that is no longer good
ends the hold at once (the time after it is a gap); while the wall clock is not synchronized nothing
is sampled, so nothing is fed or held. When the clock goes back, the archives ignore samples before
the time they already hold (the ring drops its "future" samples as before); a jump forward closes
the open slot and opens the next one, the time between stays empty. A resolution of whole days is
aligned to the local days of `settings.tz` (local midnight found through the time zone, so a DST day
has 23 or 25 hours); any other resolution to multiples of it in UTC. Integers are kept as floats in
min, max and last: exact up to 2^24.

### The storage decision: open slots in RAM, closed slots on flash

The archives' slots would not fit in RAM (a year of hourly slots is 280 KB), so only each archive's
**open slot** and a queue of slots closed since the last save (**pending**, up to an hour of them,
`kPendingS`) are in RAM. The closed slots live on flash in **segments**: blobs of a 32-byte header
and 127 slots, exactly 4096 bytes, one LittleFS block. Slot number n (counted from the epoch in
resolutions, or in local days) is at position `n % (segments × 127)`, which keeps at least the
retention; a stale slot of a position (from a previous round) is recognized because its start is
not the start of the slot asked for. A slot is written in place with `BlobStore::patch()`, so a
save writes one block per archive, not the whole archive. A segment is created on its first use
(the header and empty slots), not at load: an archive of a year occupies its blocks only as the
year fills them.

| | RAM (`history.budget`) | Flash (`history.archiveBudget`) |
|---|---|---|
| per archive | `(ceil(3600 / resolution) + 2) × 32`: the open slot, an hour of pending slots and the one closing meanwhile; 96 bytes from a resolution of an hour on, 2 KB at 60 s | `ceil(slots / 127) × 4096` |
| cap on the C6 | 64 KB with the rings (`HistoryLimits::cap_bytes`) | 64 KB (`archive_cap_bytes`): 16 blocks |
| hourly for 31 days (745 slots) | 96 | 6 segments, 24 KB |
| daily for 2 years (731 slots) | 96 | 6 segments, 24 KB |
| hourly for a year (8761 slots) | refused: more than 2048 slots | |
| daily for 10 years (3651 slots) | refused: more than 2048 slots | |
| a temperature as E-Home derives it for charts up to a year (ehome#73) | 968 | 9 segments, 36 KB |

Measured free flash on the C6 (4 MB board, `storage` 208 896 bytes = 51 blocks): 45 056 bytes used
and **163 840 free (40 blocks)** with a 23 KB image, its chunks, the value records and the rings of
the test system. The flash cap of 16 blocks leaves 24 for the image's second generation (an update
writes the new chunks before it drops the old), the language packs and the rings. A load whose
segments still to be created exceed the free space logs a warning ("history: the archives need N
more bytes of flash as they fill, M are free") and goes on: a segment that cannot be created later
keeps its slots pending until the queue is full, then drops the oldest, logged.

### Segment format (frozen)

Blob `a.<hash>.<resolution>.<segment>`: the FNV-1a hash of the datapoint name in 8 hex digits, the
resolution in seconds and the segment index in hex (`a.1c2d3e4f.e10.0` is the first segment of an
hourly archive). Little-endian:

| Bytes | Content |
|---|---|
| 0..1 | Magic `H` `A` |
| 2 | Format generation, 1 (a reader drops an archive of a higher one) |
| 3 | Slot size, 32 |
| 4..7 | Resolution, u32 seconds |
| 8..11 | Retention, u32 seconds (informative) |
| 12..15 | FNV-1a hash of the datapoint name |
| 16 | `ValueType` number |
| 17 | Flags: bit 0 local days |
| 18..19 | Segment index, u16 |
| 20..21 | Segments of the archive, u16 |
| 22..23 | Slots per segment, u16 (127) |
| 24..27 | Reserved, 0 |
| 28..31 | CRC-32 of bytes 0..27 |
| 32.. | 127 slots |

The slots have no checksum (they are patched one by one); a slot is used only if it is plausible:
its start is the start of its number, its numbers are finite, min <= max, and `held_s` at most the
resolution plus two hours. A fixture test builds a segment by hand and reads it back
(`HistoryArchive probe: a segment written by hand is read`).

### Load, swap and restart

`load()` checks the segments of every archive of the image: an archive whose segments have the same
layout (resolution, type, slots per segment, number of segments, local days) is kept, a changed
retention of the same layout too ("the 3600 s archive keeps its slots (retention ..., same
segments)"; its headers are rewritten); otherwise the archive is
dropped with the reason in the log ("the 3600 s archive is dropped: its segments are damaged",
"... the datapoint's type changed", "... its layout changed", "... the image no longer has it") and
its segments deleted. When a slot opens it is taken up from pending or from flash if a slot of the
same start is there: after a restart (the slot was saved) or a swap with the same spec the open slot
goes on. A swap flushes the old history first; when the last sample is still fresh, the new
archives hold its value on from the swap.

### A new archive starts with what was kept (homecontrol #115)

A new image may dimension the stores otherwise (haumacher/ehome#221: archives of 3 h, 12 h, 7 days
and 31 days where hourly and daily ones were derived from the charts before). An archive none of whose
segments is on the store takes the slots its window already has data for: each slot of the window from
the finest source that reaches back to its start, else from the one that reaches furthest — the ring's
blob (its samples at the spacing they were taken at, held as `feed()` and `hold()` hold them) or an
archive of the datapoint on the store finer than the new one (its valid slots whose middle lies in the
slot, summed: min, max, last, count, the sum and the held seconds). The window ends at the **cut**: the
load, or, when the clock was not synchronized at the load, the ring's last sample, or without one the
first sample taken once the clock is known; without the time the carry-over waits for it and keeps
the old archives ("history of 'x': the carry-over into the new stores waits for the time"). The
segments with a filled slot are written whole ("history of 'x': the 10800 s archive starts with 57
slots consolidated from what was kept before"). Sources of another type or datapoint are not read (the
checks of the ring blob and the segment header). A changed layout of the same resolution (another
number of segments) is still dropped, and the ring then seeds the new archive with what it holds.

### Carrying a history over: in steps, through the queue (homecontrol #118)

On the desk board the carry-over of one datapoint (26 + 9 + 2 slots) held the loop for about 1.5 s in
the apply step of a deploy, its segments written on the loop because the write queue was full; 20
recorded datapoints would have held it for half a minute, longer than the KNX tunnel's 1 s
TUNNELING_ACK. Now `load()` only decides which archives get a carry-over (and does not read their
sources); the consolidation runs afterwards on the event loop (`core/src/history_carry.cpp`):

- **Steps.** One datapoint after the other, in phases that a step leaves and the next takes up where
  it stopped (the ring blob, the cut, the sources' headers and how far back each reaches, the new
  archives' segments slot by slot, the newest segment first). A step makes at most
  `History::kCarryStepWork` (32) store calls and stops after `kCarryStepMs` (50 ms) once it has made
  one; it checks the budget before each call, with the most calls the next piece can make (a slot read
  through a segment change is 3, a ring search 13). The next step follows after `kCarryGapMs` (10 ms):
  the loop sleeps, so the flash worker and the idle task run. On the host the bound is counted in
  store calls (`HistoryCarryStats`, `carry_stats()`), not in time. The steps are the loop's: stepping
  in the flash worker instead would hold the worker for the whole consolidation (every other writer's
  change would wait in the queue until it is full and then go to the flash on the loop, which is what
  #101 removed), and the worker would read the stores beside the loop's live writes to the same
  datapoint's archives.
- **Through the queue, with back-pressure.** A new segment (4 KB) is written only when
  `BlobStore::has_room()` says the queue takes it, one at a time; a full queue makes the step look
  again after `kCarryPollMs` (20 ms) instead of falling back to a write on the loop. That fallback
  (below "Writing off the loop") stays for the other writers: it is what keeps a write that has no
  room from being lost, and what writes a blob larger than the arena.
- **Durable before anything goes.** Before the first new segment of a datapoint a **journal**
  `ac.<hash>` names its new archives; it must be durable (`BlobStore::settled()`: the queue performed
  it, and it reads back) before a segment is written. Each segment is read back once settled (CRC of
  the 4 KB against what was written). After the last one the journal is removed (settled, gone), and
  only then the archives the image no longer has. A write the queue drops (a store that fails) shows
  in the read-back: nothing is deleted, the datapoint starts again after `kCarryRetryMs` (30 s), logged
  once ("carrying it over into the new stores failed (...); it starts again in 30 s, what was kept
  stays").
- **While it runs.** The datapoint's live samples go into the ring and the new archives' open and
  pending slots as always; the carry-over's slots end at the cut and the live ones start there, so
  only the slot of the cut has both: when the carry-over writes it, the live slot (open or pending)
  takes what was kept (min, max, count, sum and held seconds added, `last` the live one), once
  (`Archive::merged`). Until the carry-over of the datapoint is done its ring blob and its archives
  are not saved by the periodic save (they are what it reads, and a new archive's slots on the store
  are not this load's), and a slot opening in a new archive is not taken up from the store (except the
  slot of the cut once written). A pending queue full after an hour (only a store that keeps failing
  gets there) drops its oldest slot as before. A **query** meanwhile answers from what is there: a
  new archive's slots carried over so far (the newest first) and the live ones, empty where the
  carry-over has not arrived yet; the stores that did not change answer as always.
- **A restart or another load in the middle.** `flush()` (before a restart, and at the start of every
  `load()`) saves the rings, with their live samples, and the archives that are not being filled, and
  ends the carry-over. The next `load()` reads the journal: an archive it names is carried over again
  from the beginning, whatever of it is on the store (written again whole; a segment the new run does
  not fill keeps slots of the old one, which lie outside the window or are the same, since its sources
  are the same), and none it names is a source. An archive it names that the image no longer has is
  deleted first (and that is durable before a new journal replaces the old one). The old archives of
  a datapoint with a carry-over are not deleted at the load. So after a power cut at any moment the
  store holds the old archives complete, or the journal is gone and the new ones are complete; the
  restart ends in the same archives as a carry-over that ran through (the test cuts the power after
  every loop turn). A ring saved by such a `flush()` holds the samples at the new spacing: a carry-over
  done again after a planned restart takes the ring's part at that spacing (the old ring's finer
  samples are gone), never a sample twice.
- **The load itself.** `load()` no longer asks for every possible segment of every archive (17 each):
  one walk of the store tells which segments exist, and the rings are restored through one 2 KB buffer
  per load, a few calls a ring instead of one per 256 bytes. 20 recorded datapoints with rings of
  1440 samples and four archives each: about 320 store calls in the apply step, from about 1900.

Tests: `core/test/history_migration_test.cpp` (what is carried over), `core/test/history_carry_test.cpp`
(20 datapoints behind a write queue of the C6's size, a full queue, a power cut after every turn,
another image in the middle, a store that refuses, live samples during the carry-over).

### Journal format

Blob `ac.<hash>` (the FNV-1a hash of the datapoint name, 8 hex digits), little-endian: magic `H` `J`,
the format generation 1, the number n of new archives (at most 4), the hash (u32), n resolutions
(u32 seconds), and the CRC-32 of everything before it: 12 + 4n bytes. A journal that cannot be read
(another generation, a bad checksum) makes every archive of the datapoint with segments count as
unfinished. A journal of a datapoint without a carry-over is deleted at the load.

### Saving

At every save (`save_interval_s()`) and `flush()`, each archive writes its pending slots and its
open slot if it changed, as patches of up to 32 chunks into the segments they belong to (the gaps
between them written as empty slots). A full pending queue (a save interval longer than an hour, a
store that failed) writes that archive at once; if that fails too, the oldest pending slot is
dropped (logged once, `slots_dropped()`). A failed save is logged ("cannot save the 3600 s archive
of '...' (...): ...; retrying at the next save", for a store without `patch()` "the blob store cannot
patch") and retried; "saving the archives works again" when it does. The tests' `FakeBlobStore`
counts the patches and can refuse them (`patchable`).

### Memory

Allocated at load beside the rings, nothing in steady state: the archive table (96 bytes per
archive on the C6) and the pending blocks (the budget above). The query reads slots through a
window of 16 slots (512 bytes on the stack); a save writes from the slots in place. A load restores
the rings through a buffer of 2 KB, freed when they are restored. A carry-over (#118) holds its state
on the heap while it runs, in no block above 3 KB (#71): about 1.6 KB from the load (the sources, a
window of the ring and of a source) and pages of 32 jobs of 48 bytes, one job per datapoint it carries
over, and from its first step a segment's slots in two blocks of 2 KB (also the scratch of the ring's
checksum and of the read-back). Without the first, nothing is carried over or deleted and the next
load tries again; without the second, the step tries again after 30 s (both logged).

## The flash wear budget

Only settings and values the controller manages, which a restart would lose, go to the flash, and
only when they change (homecontrol #102): the settings, the parameter and enable overrides of rules
and blocks, the inhabitant's timelines and the schedules' records, the rule states, the datapoints
with `persist` (this store), the blocks' managed state (`spec/blocks.md` B4: of the built-in types
only the house mode's mode) and the history the image asks for. Nothing internal is written
periodically: neither a block's recomputable state (an integral, a filter) nor an uptime record.

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
- **An archive patch** copies the segment's one block and commits the file: assumed as the value
  record's commit, **1 + 1/8 erases per patch** (1 + 1 with the rings' margin).
- **The save interval** is `max(min_save_interval, (ring bytes + 4096 × archives) × 3600 /
  save_bytes_per_hour)`; beyond an hour the archives write hourly anyway (their pending queue), so
  the rings are paced on what the archives leave of the 64 KB an hour, at least a quarter of it.

At the limits:

| Writer | Per hour | Erases per hour |
|---|---|---|
| Value records: 60 writes | 60 × 1/8 | 7.5 |
| History: 64 KB of samples (e.g. five 24 h rings) | 16 + 5 | 21 |
| **Total** | | **28.5** |

28.5 erases per hour × 87,600 hours = 2.5 million erases over 35 blocks = about **71,000 cycles per
block, 71 % of the endurance** after ten years of continuous worst case. The blocks' records are no
longer a writer of the budget: the house mode writes its record when its mode changes, a few times a
day (well under 0.1 erases an hour). The guard of `spec/blocks.md` B13 (30 record writes in any hour)
bounds a type that would mark a field that changes all the time; used to the full it would add 3.75
erases an hour (81 %). Before homecontrol #102 the records were paced to that limit and written
whenever an internal field changed, which the room controller's integral did at every step: the
budget then was 32.25 an hour, 81 %.

With archives at both caps (64 KB of rings, 16 archives of one segment each, so 128 KB to write):
the rings are saved every 4 hours, the archives hourly.

| Writer | Per hour | Erases per hour |
|---|---|---|
| Value records: 60 writes | 60 × 1/8 | 7.5 |
| History rings: 64 KB every 4 hours | (16 + 5) / 4 | 5.25 |
| Archives: 16 patches | 16 × 1.125 | 18 |
| **Total** | | **30.75** |

2.7 million erases over 35 blocks: about **77,000 cycles, 77 %** (86 % before homecontrol #102 with
the blocks' records at their limit) (with the rings' margin of a whole erase per commit 41 an hour,
103 % over 35 blocks and 90 % over the 40 measured free). An image as
E-Home derives it for a few charts writes a small fraction: an hourly archive patches its block
once per save while its slot changes. A real house writes far
less: setpoints and states change a few times a day, and the history budget of a typical image is
a fraction of the cap.

The Zigbee device database (homecontrol #111, doc/zigbee.md "Partitions") joined the store from its
own NVS partition. It is written when a device joins, is interviewed, renamed or removed, when a
short address changes or a receiver is learnt (at most one write per check of 10 s, events of a few a
day), and for the devices' last-seen times about once an hour (`ZigbeeAdapter::kSeenSaveS`). Assumed:
**one write an hour** of the largest database, 30 devices of about 15 KB: four data blocks
(`littlefs_footprint()`) and a metadata commit, **4.125 erases per hour**. With it the two worst
cases above are 32.6 and 34.9 erases an hour: about **82 % and 87 %** of the endurance over 35
blocks. The house controller's two devices are about 1 KB, one block per write. On NVS the same
writes would have worn `zb_db`'s six sectors: a 15 KB save fills about 3.8 pages of 4000 bytes, each
erased again when NVS reclaims it, so 0.63 erases per sector and save, 55,000 cycles (55 %) after ten
years; and a database of more than about 20 such devices could not be replaced there at all (two
copies do not fit its five usable pages).

What a real installation writes is far below these limits. The house controller (firmware 1.5.0,
two room controllers, KNX, a handful of persisted datapoints; measured 2026-11-20 from its log over
30 minutes, haumacher/homecontrol#101), and the same installation on this firmware by its design:

| Writer | Firmware 1.5.0 (measured) | This firmware |
|---|---|---|
| Block state records (`BlockRuntime`, LittleFS) | about one write a minute (`on_persist` every 60 s, 26-44 ms each, 143-182 ms every 8-12 min) | none (the room controllers have no managed state); at the first start the two old records are deleted |
| Uptime record (`EspDiagnostics`, NVS `diag`) | one entry and commit a minute (43 ms) | none; the boot record once per start, the old key erased with it |
| Value records (`ValueStore`, LittleFS) | on change of the persisted datapoints, at most one per datapoint a minute (35-41 ms) | the same |
| History, archives, rules, schedules | as the image asks (none seen in the half hour) | the same |

The device counts its writes per writer since boot (`GET /api/system/diagnostics`,
`storage.writes`, `spec/api.md`), so the right-hand column is checked on the installation: an hour of
operation with the two room controllers shows `block_records` 0 (the acceptance of #102).

The uptime record was the one periodic writer of the `nvs` partition: 1440 entries a day, about two
erases of each of its six pages a day. NVS now changes only with the settings, the overrides, the
rule states, the schedule records and the boot record (once per start).

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
budget (counted writes), the refresh rule, restore with good quality and the saved time, a
datapoint without a value staying unknown, a record with an unknown field restoring, a newer
generation and a bad checksum ignored, every value type through encode and decode, a changed type,
orphan deletion, flush and the image swap, failing stores; the ring capacity, the budget and cap
refusals, spec validation, sampling with spacing and keepalives, gaps, the ring wrap, retention,
decimation (min, max, last per bucket, never more than `max_points`), the save interval, save and
restore across a reboot and an image swap, a blob with unknown header fields and larger samples, a
damaged blob, a clock going back. `core/test/persisted_restore_test.cpp` (homecontrol #100) restarts
the image service on the same blobs, slot and Zigbee database: a persisted application value, a KNX
datapoint without and one with `readOnStart` and a Zigbee one come back good with their saved
values, a start rule `write x = if(@setting, 100, 80)` writes 100, the read on start's answer replaces
the restored value, a tunnel loss still makes them stale; and a report before or after the store's
load wins over the record. Rule corpus case 102 pins the same for both interpreters.

`core/test/history_archive_test.cpp` (issue #56) runs the archives the same way: the budget
arithmetic as E-Home computes it, three days at 1 min into an hourly archive (min, max, last, the
time-weighted mean), a boolean's on-time in daily slots across a DST change, the choice of the
store, the budget, cap and limit refusals, a restart and a swap keeping the archives with the open
slot going on, a changed resolution, type or layout dropping them (logged) and a changed retention
keeping them, gaps for a stale value and an unsynchronized clock, the save interval, a store
without `patch()` and the pending queue, a hand-built segment of the frozen format and a damaged
one, the clock going back and jumping forward. `core/test/web_api_test.cpp` checks the API's
`store`, the long form with `on_s` and the truncation.

`platform/posix/test/file_blob_store_test.cpp` runs the POSIX store against a temporary directory:
the round trip with chunks and reads at offsets, replace and remove, `visit()` skipping foreign
files, a crash's temporary files ignored and deleted while the old blob is still served, a blob
named `x.tmp`, the name and size refusals before anything is touched, an unusable directory refusing
every call (logged once), the default directory, the streaming write (a 50 KB blob in 1 KB pieces:
the temporary file is not listed and the old content is served until the commit, `busy`, `discard()`
keeps the old blob, the free-space refusal), and `patch()` (in place, the size kept, refused past
the end and without the blob). `core/test/blob_writer_test.cpp` runs the same for
`MemoryBlobStore` and the fake. `no_space` from the file system is not tested (it needs a full file
system). The ESP store shares the algorithm; it is verified on the board (`storage status`, a
reboot keeps the values).

`core/test/blob_queue_test.cpp` runs the write queue (homecontrol #101) with the stepped
`ManualWorker`, and its `write_from()` (homecontrol #111): the source asked once, when the worker
takes the write, a waiting one replaced, reads, sizes and visits answered from the source
meanwhile, a refusing source retried and dropped like a failed write, the fallback when the entries
are full, the pass-through without a worker. `core/test/zigbee_database_store_test.cpp` runs the
Zigbee device database behind the queue: a save of 30 devices (15 KB, more than the arena) written by
the worker and none on the loop, saves meanwhile holding one buffer of the JSON at most and none
between the attempts of a failed write, the database going while its save waits, the adapter's
interview and address change written by the worker and read by the next start; and the move out
of the previous store: verified and removed there, a failed write or a copy that reads back
different keeping the old blob (and loading it), the next start moving it, a newer blob in the new
store winning, a damaged new blob leaving the old one loaded, a damaged old one not moved.
