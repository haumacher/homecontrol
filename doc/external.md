# External values: weather and other HTTP services as datapoints

Issue #17. `hc::ExternalValues` (core, `core/include/hc/external_values.h`) is a technology that
owns application datapoints fed from HTTP JSON sources: the weather from Open-Meteo, a PV
inverter's local API, any service that answers a GET with JSON. The sources are configuration (the
runtime image's `external` member, `spec/external-values.md`), not code. Rules and the UI see only
datapoints (ROADMAP: everything is a datapoint): "close the blinds above 25 °C outside" is a rule
on `weather.temperature`.

## Core and platform

| Who | What |
|---|---|
| core: `ExternalValues` | The sources and bindings, when to fetch, the conditions, the backoff, parsing (`json_parse`, `JsonSpan`), extraction, conversion, reporting, the stale rule, the refusals. |
| platform: `hc::HttpFetcher` (`core/include/hc/platform/http_fetcher.h`) | One GET (or POST) at a time: `fetch(url, receiver, error)`. The response streams to the `FetchReceiver` on the event loop's thread, never from within `fetch()` (issue #43): `fetch_begin(status, content_length or kUnknownLength)`, `fetch_chunk(data, size)` per piece of the body, then always one `fetch_end(FetchResult)` (ok, complete, HTTP status, length, aborted, reason); `false` from begin or a chunk aborts. The platform holds no body: every limit is the receiver's. |
| core: `FetchBody` (`core/include/hc/fetch_body.h`) | The JSON consumers' accumulator over a buffer of their own size: refuses an announced or delivered body above it with a reason that names the limit ("the response is larger than 8192 bytes"). |
| core: `FetchQueue` (`core/include/hc/fetch_queue.h`) | One fetch worker for every request (issue #45 item 13, "One fetch worker" below): a `Lane` per consumer, each an `HttpFetcher`, whose requests run one at a time on the platform's fetcher, by priority, each with its own timeout. |
| ESP: `hc::esp::EspHttpFetcher` (component `hc_external`) | esp_http_client with the IDF certificate bundle, a short-lived worker task `hc_fetch` per request (8 KB stack; the timeout the lane gives, `HttpFetcher::request()`). The task reads at most 2 KB into its piece buffer, posts the piece through the `LoopQueue` and waits on a semaphore until the loop ran the receiver (the hand-over of `hc_ehome`'s WebSocket client); the receiver's answer decides whether it reads on. About 3 KB, created on the heap by `OnDemandFetcher` for a fetch and freed 2 s after the end. |
| POSIX: `hc::posix::HttplibFetcher` | cpp-httplib's client on a worker thread; its response handler and content receiver run each receiver call through the web server's `LoopExecutor` (which waits for the loop). HTTPS only when the build finds OpenSSL's development files (CMake prints which); otherwise `https://` is refused with that reason and `http://` works. |
| tests: `FakeFetcher` (`core/test/external_values_test.cpp`), `respond()` (`core/test/fake_fetch.h`) | Records the requests; the test answers them in pieces as the platforms do. |

## When a source is fetched

- **Grid and stagger.** After `configure()` source *i* is first fetched `kStartDelayMs` (5 s) plus
  *i* × `kStaggerMs` (20 s) later, then every `interval` (300..86400 s, default 900) on that grid.
  A source's grid does not drift with the fetch duration.
- **One at a time.** Only one fetch is in flight; a source that comes due meanwhile waits for the
  result and then runs. Several sources never burst onto the network.
- **Conditions.** Nothing is fetched while the network is down, the wall clock is not synchronized
  (TLS checks the certificates' dates) or, for a URL with `{lat}`/`{lon}`, no valid location is set
  (`location.lat`, `location.lon`, `doc/time.md`). A due time that passes so is a *missed* fetch
  (logged at info level with the reason, counted like a failure, no backoff); the fetch follows
  `kStartDelayMs` (plus the stagger) after the condition clears. The conditions are polled every
  `kReadyPollMs` (30 s) only while a source waits for them: the Connectivity's listener table is
  full on the device (KNX tunnel, Improv, OTA, network services), and the time service's should
  stay free for the rule side.
- **Backoff.** A failed fetch is retried after 1 minute, then 2, 4, 8 … up to 60 minutes
  (`hc::Backoff`, `kRetryInitialMs`, `kRetryCapMs`); the first success resets it and puts the source
  back on its grid. A fetch without a result after `kFetchTimeoutMs` (120 s; the platforms time out
  after 15 s) counts as failed; a result arriving later is ignored.
- **On request.** `fetch_now(id)` (console `external fetch <id>`) fetches a source at once, after a
  fetch in flight; refused within a minute of its last attempt (rate limit) and while a condition
  is missing, with the reason.

## A fetch

A failure is anything of: the fetch could not start (busy, URL), no connection or TLS failure, an
HTTP status other than 200 (its body is not read), a body larger than `kMaxBody` (8192 bytes:
refused before a byte when the server announces it, else at the piece that crosses it; "the response
is larger than 8192 bytes"), a body that is not JSON, a path that is missing, an index out of range, a value of the wrong kind
(`null`, an object, a string for a number datapoint, a number out of an integer datapoint's range).
Every binding is extracted and converted before any is reported: a response reports all its values
with quality good, or none (values of one response belong together).

The reason of a failure is logged (tag `hc.ext`) when it differs from the previous one, so an
outage is one line, not one per retry. After `kStaleAfterFailures` (2) consecutive failed or missed
fetches the source's datapoints become stale (`report_stale`, the last value stays) with one log
line; the next success makes them good again and logs "recovered after N failure(s)".

Commands to these datapoints are refused: the model refuses a non-writable datapoint
(`not_writable`), and `ExternalValues::command()` refuses a writable transmitting one with a log line
("an external value … is read only"). A writable datapoint without transmit is refused at
configuration, because the model would store its commands without asking the owner.

## Configuration

`configure(specs, count)` (programmatic) and `configure_json(text, length)` (the image's member) check
everything before anything changes; a refusal logs and returns the reason and keeps the previous
configuration. The datapoints are resolved by name and bound at configuration: call it again after
every model load. A configuration made while a fetch is in flight drops that fetch's result.

The Open-Meteo preset expands into an ordinary source: its URL asks only for the variables the
bindings use, each variable becomes its JSON path and default unit (`spec/external-values.md` lists
them). The location goes out with two decimals (about 1 km): what a weather grid resolves, and no
more of the house's position.

## Memory

`configure()` allocates one block sized for the configuration (issue #44): the table header, the
sources (about 600 bytes each), the bindings (about 170 bytes each) and one pending value per
binding (about 30 bytes). `configure_json()` counts the sources and bindings of the member first.
One source with four bindings takes about 1.4 KB (1480 bytes on the 64-bit host,
`ExternalValues::table_bytes()`, shown by `external status`); the limits (8 sources, 32 bindings)
stay limits and cap the table at the 11 KB it used to take always. The previous table is freed
(during a reconfiguration both exist). Each fetch takes a `kMaxBody` (8 KB) buffer on the heap for
the response while it is in flight (the JSON is parsed as a whole) and frees it at the end. The
`ExternalValues` object itself is about 700 bytes; it uses one event-loop timer. On the device the
service fetches on its lane of the one fetch worker (below): no fetcher exists without a fetch, so
an installation without sources pays nothing; a fetch creates the fetcher (3 KB) and the worker
task (8 KB stack), and esp_http_client with mbedTLS allocates its TLS buffers for the duration of
the request.

## One fetch worker

Every HTTP(S) request of the device runs on one worker (issue #45 item 13): `hc::esp::FetchService`
(component `hc_external`) holds one `OnDemandFetcher` and the core's `FetchQueue` over it, and a
lane per consumer, in this order of priority:

| Lane | Consumer | Timeout |
|---|---|---|
| `image` | the console's `image load <url>` (hc_image) | 10 s |
| `update` | the OTA update check, the manifest and its signature (hc_ota) | 15 s |
| `enrollment` | the E-Home enrollment's POST (hc_ehome) | 15 s |
| `external` | the external values (this document) | 15 s |
| `language` | the language packs (hc_i18n) | 15 s |

A request starts at once when the worker is free and nothing waits (a refusal of the worker, such
as no memory for its task, is then the consumer's refusal as before). Otherwise it waits with a copy
of its URL and body on the heap, and when the worker is free the waiting request of the highest
priority runs, in the order of arrival among equals; each keeps its own timeout on the worker. A
request waits at most 120 s (`FetchQueue::kDefaultWaitMs`; the others' timeouts bound the wait
anyway) and then ends without having run ("waited 120 s for the fetch worker (busy with ...)").
`HttpFetcher::cancel()` (a lane's) drops a waiting request or aborts a running one at its next
piece (the enrollment and the language packs call it when their own timeout gives a request up, so a
request that still waits does not run late); a receiver's
`false` aborts as with any fetcher; every request ends with exactly one `fetch_end()`. Before, each
consumer had its own task (8 KB, the image download's 6 KB) and TLS session, and up to five could
run at once; now the peak is one task stack and one TLS session. The log (tag `hc.fetch`):

```
fetch 'external' waits for the worker (running: 'language'; 1 waiting)
fetch 'external' runs after 840 ms waiting
fetch 'external' not run: waited 120 s for the fetch worker (busy with 'language')
```

The worker's task is `hc_fetch` (the System page's tasks and `/api/system/diagnostics` list it while
a request runs). POSIX runs the same queue over its one `HttplibFetcher` (lanes `enrollment`,
`external`, `language`).

## Device wiring and console

`hc::esp::ExternalService` (component `hc_external`) owns the `ExternalValues` over the `external` lane, wired
to the controller's loop, time service, settings and connectivity and to the datapoint model; the
image loader (issue #8) calls `values().configure_json()`. Console:

```
hc> external status
table <bytes> B, fetch idle
weather: preset open-meteo every 900 s, 12 fetch(es), 0 failure(s) in a row
  url   https://api.open-meteo.com/v1/forecast?latitude={lat}&longitude={lon}&current=temperature_2m&...
  last  ok 312 s ago
  next  in 588 s
  weather.temperature          10.4       °C    good    <- current.temperature_2m
hc> external fetch weather
```

On POSIX the fetcher is built into the web library; the development loop without OpenSSL serves
the captured response locally:

```
python3 -m http.server 8000 --directory spec/fixtures/external
# a source with "url": "http://127.0.0.1:8000/open-meteo-example.json"
```
