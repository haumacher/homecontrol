---
name: homecontrol-workflow
description: The working method for developing HomeControl — orchestrating sub-agents across the two builds (the POSIX host build with the unit tests and the ESP-IDF firmware build), adversarial probe reviews, the verification gate, the run loop against the C6 board and the KNX test system, GitHub-issue triage, the commit discipline, and the core/platform and format doctrines. Load at the start of any session doing feature or bug work on this repository.
---

# The HomeControl working method

This skill is the meta-knowledge of how a session on this repository should run. It complements,
never duplicates, `CLAUDE.md` (build facts, conventions, the boards), `doc/hardware.md` and
`ROADMAP.md` — the direction record (vision, phases, decisions log). Read all three first. The
GitHub issue tracker at `haumacher/homecontrol` **is** the work queue; each issue carries its
roadmap phase as a `phase:*` label (§1 and §8). The engineering counterpart is E-Home in
`../ehome` (its own repository, workflow skill `ehome-workflow`, issues at `haumacher/ehome`):
work on the contract between the two (`spec/`) usually has a sibling issue there.

## 1. The operating loop

Work is a pipeline, not a pile:

1. **Intake**: user reports (chat, monitor logs, screenshots of the web UI, KNX telegram dumps)
   and the open issues at `haumacher/homecontrol`. Bugs outrank queued features. Triage every new
   GitHub issue with a diagnosis comment *before* fixing; use the repository's labels (`bug`,
   `enhancement`, the `phase:*` labels, `cross-cutting`, `decision`, `hardware`; do not invent new
   ones without the user).
2. **Reproduce before dispatching.** A core report gets a headless reproduction first: a unit test
   in `core/test/` or a run of the POSIX controller with a fixture runtime image from
   `spec/fixtures/` (fixtures carry placeholder gateway addresses, never a real one). A firmware report gets a flash of the C6 and the monitor log (§7) — not a
   guess from the source. Know whether it is a real bug, an ESP-IDF limitation, or your own probe
   error before an agent burns tokens. If the main tree is agent-owned, reproduce in a git
   worktree at HEAD (`git worktree add <scratch>/repro <commit>`), never in the live tree.
3. **Dispatch** one sub-agent per coherent package (§2). Sequential by default; parallel only with
   disjoint file ownership stated in both briefs (§3). The natural split is core (host-tested)
   versus platform (ESP components), and `spec/` has one owner per round.
4. **Review with a novel probe** (§4). Non-negotiable.
5. **Gate** (§5), **commit to master and push** (§6), **flash and watch** (§7), close the issue
   with the fixing commit plus a summary comment naming the regression test.
6. **Loop back to step 1 — immediately, in the same turn.** A closed issue is the start of the
   next package, not the end of a pass: re-read `gh issue list -R haumacher/homecontrol` (and
   `haumacher/ehome` for the contract between the two) and take the next actionable issue without
   waiting for the user. Anything you defer becomes an issue, so a crashed session loses nothing.
   A direction-level decision taken on the way goes into `ROADMAP.md`'s decisions log in the same
   commit.

**When a pass ends.** Only when no issue is actionable — never earlier. Not reasons to end the
turn: a package was pushed or a run notice written (a progress line, not a hand-off); a number of
issues is done or the session ran long (the context is summarized automatically); the next issue is
large or needs the board; a background agent still runs; an issue needs the user's decision (ask
in the issue with options and a recommendation, take a sensible default where there is one, and
move on). An issue is not actionable only if it waits on an answer the user has to give, an
upstream fix, or a physical action only the user can do. The final message of a pass lists every
open issue that remains with that one reason.

Track packages with TaskCreate/TaskUpdate; keep statuses honest (in_progress on dispatch,
completed only after the commit is pushed).

## 2. Agent briefs — what makes them work

Use Opus for feature/bug packages (standing directive: Opus or cheaper; Sonnet only for genuinely
mechanical chores). Run in background; you'll be notified. A brief that produces a good delivery
contains, in order:

