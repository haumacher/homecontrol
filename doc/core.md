# The core and its event loop

The portable core (`core/`, namespace `hc`) is plain C++17 without exceptions, RTTI, iostreams or
any platform include. What it needs from the outside is an interface under
`core/include/hc/platform/`: `Clock` (monotonic milliseconds, wall time with a "synchronized"
flag), `SystemInfo` (free heap figures), `Settings` (small persistent named values), `TimeZone`
(applies a POSIX TZ string, converts to local time), `NetworkDriver` (connect, disconnect, pair),
`StatusLight` (the status LED), `ImprovPort` (the serial port an Improv Wi-Fi client talks to) and
`WifiScanner` (a WiFi scan); the log sink is `hc::LogSink` in `hc/log.h`. The platforms
implement them: `hc::posix` in `platform/posix/`, `hc::esp` in `platform/esp/components/hc_platform`
and `hc_net`. The platforms hand them to the controller bundled in an `hc::Platform`.

## Controller

`hc::Controller` is what a platform boots. Its constructor takes the `hc::Platform` (and installs
its log sink for `hc::Log`); `start()` logs the version and the build id, applies the time zone
setting, logs the wall clock state and the free heap, schedules a heartbeat every 10 s (uptime,
network state, free heap), watches the wall clock every second until it is synchronized and then
logs the local time once, drives the status LED every 50 ms (if the board has one) and starts the
network. The controller owns the `hc::EventLoop`, the `hc::Connectivity` (the network workflow,
`doc/network.md`) and `hc::Improv` (WiFi provisioning from the web flasher, active only when the
platform supplies an `ImprovPort`); later packages hang their work onto them.

## Event loop model

- **Single-threaded.** Every `EventLoop` method is called on the loop's thread; handlers may
  schedule, cancel and post, but must not call `run_once`. Work from other threads reaches the
  loop through the platform: the ESP build's `hc::esp::LoopQueue` (a FreeRTOS queue the loop task
  sleeps on) carries the IDF's WiFi/IP events, the console's commands and the Improv frames onto the loop thread,
  where they call into the core. The core itself stays single-threaded.
- **Timers** live on the monotonic time base (`Clock::monotonic_ms`). `schedule_at(deadline)` is a
  one-shot; `schedule_periodic(first, period)` repeats at `first + k * period` without drift. If
  the loop falls behind by more than a period, the missed periods are skipped (warning logged) and
  the timer stays on its grid: a periodic timer fires at most once per `run_once`.
- **Handles** (`TimerHandle`) carry a generation: cancelling an already fired or cancelled timer
  returns false and can never hit the timer that reused its slot. A timer cancelled before its
  handler ran does not run, even when it was already due in the current `run_once`. A periodic
  timer may cancel itself in its handler.
- **Posted callbacks** (`post`) run in FIFO order.
- **Callbacks** are a function pointer plus a context pointer (`Callback::member<T, &T::m>(obj)`
  for member functions): nothing is allocated when work is scheduled.

### Ordering guarantees

`run_once(now)` processes exactly the work that was due when it was entered:

1. the callbacks posted before entry, in FIFO order;
2. the timers with `deadline <= now` that were scheduled before entry, ordered by deadline and,
   for equal deadlines, by scheduling order (a periodic timer counts as scheduled when it was
   last re-armed).

Work a handler creates — a post, or a timer that is already due — runs in the *next* `run_once`,
never in the current one. This bounds every call (a handler that re-posts itself cannot starve
the platform) and makes the order independent of how the platform batches calls.
`next_deadline()` reports such work as due immediately, so the platform does not sleep.

### Capacities

Statically allocated inside the `EventLoop` object, no heap use:

| What | Constant | Value |
|---|---|---|
| Timers scheduled at the same time | `EventLoop::kTimerCapacity` | 32 |
| Posted callbacks waiting | `EventLoop::kEventCapacity` | 32 |
| Log message length (incl. NUL) | `Log::kMaxMessage` | 256 |

When a capacity is exhausted the call is refused: `schedule_*` returns an invalid handle, `post`
returns false, and an error is logged naming the capacity. Longer log messages are truncated and
end in `...`.

## How a platform drives it

```
controller.start();
for (;;) {
	controller.run_once();                       // run_once(clock.monotonic_ms())
	uint64_t now = clock.monotonic_ms();
	uint64_t deadline = controller.next_deadline(); // 0: work pending; EventLoop::kNever: none
	if (deadline > now)
		sleep(min(deadline - now, 1000 ms));
}
```

POSIX sleeps with `nanosleep` (interrupted by SIGINT/SIGTERM, which stop the loop); the ESP build
runs the loop in the FreeRTOS task `hc_loop` (9 KB stack, priority 5) and sleeps in
`LoopQueue::dispatch()`, a `xQueueReceive` with the timeout rounded up to the tick (10 ms at the
default 100 Hz), so a message from another task wakes it at once. The 1 s cap is a safety net
for a missed wakeup, not part of the timing contract.

**Load figures** (issue #42, the System page): the loop counts the periods its periodic timers
skipped since boot (`missed_periods()`, each skip also a warning) and keeps its longest turn with the
time it began (`longest_turn_ms()`, `longest_turn_at_ms()`), measured on the clock the Controller
hands it (`set_clock()`). A turn is one `run_once`, or a batch of messages the platform runs on the
loop's thread outside it (`account_turn()`: the ESP's `LoopQueue::dispatch()`, where web requests and
console commands run; POSIX's web jobs and KNX datagrams after its `poll()`). A handler that holds the
loop longer than 25 ms is logged with the address of its function (`slow timer|periodic|posted
handler 0x… took N ms`, and `slow message handler` for the ESP's `LoopQueue`): `addr2line -C -f -e
homecontrol.elf 0x…` names it, typically `Callback::invoke_member<hc::X, &hc::X::method>`. That is
how #50 found the scheduler's 333 ms evaluation (the sun computed for ten dates per astro entry,
now cached by the TimeService, and since #57 computed only for the two or so dates that can hold
the next firing), and #55 the live feed's `pump` holding the loop 111 and 164 ms when
E-Home joined: the E-Home client wrote each snapshot chunk into the TLS socket on the loop (about
6 ms of CPU each, and about 60 ms waiting for the server's ACK when lwIP's send buffer was below its
low-water mark); it now queues and a task of its own writes (doc/ehome.md, "The platforms").
On the device no handler on the loop writes to a WebSocket: both transports queue (the POSIX build
writes synchronously into the kernel's socket buffer).

## Tests

The core's unit tests (`core/test/`, doctest vendored in `third_party/doctest/`) run on the host
with a fake clock and a capturing log sink; each test case is its own CTest entry
(`ctest --test-dir build -R EventLoop`).
