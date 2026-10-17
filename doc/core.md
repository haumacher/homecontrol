# The core and its event loop

The portable core (`core/`, namespace `hc`) is plain C++17 without exceptions, RTTI, iostreams or
any platform include. What it needs from the outside is an interface under
`core/include/hc/platform/`: `Clock` (monotonic milliseconds, wall time with a "synchronized"
flag) and `SystemInfo` (free heap figures); the log sink is `hc::LogSink` in `hc/log.h`. The
platforms implement them: `hc::posix` in `platform/posix/`, `hc::esp` in `platform/esp/main/`.

## Controller

`hc::Controller` is what a platform boots. Its constructor takes the clock, the log sink (which it
installs for `hc::Log`) and the system info; `start()` logs the version, the build id, the wall
clock state and the free heap, and schedules a heartbeat every 10 s (uptime and free heap). The
controller owns the `hc::EventLoop`; later packages hang their work onto it.

## Event loop model

- **Single-threaded.** Every `EventLoop` method is called on the loop's thread; handlers may
  schedule, cancel and post, but must not call `run_once`. Posting from another thread (an ISR,
  a network task) is not supported yet; the first adapter that needs it adds a platform-provided
  thread-safe queue with a wakeup.
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
runs the loop in the FreeRTOS task `hc_loop` (6 KB stack, priority 5) and sleeps with
`vTaskDelay`, rounding up to the tick (10 ms at the default 100 Hz). The 1 s cap is a safety net
for a missed wakeup, not part of the timing contract.

## Tests

The core's unit tests (`core/test/`, doctest vendored in `third_party/doctest/`) run on the host
with a fake clock and a capturing log sink; each test case is its own CTest entry
(`ctest --test-dir build -R EventLoop`).