- **Grounding**: "Read CLAUDE.md, ROADMAP.md and doc/hardware.md. Study <the specific headers in
  `core/include/hc/`, the specific components under `platform/esp/components/`, the spec documents
  and the tests that are the behavioral contract>." Name where the work lives — `core/` (portable,
  host-tested), `platform/posix/`, `platform/esp/main/` or a component, `spec/` (the contract with
  E-Home), `web/` — and **whether `spec/` is in scope**, because a format change crosses into
  E-Home.
- **The task as design intent**, not implementation orders — with the load-bearing decisions
  made: what the core decides versus what a platform provides, what the runtime image carries
  versus what the device derives, what refuses versus what heals, what must survive a reboot and
  where it is stored (NVS, LittleFS, RAM). Quote the user's own words where they designed the
  feature; their design is usually better than your draft (the OTA layout in `ROADMAP.md` is an
  example).
- **User fixtures embedded verbatim** as required regression tests: a telegram dump, a runtime
  image fragment, a monitor log, a rule the user wrote — whatever the report contained.
- **Explicit acceptance tests**, including exact values where computable (DPT encodings byte for
  byte, sunrise times for a date and place, scheduler firings across a DST change, heap
  watermark bounds). "encode → decode is identity" and "load image → serialize → load again is
  equal" belong in almost every core brief.
- **The build discipline block** (the point is not wasting runs, not avoiding them):
  - Host: the edit loop runs `cmake --build build && ctest --test-dir build -R <TestName>`; the
    full `ctest` runs at the end. Configure once (`cmake -S platform/posix -B build -G Ninja`).
  - ESP-IDF: `source ~/tools/esp/esp-idf/export.sh` in **every** Bash call that uses `idf.py`
    (shell state does not persist). The edit loop runs `idf.py build` for the one target under
    work; the full matrix (`esp32c6` and whatever `CLAUDE.md` lists) runs at the end, each in its
    own build directory (`idf.py -B build-esp32c6 set-target esp32c6`, so switching targets does
    not wipe the other build). Flashing is **not** part of an agent's loop unless the brief says
    so and names the port; the user said: only the C6 (resolve its node by id, CLAUDE.md), never the S3.
  - A run whose inputs did not change is not repeated "to be sure". Batch independent lookups
    (several greps in one call) instead of one grep per turn.
- **The standing rules block**: ALL existing tests green; the core compiles with
  `-Wall -Wextra -Werror` and without exceptions or RTTI; **no ESP-IDF or POSIX include in
  `core/`** (the platform interfaces in `core/include/hc/platform/` are the only way out);
  **never write to the user's home KNX installation** — development talks only to the test system,
  whose address the orchestrator supplies in the brief and which never lands in a file; persisted formats stay readable (§8); do NOT commit; **nothing half-done
  — cut whole items only, refuse at runtime with a visible reason (log and web UI), and report
  every cut**.
- **Ask for a report**: mechanism chosen and why, alternatives rejected, cuts, test count
  before/after, firmware size and free heap before/after when the ESP build changed. The report is
  your review input; a vague report predicts a vague delivery.

**Reworks go back to the same agent** via SendMessage with the failing probe path and the demand:
*fix it generally, not to the probe*. If several rework rounds fail, question the architecture,
not the tests.

