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
6. **Before ever finishing: `gh issue list -R haumacher/homecontrol`.** Stop only when the issue
   list holds nothing actionable. Anything you defer becomes an issue, so a crashed session loses
   nothing. A direction-level decision taken on the way goes into `ROADMAP.md`'s decisions log in
   the same commit.

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
    so and names the port; the user said: only the C6 at `/dev/ttyACM1`, never `/dev/ttyACM0`.
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
  `/dev/ttyACM1` at a time; an open `idf.py monitor` blocks the next flash.
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
source ~/tools/esp/esp-idf/export.sh && cd platform/esp && idf.py -B build-esp32c6 -p /dev/ttyACM1 flash monitor
```

The monitor is left with Ctrl-]. Read the boot log for the heap watermark, the IP address and the
mDNS name, then open the device's web UI in the Playwright browser when the package touched it.
Only the C6 at `/dev/ttyACM1` is flashed; the S3 at `/dev/ttyACM0` is another project's board.
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
