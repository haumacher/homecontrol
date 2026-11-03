# Blocks as WebAssembly modules: the spike

This spike (issue #59, measured 2026-11-03) asks whether the ESP32-C6 can run *loaded blocks*,
the virtual devices of the block contract (#60), as WebAssembly modules, and how. The contract
itself is #60. This spike uses only a draft ABI, which is just enough to measure with.

The answer is **yes, with the WAMR interpreter, instruction metering and one module instance per
block type**. The limit is RAM, not flash, and RAM is the same on every C6 board: a loaded block
type costs about 12 KB of heap when it is small and about 40 KB when it is 20 KB of code. The
recommendation is in the ROADMAP (2026-11-03, a recommendation for the author). The measurements
are below.

| Piece | Where |
|---|---|
| Draft ABI (imports `hc.*`, exports `hc_*`) | `spike/blocks-wasm/abi/hc_block.h` |
| Test block: PI room controller in C, Rust and Zig | `spike/blocks-wasm/blocks/{c,rust,zig}/` |
| Fault and benchmark probes; generated 20 KB module | `spike/blocks-wasm/blocks/c/probes.c`, `gen_bulk.py` |
| Built modules (committed; `build-blocks.sh` rebuilds them byte-identically) | `spike/blocks-wasm/wasm/` |
| Recorded input series and expected bits | `spike/blocks-wasm/fixtures/series.csv`, `expected.csv` |
| Block host on WAMR, shared by host and device | `spike/blocks-wasm/runner/spike_runner.{h,cpp}` |
| Host test (WAMR on Linux) | `spike/blocks-wasm/host/` |
| Java test (Chicory) | `spike/blocks-wasm/java/` |
| Measurement app for the C6 | `spike/blocks-wasm/esp/` |

None of this is part of the core build. Nothing in `core/` or `platform/` changed.

## Setup

- **Board:** the development ESP32-C6, 4 MB, at 160 MHz, with flash at 40 MHz, ESP-IDF 5.5. It
  uses the controller's PMP IRAM/DRAM split and the 4 MB partition table.
- **WAMR:** 2.4.5, the unpatched upstream release.
  - The tarball is pinned by SHA-256 `1ab09d51…c199b`.
  - It is used as an IDF component (its repository root is one). The ESP build and the host build
    each download and check it.
  - The configuration has instruction metering, no WASI, no libc builtins, no bulk memory,
    reference types or SIMD, and `WASM_ENABLE_SHRUNK_MEMORY`.
  - On the device, WAMR is compiled at the project's `-Og` unless a figure says `-O2`.
- **Measurement app:** a separate application.
  - It is flashed to ota_0 (0x100000) only. Bootloader, partition table, updater, otadata, NVS
    and storage stay untouched.
  - Its scratch region lies inside ota_0 at 0x280000: a LittleFS of 192 KB, plus 64 KB at 0x2B0000
    for AOT code that runs in place. Restoring the controller overwrites the region.
  - The app runs every measurement once per boot and prints `SPIKE ...` lines.
- **Variants** (`SPIKE_VARIANT`):
  - `both`: the fast interpreter and AOT; this is the app that was run.
  - `interp`, `aot`: for the flash cost of each.
  - `classic`: the classic interpreter, which was also run.
- **Test block:** the room controller, a PI controller.
  - Inputs: 0 temperature, 1 setpoint, 2 enable. Output: 0 valve, 0..100 %.
  - Parameters: xp 2 K, tn 90 min, filter 5 min, fallback 30 %.
  - It integrates over the time between calls, read from `hc.now()`.
  - It filters the actual value with a first-order filter.
  - Anti-windup is conditional integration.
  - Disabled, the valve is 0. Without a temperature, the valve goes to the fallback position.
  - On start with `restored` it keeps the integral and the filtered value.
- **Input series:** `series.csv`, 2211 events over 24 h, generated with seed 59.
  - Reports are off the simulation grid, some are lost and some doubled.
  - A window is open from 8 to 10 h; the block is disabled from 13 to 13.5 h; a sensor fails at
    16 h; the setpoint changes several times.

### Draft ABI

The ABI is in `abi/hc_block.h`. It has no WASI and no libc, and the module is MVP WebAssembly 1.0
(`-mcpu=mvp`).

- **Imports** (module `hc`):
  - `now() -> i64`: monotonic ms;
  - `read(port) -> f64` and `quality(port) -> i32`;
  - `param(i) -> f64`;
  - `write(port, f64)`;
  - `log(ptr, len)`.
- **Exports:**
  - `memory`, `hc_abi`;
  - `hc_instances`: how many state records the module holds;
  - `hc_state(self)` and `hc_state_size`: where instance `self`'s state lies, for persistence and
    diagnostics;
  - `hc_start(self, reason, restored)`, `hc_input(self, port)`, `hc_period(self)`.

Each call names the instance (`self`), so one module instance serves every block of its type
(question 2). The manifest belongs to #60 and is not drafted here.

## 1. Runtime: WAMR, interpreter against AOT

Flash cost of WAMR, from `idf.py size-components` (`libwamr.a`, code and read-only data):

| Configuration | Flash | DRAM |
|---|---|---|
| Classic interpreter only | 66.6 KB | |
| Fast interpreter only | 75.4 KB | 1.1 KB |
| AOT loader only | 42.4 KB | 1.9 KB |
| Fast interpreter + AOT | 106.9 KB | 2.9 KB |
| Fast interpreter + AOT, WAMR at -O2 | 118.2 KB | |

- **ota_0 headroom:** the controller (2,551,408 B on 2026-11-03) has about 260 KB free in ota_0 on
  the 4 MB table, so even the largest configuration fits.

**Where AOT code runs.**

- **From RAM: no.** The PMP IRAM/DRAM split, which the controller uses, leaves no executable heap
  (`MALLOC_CAP_EXEC` free: 0 B).
  - A normal AOT file fails with "allocate memory failed".
  - Turning the split off would weaken the protection of the whole firmware for the blocks' sake.
- **From flash: yes.**
  - `wamrc --xip` writes code without relocations in `.text`.
  - Written to flash and mapped with `esp_partition_mmap(..., ESP_PARTITION_MMAP_INST)`, it runs in
    place.
  - The XIP code is 4.2 KB for the room controller and 34.8 KB for the 20.7 KB module.
  - This needs a flash region that is written at image deploy and mapped for execution. The
    controller has no such region; it would take a partition-table change. The spike borrowed the
    tail of ota_0 for it.

**AOT cannot be bounded.** This decides question 3 against AOT for blocks that anybody may write.
See there.

## 2. Memory per instance

**Custom page sizes: not supported, neither in WAMR 2.4.5 nor in Chicory 1.7.5.**

- `wasm-ld --page-size=1` links such a module.
- WAMR refuses it: "invalid limits flags".
- Chicory refuses it: "invalid memory limit: 9".

**What works instead: a memory smaller than a page, through WAMR's SHRUNK_MEMORY.**

- The module declares exactly one page (`--initial-memory = --max-memory = 65536`), never grows it,
  and exports `__heap_base` and `__data_end`.
- WAMR then allocates the memory only up to `__heap_base`, that is the data plus the C stack.
- With `--stack-first`, the stack lies below the data. A stack overflow leaves the memory and traps
  instead of overwriting the state.
- The room controller's linear memory is 4384 B: a 4 KB stack plus 288 B of data, which hold eight
  32 B state records.
- Chicory allocates the full 64 KB page, which does not matter in Java.

Heap on the C6, measured with WAMR's allocator counted and the heap's free size (fast
interpreter):

| Item | room_controller.wasm, 934 B | probes.wasm, 1536 B | bulk.wasm, 20 756 B |
|---|---|---|---|
| Module, binary kept | 4.2 KB | 7.3 KB | 62.4 KB |
| Module, binary freed after load | 3.8 KB | 6.5 KB | 41.9 KB |
| Instance and execution environment | 8.0 KB | 8.1 KB | 9.9 KB |

- **Load allocations:** the load makes 145 allocations for the room controller and 2052 for bulk,
  all freed at unload.
- **The instance:** it is the linear memory (4.4 KB), the interpreter stack (2 KB, a parameter of
  instantiation) and about 1.6 KB of fixed structures.
- **Classic interpreter:**
  - The module costs 2.6 KB for the room controller and 27.3 KB for bulk (6.7 KB of runtime
    structures plus the 20.7 KB binary, which it must keep).
  - The instance costs 9.1 KB and 10.7 KB.
- **AOT (XIP):** the module costs 5.4 KB of heap whatever its size (5.5 KB for bulk); the instance
  costs 7.7 KB.
- **Runtime init:** 160 B.
- **Steady state:** 0 B (question 5).

**Several blocks in one instance.** One module instance per *block type* holds the state records
of all its instances; the ABI's `self` selects one.

| Strategy, room controller, fast interpreter | Heap |
|---|---|
| One module, one instance | 12.2 KB |
| 8 blocks as 8 loaded modules, each instantiated | 97.6 KB |
| 8 blocks as 8 state records in one module instance | **12.2 KB** |
| 8 blocks in AOT XIP, 8 separate instances | 104.8 KB |

- The host test checks that the 8 records keep their states apart.
- Chicory gives instance 7 the same bits as instance 0.
- The cost of this strategy is that a fault in one block stops every block of its type, because
  they share the memory. The contract's "the instance stays stopped" (#60) then applies to the
  type. Restarting means instantiating the module again, see "What fails".
- **One instance for all block types is not possible.** A module is one compilation unit, and two
  blocks written by different authors cannot share one.

**Native stack.** The loop task's own stack:

- about 3 KB used by the replay;
- 3.4 KB at most in the classic run;
- 7.4 KB at most in the fault test with AOT's native stack overflow.

The controller's loop task has 9216 B.

## 3. Bounded execution

**Interpreters (fast and classic): bounded.**

- `wasm_runtime_set_instruction_count_limit` gives each call a budget.
- The endless loop is stopped with "instruction limit exceeded".
- Afterwards the block refuses every call with its reason, a sibling block keeps running, and
  there is no reset and no watchdog.

| Budget | Fast interpreter | Fast at -O2 | Classic |
|---|---|---|---|
| 10 000 instructions | 4.0 ms | | 2.7 ms |
| 100 000 | 36.6 ms | | 23.3 ms |
| 1 000 000 | 363 ms (2.8 instr/µs) | 285 ms (3.5/µs) | 231 ms (4.3/µs) |
| 5 000 000 | 1.81 s | | 1.15 s |

- **The two interpreters count different things.** The fast one counts its own pre-compiled
  operations; the classic one counts WebAssembly opcodes.
  - One PI step of the room controller needs 78 instructions on the fast interpreter and 164 on the
    classic one, both found by bisecting the budget on the device.
  - A budget therefore has to be stated per interpreter, or the device has to convert it.
- **Time per call is linear in the budget.** A per-call budget of 20 000 classic instructions
  bounds a call to about 5 ms; the PI step uses less than 1 % of that.

**AOT: not bounded.**

- WAMR's metering exists only in the interpreters.
- `wasm_runtime_terminate` from another thread does not stop AOT code that is in a loop. The
  reason is in the source: in 2.4.5, the AOT compiler emits the suspend-flag check only for
  modules with shared memory.
- `host/thread-mgr/aot_terminate_test.cpp` shows it on the host: the loop is not stopped.
- The test passes while the limitation holds and fails when a later WAMR does stop the loop, so it
  doubles as a tripwire for a version update.
- On the device, an endless AOT loop holds the loop task until the task watchdog resets the board.
  For blocks that anybody may write, that is disqualifying.

**Chicory.**

- Chicory has no fuel, but its interpreter checks the thread's interrupt flag.
- E-Home's simulation can therefore bound a call by time: the endless loop leaves the module 51 ms
  after the interrupt, through `ChicoryInterruptedException`.

## 4. Faults

Every fault traps, stops the block with its reason, refuses later calls, leaves the room
controller beside it running and leaks nothing (heap afterwards: 0 B against before). There is no
reset and no watchdog. Times are from the call to its return.

| Probe | Fast interpreter | AOT XIP | Classic |
|---|---|---|---|
| Endless loop | "instruction limit exceeded" (budget 1M: 363 ms) | not run (would hold the task) | same, 231 ms |
| Memory access out of bounds | "out of bounds memory access", 0.6 ms | same, 0.4 ms | 0.8 ms |
| Deep recursion | "wasm operand stack overflow", 0.3 ms | "native stack overflow", 0.2 ms | 0.5 ms |
| C stack (shadow stack) overflow | "wasm auxiliary stack overflow", 0.4 ms | same, 0.1 ms | 0.5 ms |
| `unreachable`, `__builtin_trap` | "unreachable", 0.2 ms | same, 0.1 ms | 0.2 ms |
| Integer division by zero | "integer divide by zero", 0.5 ms | same, 0.2 ms | 0.7 ms |

- **AOT recursion:** AOT's deep recursion is caught against the native stack boundary. That needs
  `wasm_runtime_set_native_stack_boundary` set with a guard: 1 KB was not enough and ended in an
  IDF stack-protection fault; the spike uses 3 KB.
- **Chicory** traps the same probes with exceptions:
  - `WasmRuntimeException` for out of bounds;
  - "call stack exhausted" for recursion;
  - out of bounds for the shadow stack, which lies first in memory;
  - `TrapException` for `unreachable`;
  - an exception for the division.

## 5. Host calls and allocations

Costs on the C6 at 160 MHz, with the native stack boundary set (see "What fails": without it,
every call costs about 200 µs more). The `bench_*` probes loop n times inside the module; the
figure is the time per iteration.

| µs | Fast interpreter | Fast, -O2 | Classic | AOT XIP |
|---|---|---|---|---|
| Loop iteration (baseline) | 1.66 | 1.48 | 2.31 | 0.25 |
| + `hc.now()` | 4.1 | 3.2 | 5.5 | 2.1 |
| + `hc.read(port)` | 5.4 | | 7.2 | 2.9 |
| + `hc.write(port, v)` | 5.7 | | 8.0 | 3.6 |
| + `hc.log(ptr, len)` | 5.4 | | 7.4 | 3.4 |
| + float arithmetic | 8.3 | | 11.6 | 6.4 |
| Call into the module from C (empty export) | 8.2 | 7.2 | 8.2 | 6.0 |
| **One PI step** (`hc_period`: 3 reads, 2 quality, 4 params, now, write) | **80.9** | 69.4 | **106.9** | 51.1 |
| PI step without the stack boundary | 286 | 231 | 288 | 245 |

- **Import calls:** an import costs about 2.5–4 µs beyond the loop (fast interpreter).
- **Per event:** about 8 µs to enter the module, plus about 70–100 µs for a block like the room
  controller.
- **Replay:** during the replay of the series, without the boundary, the longest call took 1.1 ms.
- **Host for comparison** (x86-64, fast interpreter): a PI step takes about 0.3 µs, an import
  25–30 ns, a module call 30 ns.

**Allocations: none in steady state.**

- 2000 PI steps make 0 mallocs, 0 reallocs and 0 frees by the runtime, and the heap delta is 0.
- This holds for every variant (fast, classic, AOT, with and without the boundary) and on the host
  (10 000 calls).
- Requirements on the host: one execution environment per module instance, created at
  instantiation and reused, and function handles looked up once.
- WAMR allocates at load and instantiation only.

## 6. Loading and replacing

| C6 | Fast interpreter | Fast, -O2 | Classic | AOT XIP (code already in flash) |
|---|---|---|---|---|
| room_controller, 934 B: load + validate | 5.6–7.0 ms | 4.9–6.3 ms | 2.4–3.8 ms | 1.9–2.5 ms |
| bulk, 20 756 B: load + validate | 82.6 ms | 61.1 ms | 21.2 ms | 0.8 ms |
| Instantiate | 0.6–1.1 ms | 0.6–0.9 ms | 0.5–1.0 ms | 0.3–0.7 ms |

- **From LittleFS** (the blob store):
  - The mount of a 192 KB LittleFS costs 1.7 KB of heap.
  - Reading the 20.7 KB module takes 9.6–12.1 ms, the 934 B one 3.0–5.7 ms.
  - Read, load, validate and instantiate work end to end for both modules.
- **Replace without reboot: yes.**
  - The swap test runs 100 swaps of room controller and bulk (each a load, instantiate, call and
    unload).
  - It leaves the heap where it was (delta 0 B) and the largest free block unchanged (417 792 B
    fast, 425 984 B classic), and the runtime's live allocation is 0 B.
  - The longest swap took 88 ms on the fast interpreter, 66 ms at -O2 and 26 ms on the classic one.
  - On the host, 200 swap cycles leak nothing.
- **The loader rewrites its input buffer** (see "What fails").
  - The fast interpreter copies what it keeps, so the buffer can be freed after the load
    (`wasm_binary_freeable`).
  - The classic interpreter executes from the buffer, so it must live until unload. The figures
    above include it.
- **The image references a module by hash (#60).**
  - The `.wasm` would come with the image and be stored in LittleFS.
  - AOT would additionally need its XIP region written. AOT's load is fast because its compilation
    happened in `wamrc`, which E-Home would have to run per target.

## 7. Toolchains

The same block, op for op, in three languages. Each gives a module that imports only `hc.*` and
needs no language runtime, no WASI and no libc:

| Language | Toolchain | Module |
|---|---|---|
| C | clang of WASI SDK 34 (its sysroot unused): `--target=wasm32 -mcpu=mvp -O2 -nostdlib -ffreestanding`, `wasm-ld --no-entry --stack-first -z stack-size=4096 --initial-memory=65536 --max-memory=65536 --export=__heap_base --export=__data_end` | 934 B |
| Rust | 1.99.1, `wasm32-unknown-unknown`, `#![no_std]`, `opt-level = "s"`, `panic = "abort"`, `target-cpu=mvp`, the same link arguments | 1033 B |
| Zig | 0.17.1, `wasm32-freestanding`, `-mcpu=mvp`, ReleaseSmall | 946 B |

- **The same bits:** all three give the hash `8e0b6013be72317b` over all 2211 rows, so the ABI
  depends on no language.
- **wamrc 2.4.5:** `--target=riscv32 --target-abi=ilp32 --cpu=generic-rv32
  --cpu-features=+m,+a,+c --xip` for the C6.
- **Reproducible:** `build-blocks.sh` builds everything into `wasm/`, and a rebuild gives the same
  bytes.
- **The author's pitfalls:**
  - Newer LLVM enables bulk memory, reference types, sign extension and multi-value by default;
    `-mcpu=mvp` keeps the module plain.
  - Without `--stack-first`, a stack overflow overwrites data.
  - Without one fixed page and the two exports, the instance costs 64 KB.

## 8. The Java side

- **Runtime:** Chicory 1.7.5, pure Java with no JNI, under JUnit 5 (`java/`, `mvn test`, 5 tests).
- **What it runs:** the same files from `wasm/` against the same series.
  - The C, Rust and Zig modules, and instance 7 of the C module, give **every row bit-identical**
    to `expected.csv`: valve, integral and filtered value, as raw IEEE 754 bits.
  - The faults trap.
  - The endless loop is interrupted (question 3).
- **The same hash everywhere** (`8e0b6013be72317b`):
  - device: C on the fast interpreter, Rust on the fast interpreter, C as AOT XIP, C and Rust on
    the classic interpreter, and the -O2 build;
  - host WAMR: C, Rust, Zig and x86-64 AOT;
  - Chicory: C, Rust and Zig.
- **Float semantics:** WebAssembly arithmetic is IEEE 754 with round-to-nearest. The block avoids
  NaN in its outputs and state (a missing value is the quality, not a NaN), so NaN payloads, the
  one non-determinism, never show.
- **GraalWasm was not tried.** Chicory already meets the requirement, without a native image or a
  Graal JDK.

## Chosen configuration (recommendation)

- **WAMR 2.4.5, classic interpreter**, with instruction metering, SHRUNK_MEMORY, no WASI and no
  libc. The fast interpreter is the alternative.
  - The classic interpreter costs less flash (66.6 against 75.4 KB) and less RAM for larger modules
    (38 against 52 KB for the 20 KB module, binary included).
  - It loads 4 times faster (21 against 83 ms).
  - It costs about 30 % more per call (107 against 81 µs per PI step).
  - On the 4 MB board RAM is the scarce resource, so classic wins.
- **No AOT:** it cannot be bounded, and it would need an executable flash region in the partition
  table.
- **Page strategy:** modules declare one fixed page and are shrunk to data plus stack. There is
  **one module instance per block type**, and the block instances are state records selected by
  `self`.
- **Metering:** a budget per call of instructions of the chosen interpreter, e.g. 20 000 (about
  5 ms). An exhausted budget is a fault like a trap.
- **Event loop:** it runs as a pthread (see "What fails"). The native stack boundary is set once,
  with a 3 KB guard, and one execution environment per module instance is reused.
- **Faults and restart:** a fault stops all instances of the type and their outputs go to their
  failsafe values. Restart instantiates the module again; the declared persistent state is
  restored from the persistence layer, as at boot.
- **E-Home:** Chicory, with a time bound per call through interruption.

## Budget per board

**RAM is the same on every C6 board** (512 KB SRAM, no PSRAM), so the RAM budget does not grow with
the flash size:

- with WiFi, KNX, Zigbee, the web server and the test-system image, the controller has about
  110 KB of free heap (93 KB minimum, `doc/hardware.md`);
- a WebSocket client takes about 30 KB of it.

A **block budget of about 30 KB of heap** leaves the margin. Examples on the classic interpreter:

- the room controller type with up to 8 instances costs 11.7 KB;
- two or three such small types fit;
- one 20 KB module type costs 38 KB and does not fit next to them.

A module's instance cost could still be cut by a smaller C stack in the module (the 4 KB of the
link option) and a smaller interpreter stack (the 2 KB). The PI step needs far less, but the spike
did not measure by how much.

| | 4 MB | 8 MB | 16 MB |
|---|---|---|---|
| ota_0 / free with the controller (2,551,408 B) | 2752 KB / about 260 KB | 4096 KB / about 1.6 MB | 6144 KB / about 3.6 MB |
| WAMR in the app (classic / fast) | 66.6 / 75.4 KB | same | same |
| Module storage: LittleFS `storage` (shared with values, history) | 204 KB | 2956 KB | 9100 KB |
| Heap for loaded blocks | about 30 KB | same | same |
| AOT XIP region (not recommended) | would have to come out of `storage` | from `storage` | from `storage` |

**The 4 MB board can run loaded blocks.** No limit of the C6 rules it out:

- flash: the runtime takes a quarter of the free ota_0 space, and modules of a few KB fit easily in
  `storage`;
- the binding limit is the heap, and it binds every board alike.

Making loaded blocks an 8 MB feature would therefore not buy anything. More room for blocks takes
more RAM, which means a different chip (the ESP32-P4 option of `doc/hardware.md`), or fewer other
features on the C6.

## What fails, and what it takes

- **Custom page sizes:** neither WAMR 2.4.5 nor Chicory 1.7.5 reads them. SHRUNK_MEMORY replaces
  them on WAMR, and Chicory does not need them.
- **AOT cannot be stopped:** it has no metering, and `wasm_runtime_terminate` is ignored unless the
  module has shared memory. A WAMR that checks in loops would be needed; the host tripwire test
  shows when.
- **AOT from RAM:** there is no executable heap under the PMP split. AOT needs an XIP flash region,
  which is a partition-table change; the spike borrowed the end of ota_0.
- **WAMR needs a pthread:**
  - On ESP-IDF, `os_self_thread()` calls `pthread_self()`, which asserts in a task created with
    `xTaskCreate`.
  - The controller's `hc_loop` is such a task (`platform/esp/main/main.cpp`), so it would be
    started through `pthread_create` with `esp_pthread_set_cfg`, which keeps the name, stack and
    priority.
  - The spike's loop is such a pthread, with the same 9216 B and priority 5.
- **Thread info on every call:**
  - Each call runs `wasm_exec_env_set_thread_info`. Without a native stack boundary, that calls
    `vTaskGetInfo` with the stack high-water mark, a stack scan of about 200 µs.
  - Setting `wasm_runtime_set_native_stack_boundary` once avoids it: the PI step drops from 286 to
    81 µs.
  - The guard must be about 3 KB, as with 1 KB AOT's recursion reached the IDF stack protection.
- **The loader rewrites its input:** a second load of the same buffer fails with "unexpected end
  of section". The host loads from a private copy (fast interpreter, freeable) or keeps the buffer
  (classic).
- **The classic interpreter needs the binary for the module's lifetime.** The classic run's
  "binary freed" rows are therefore not valid: the module still worked because the freed memory
  was not yet reused. That run's log lines also say "fast interpreter", because the label had not
  been made variant-aware yet when it was taken; the figures are the classic ones.
- **A trap leaves the module's C stack pointer moved:** a trap skips the epilogue, and after the
  shadow-stack overflow the stack pointer global moved from 4092 to 1020. The same instance must
  not be called again. Restart means a new instance, about 1 ms, as the recommendation says.
- **Writing the running ota_0** (the spike's scratch region only) needs
  `CONFIG_SPI_FLASH_DANGEROUS_WRITE_ALLOWED`. A real XIP region would be its own partition.
- **Not measured:**
  - the block calls inside the controller's own event loop with WiFi and KNX running;
  - persistence of the state records through the controller's value store.

  Both belong to the #60 implementation, and nothing in the spike's figures suggests a problem
  there.

## Running it

```sh
# Modules (committed; only needed after a change). Tools under ~/tools, see the script's header.
spike/blocks-wasm/build-blocks.sh

# Host test (WAMR fetched by URL and hash; -DFETCHCONTENT_SOURCE_DIR_WAMR=<dir> for a local copy)
cmake -S spike/blocks-wasm/host -B build-blocks-wasm -G Ninja
cmake --build build-blocks-wasm && ctest --test-dir build-blocks-wasm --output-on-failure
#   build-blocks-wasm/blocks_wasm_host_test spike/blocks-wasm --write-expected regenerates fixtures/expected.csv

# Java test
(cd spike/blocks-wasm/java && mvn -q test)

# Device: build, flash ota_0 only, read the console up to "SPIKE DONE", flash the controller back
cd spike/blocks-wasm/esp
idf.py -B build-both -D SPIKE_VARIANT=both set-target esp32c6 && idf.py -B build-both build
python -m esptool --chip esp32c6 --port <port> --after hard_reset write_flash 0x100000 build-both/spike_wasm.bin
```

The flashed app writes only inside ota_0. Afterwards, restore the controller with
`write_flash @app-flash_args` from its build directory.