**Stalled agents are real.** If a background agent's transcript goes quiet: arm a Monitor on the
output file's mtime (10-minute quiet threshold, until-loop, not tail -f). **The tasks-dir output
path is a symlink — stat it with `-L`**, or the monitor reads the symlink's own never-changing
mtime and cries wolf in exact 600 s increments. Verify before acting on any event: a quiet
transcript with a busy `ninja` or `idf.py` process is a long build (a first ESP-IDF build of a
target takes minutes), not a stall. On silence: nudge-resume via SendMessage ("Resume exactly where
you left off: <last visible step>"). If the transcript stays frozen after a resume, the agent is
dead — take the remainder over yourself; its uncommitted work is usually further along than the
last message suggests (`git status`, then run its tests before redoing anything).

**Crash recovery.** The crashed session's transcript is
`~/.claude/projects/<project-dir>/<session-id>.jsonl`; its sub-agents' transcripts are
`<session-id>/subagents/agent-*.jsonl` beside it; `~/.claude/tasks/<session-id>/` holds the task
list. Pull the last `Agent` tool_use out of the session transcript with a short python filter —
that is the brief, verbatim — and the last tool calls out of the newest sub-agent transcript to see
where it died. The agent's uncommitted work is in the tree: `git status`, `cmake --build build`,
then **re-dispatch the same brief with a resume preamble** stating exactly what exists, what
compiles, and what is not started; never redo the work. Check `gh issue list` immediately —
reports filed during the crash are untriaged.

## 3. Concurrency — the rules

- **Never let two agents edit the same file.** When parallelizing, write an explicit FILE BOUNDARY
  paragraph into *both* briefs ("do not touch X, Y, Z; if you must, STOP and report").
- **`spec/` has one owner per round.** The runtime image format and the rule language are shared
  with E-Home's Java side; whoever changes them owns the fixtures, the C++ fallout, and the
  sibling issue at `haumacher/ehome`.
- **Platform interfaces (`core/include/hc/platform/`) have one owner per round.** A core agent
  that needs a new platform capability adds the interface and a POSIX implementation; the ESP
  implementation is either in the same package or an explicit follow-up issue — never an
  unbuildable firmware.
- **Build directories are per tree and per target.** Two agents in one tree may share
  `build/` only if neither runs the host build; give a second agent its own git worktree (§10)
  and its own build directories. ESP-IDF builds in one `build-<target>/` are never concurrent.
- **The board is a single resource.** Only one agent (or the orchestrator) flashes or monitors
  the C6's port at a time; an open `idf.py monitor` blocks the next flash.
- A commit made while an agent has uncommitted work in the tree must be **selective**
  (`git add <files>`).

## 4. The probe review (the quality mechanism)

After a delivery reports done and before committing: **write a test the agent never saw**,
composing the new feature with pre-existing features — a DPT with a value at the edge of its
range, a schedule across the DST change, a rule whose trigger and action use the same datapoint, a
runtime image with an unknown field or a missing section, a reconnect after the KNX gateway drops
the tunnel, a reboot in the middle of a swap. Good probes ask "is the mechanism general?" not
"does the happy path work?". Keep passing probes as permanent tests (`<feature>_probe_test.cpp`
in `core/test/`).

**Expect your own probes to be wrong first.** The recurring traps — check these before blaming the
delivery (this list grows; add every trap you fall into):

- **The Bash tool resets its working directory between calls.** A chain that relies on an earlier
  `cd` runs in the repo root; use absolute paths or one `cd` per command, and never `git add -A`
  after a relative `cd ..`.
- **ESP-IDF is not on the PATH.** `idf.py: command not found` means the call did not
  `source ~/tools/esp/esp-idf/export.sh`; it is not a broken install.
- **`idf.py set-target` wipes the build directory and `sdkconfig`.** Use one `-B build-<target>`
  per target and keep settings in `sdkconfig.defaults` (plus `sdkconfig.defaults.<target>`), never
  only in the generated `sdkconfig`.
- **`esptool` "Write timeout" on the C6** means the firmware on it is crash-looping and the USB
  Serial/JTAG keeps resetting, not a cabling fault: `--connect-attempts 15`, or BOOT held during
  RESET. Port numbers follow plug-in order; `ls -l /dev/serial/by-id/` tells the C6
  (MAC E8:F6:0A:F8:83:EC) from the S3.
- **`pkill -f <pattern>` matches your own shell** when the pattern appears in the command line you
  are running, and kills the rest of the chain (exit 144). Stop a monitor or the POSIX controller
  by PID (`kill $(pgrep -x homecontrol)`), in its own call.
- **The POSIX controller and the firmware share the core but not the clock.** A time test that
  passes on the host with the system time zone set may fail on the device, which has only the
  `TZ` string the runtime image gives it. Tests pin the time zone explicitly.
- **A green host build says nothing about flash or heap.** Any package that adds a component or a
  library reports the binary size (`idf.py size`) and the free heap after boot from the monitor;
  the C6 has about 512 KB of RAM in total.
- **The Playwright MCP writes into the repository root** (`.playwright-mcp/`, `recording.json`),
  not the scratchpad; both are in `.gitignore` — never `git add -A` while a browser session is
  open in another agent.
- **A failed CMake configure leaves the previous test binary in place, and `ctest` then passes
  against it.** After resolving a merge in `core/hc_core.cmake` or `platform/posix/CMakeLists.txt`
  (both sides append to the same `list(APPEND …)`/`target_sources(…)` block, and a keep-both
  resolution can swallow a closing parenthesis), read the build output for `FAILED: build.ninja`
  before believing a green `ctest`, and compare the test count with what the branches should add.
- **A merge is not gated by the commits it merges.** Run the full gate on the merged tree before
  pushing, even when every branch was green on its own.
- **A scripted multi-file edit that stops at a failed anchor has already written the files before
  it.** Edits applied in order are on disk, the later ones are not, and the commit message you
  drafted from the plan is then wrong (the MQTT device wiring was "done" in a commit that did not
  contain it). After such a script, grep every target for its new symbol before building, and
  prefer one script per file or a script that checks all anchors first.
- **A background process started inside a tool call must have its stdout and stderr redirected.**
  A broker or server launched with `&` that keeps the shell's pipe open makes the call wait for
  ever; two agents stalled on exactly this. The same for `rm -rf ./*` after a `cd`: the safety check
  refuses it and the call never returns in an agent. Use a fresh directory per run instead.
- **Parallel Bash calls share one shell working directory.** A `cd` inside one call (`cd
  platform/esp && idf.py …`) changes the directory under a call running at the same time, whose
  relative paths (`spec/api.md`) then miss. Every call that may run beside another uses absolute
  paths, including the first `cd` of a chain.
- **A build that stalls the event loop locks you out of the board.** The task watchdog resets a
  stalled loop every few seconds, esptool then fails with "Write timeout" on every reset mode, and
  only BOOT held during RESET (the user's hands) gets the chip into download mode. A stall inside a
  flash operation (NVS, LittleFS) also leaves no core dump, so there is no backtrace afterwards.
  Before uploading a build with new storage, locking or task hand-over code: build the previous
  master in a worktree first so the rollback image is at hand, watch the serial log for the first
  minutes with the browser open, and treat two watchdog resets as a revert, not a debugging session.
- **Verify a wiring claim on the hardware by its log line, not by the build growing.** The core
  links in whether or not a platform service uses it; only the service's own log line (or its
  console command) proves it runs.
- **Diagnose a stall from the API, not from the serial port.** `GET /api/system/diagnostics` gives
  `loop.missed` and `loop.longest_turn_ms` (sample deltas over 30 s: the idle rate is a miss every
  few seconds), `console.queued/dropped`, and per task `stack_free` (the high-water mark) and
  `cpu_permille` (cumulative since boot: a short burst barely moves it, read it as a delta against
  the uptime). A loop that misses periods with short turns and an idle CPU is not running: an
  interrupt storm or a blocked wait, not work. Attach a serial reader only as the experiment's
  variable, and resolve the C6's node by id each time (CLAUDE.md).
- **A gate whose output goes through `tail` or `grep` has the exit code of the filter, not of
  `ctest`.** Read the `N% tests passed` line, never the shell's exit code of a pipeline. And
  `BuildId.FollowsGitDescribe` compares the test binary's build id with `git describe`: a binary
  built from a dirty tree fails it after the commit (the id says `-dirty`, the tree no longer is);
  rebuild and rerun that one test instead of hunting a regression.
- **A watchdog panic on the C6 can leave no core dump and a cut register dump.** The panic
  handler prints with a 1 s watchdog budget before it writes the core dump; a slow USB Serial/JTAG
  reader exhausts it (`Saved PC` in `panic_print_char_usb_serial_jtag`, reset `TG0_WDT_HPSYS`).
  Symbolize what did print by rebuilding the *same commit* in a worktree: the IRAM addresses are
  stable across rebuilds of one commit and sdkconfig (`riscv32-esp-elf-addr2line -e homecontrol.elf
  -f -i -p <MEPC> <RA>`). `MEPC` in `vPortYield` (port.c, the spin waiting for the yield interrupt)
  with IDLE starved means a task that runs without ever switching, not a lock wait; the loop passes
  that spin at every ordinary wait, so it names the mechanism, not the caller. A silent serial port
  plus esptool "Write timeout" is such a firmware still running with its console reader starved;
  only BOOT held during RESET gets the chip back.

When a probe legitimately fails: send it back (§2). When it exposes something deeper than the
package (a format gap, a platform-interface hole), fix small ones yourself with a regression test;
spawn a package for large ones. Two classics to stay paranoid about: silent refusals (nothing may
decline without a log line and, where there is a UI, a visible message), and anything that sends
to the KNX bus (only the test system, only when a rule or the user asked for it).

## 5. The verification gate (before every commit)

```bash
cmake -S platform/posix -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure
source ~/tools/esp/esp-idf/export.sh && cd platform/esp && idf.py -B build-esp32c6 set-target esp32c6 && idf.py -B build-esp32c6 build
```

All green or no commit: host build with zero warnings, every test passing, the firmware building
for every target `CLAUDE.md` lists (each in its own build directory). Both builds must run even
when only one side changed, because the core couples them. Never trust a green result from a tree
with uncommitted foreign edits; check `git status` first.

## 6. Committing

- **Commit to `master` and push to `origin/master` every time** (standing directive; CLAUDE.md
  records this convention). Feature branches and PRs are for external contributors. `Closes #N`
  in the body closes the issue.
- House style: an imperative one-line title stating the change, then a paragraph of the why
  (never a bullet changelog), ending with the Co-Authored-By line from the harness rules.
- The agents don't commit; you do, after review. A `spec/` change rides the same commit as the
  C++ code and fixtures that implement it.

## 7. The run loop

Two things to run:

```bash
./build/homecontrol --image spec/fixtures/<image>.json        # the controller on Linux, against the KNX test system
source ~/tools/esp/esp-idf/export.sh && cd platform/esp && idf.py -B build-esp32c6 -p $(readlink -f /dev/serial/by-id/*E8:F6:0A:F8:83:EC*) flash monitor
```

The monitor is left with Ctrl-]. Read the boot log for the heap watermark, the IP address and the
mDNS name, then open the device's web UI in the Playwright browser when the package touched it.

**Application builds go to the board through the browser, not USB** (since #39, 2026-10-18): on the
device's Settings page, opened by IP address, System → "Install firmware file" → pick
`platform/esp/build-esp32c6-devtls/homecontrol.bin` (Playwright: click the button, `browser_file_upload`,
accept the confirm dialog), then wait for the status line "<version> (<build>) läuft"; the serial
capture shows `hc.updater` writing the image and `hc.ota ... completed (uploaded from the browser)`.
About 40 s all in, the stored image and the Zigbee database stay in place, and the page shows the
build id to compare with `hc_generated/hc_build_id.h`. USB (`esptool.py ... write_flash`) stays for
the updater (`0x10000 hc_updater.bin`), the bootloader, the partition table, and the empty file
system `storage.bin` (which wipes the stored runtime image and persisted values: redeploy
afterwards). The web UI is compiled into the application and comes with every OTA update.
**The desk board runs the development build** `build-esp32c6-devtls` (configured with
`-DHC_EXTRA_CA=$HOME/.config/ehome/dev-tls/ca.pem`, `doc/ehome.md` "TLS during development"; build
it with `source ~/tools/esp/esp-idf-v5.5/export.sh`, the IDF it was configured with): it trusts the
local CA of E-Home's TLS proxy. A build without it (`build-esp32c6`) runs, but its E-Home link fails
with `ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED` and E-Home cannot deploy (2026-11-08).
Only the C6 is flashed, at the node its id resolves to (it moves between ttyACM0 and ttyACM1); the S3 is another project's board.
Tell the user the commit hash and what to try. Never leave a broken build on the board — it is the
shared reference for bug reports.

## 8. Doctrines (enforce in every brief and review)

- **The core is portable.** No ESP-IDF, FreeRTOS or POSIX include under `core/`; no exceptions,
  no RTTI; platform capabilities are interfaces with a POSIX and an ESP implementation. A core
  feature that works only on the device is not done.
- **Everything is a datapoint.** New technology adapters (KNX, Zigbee, weather, MQTT) translate
  into the datapoint model; rules and the UI never see a technology.
- **The runtime image is E-Home's output and a persisted format.** It is versioned; the device
  validates before it swaps; an old image must load after a firmware update. `spec/` documents
  every field and `spec/fixtures/` holds examples both sides test against.
- **One rule semantics.** The C++ interpreter and E-Home's Java interpreter pass the same
  conformance corpus in `spec/`; a semantic change lands with new corpus cases.
- **The device executes, E-Home engineers.** The device UI changes only what a rule declared
  adjustable; everything else is a refusal with a reason.
- **The bus is real.** Development sends only to the user's KNX test system, which they released
  for free use; its address is in the orchestrator's private memory, never in the repository, a
  fixture or an issue. The user's home installation is touched only with their explicit permission
  given in the same conversation; it is never configured into a build, fixture, test or running
  controller, and no gateway is ever chosen from discovery automatically.
- **Memory is the budget.** Steady-state paths allocate nothing; images allocate on load; every
  package reports size and heap.
- **Refusals speak**: no code path may decline silently; the log and the UI show the reason.
- **Everything generic**: issues are exercises of general mechanisms — if a fix is shaped like the
  bug report, it isn't done.
- **Decisions live in ROADMAP.md, issues and commit messages.** Direction-level decisions go into
  the roadmap's decisions log; the commit message is the as-built note and the issue comment is
  the rationale; deliberate cuts become issues, never silence.

## 9. Working with this user

They are an expert (Java, software architecture, KNX) and the project's author, testing
continuously on real hardware and a real bus. Their bug reports come as monitor logs, telegram
dumps or screenshots — embed them verbatim as regressions. Their feature sketches are usually
complete designs (the OTA layout is one) — restate the design back crisply, adopt it, credit it in
the commit message. When they overrule a recorded decision, the reversal is documented in
`ROADMAP.md` and the issue, not litigated. Answer their questions with the mechanism, then the
plan, then queue position. Run notices always include the commit hash and what to try.

## 10. Two agents at once — the worktree pattern

Parallel packages work when, and only when, they are physically separated:

- **One agent in the main tree, the other in a git worktree**
  (`git worktree add -b <branch> <scratch>/wt-x HEAD`, run **from this repo**), each with a FILE
  BOUNDARY paragraph and its own build directories.
- **`spec/` and the platform interfaces have one writer** (§3). If both packages need them,
  serialize.
- **Merging**: commit the worktree branch yourself (house-style message), merge it into master
  after the main-tree package is committed, run the full gate (§5) on the merged tree — build
  files and shared headers are exactly where merges break — then push.
- After a merge, `git ls-files` the new directories the report names; a `.gitignore` rule
  (`build*/`) can swallow a wrongly named source directory.
- **A session limit kills every running agent at once** (HTTP 429 mid-flight). Their uncommitted
  work is in the trees; when the limit resets, `SendMessage` each one "Resume exactly where you
  left off: <its last visible step, read from the transcript's last tool calls>" rather than
  re-dispatching. Make the stall monitor grep the transcript tail for "session limit" so the two
  cases are told apart.
