# Firmware performance optimization plan

## Why this document exists

We have reached the practical limit of what the *emulator* can give us. Under
heavy synthesis load the emulated Cortex-A9 cannot always render audio at real
time, which causes occasional breakup during dense playback. This is a **TCG
throughput limit, not a buffering one** (see
[register-coverage.md](register-coverage.md) and `DEBUGGING.md`), and we have
already proven it cannot be closed from the host side:

- Rebuilding QEMU with `--extra-cflags='-O3 -march=native'` gave **zero**
  improvement. In TCG the audio hot path is JIT-generated guest code, not
  statically-compiled QEMU C, so host compiler flags do not speed up guest
  execution.
- The guest is a single-core Cortex-A9, so MTTCG cannot help (no guest SMP).

Measured guest audio production pins at **~280–310 kB/s** during clip playback
against a real-time requirement of **44100 × 8 = 352,800 B/s** (~80–85% of real
time). The FIFO sits near-empty with ~50–80 underruns/sec. No amount of
buffering fixes a sustained throughput *deficit*.

So the remaining lever is the **firmware** itself: make the audio DSP path do
less work. In TCG, execution cost is approximately proportional to *guest
instructions retired + helper calls*, so **reducing guest instructions in the
render path translates almost 1:1 into emulator speedup**.

## Correction (2026-06-09): profile the Release build, and it is the scheduler

The original first-pass profile below was taken against the **Debug** ELF
(`-Og`, no `-flto`, inlining suppressed). That is *not* what real hardware runs.
`firmware/deluge.elf` is byte-identical to `build/Debug/deluge.elf`; real Deluge
hardware ships the **Release** build (`-O2 -flto=auto`). Re-profiling the
Release build changes the conclusion materially:

- At `-O2`, the scheduler helpers that dominated the Debug profile
  (`Time::operator<=>`, `Task::isReady`, `Task::isRunnable`) are **fully
  inlined** — they vanish as separate symbols. Verified by disassembling
  `chooseBestTask` in both builds: Debug emits out-of-line `bl` calls to those
  helpers plus VFP ops to zero 8-byte `Time` temporaries; Release has **zero**
  out-of-line helper calls and **zero** VFP/soft-float in the hot path.
- So "hand-inline the scheduler helpers" (the old Phase-4 lever) is **already
  done by the compiler** on real hardware and would be rejected upstream.
- The **complete** Release profile (FAM1, no `--icount`, `libhotblocks,limit=0`,
  12,974 blocks, 48 B instructions) is overwhelmingly the scheduler:

  | Symbol | % of guest instructions |
  |--------|------------------------|
  | `TaskManager::chooseBestTask` | **74.9%** |
  | `checkConditionalTasks` | 7.5% |
  | `getSecondsFromStart` | 7.1% |
  | `runHighestPriTask` + `runTask` | ~1.6% |
  | **scheduler subtotal** | **~91%** |
  | all audio DSP combined (Freeverb, Sound::render, compressor, filters, oscillators, voices) | **~7%** |

- This holds at both `--icount 2` and full host speed, and on both Debug and
  Release — it is **not** an idle-throttle artifact. The cooperative scheduler
  busy-spins: when no task is "ready", the dispatch loop re-runs `chooseBestTask`
  (three O(N) passes over all tasks) + `checkConditionalTasks` (O(25)) +
  `runHighestPriTask` and loops immediately.
- **Consequence for this campaign:** the DSP levers (compressor LUT, per-voice
  block processing) target a ~7% slice and cannot move the needle much. The only
  material, portable lever is **`chooseBestTask` itself** — make an idle decision
  cheap (early-out / incremental next-deadline) without changing which task is
  selected. That is sample-neutral by construction and frees real-hardware
  cycles (more polyphony / lower power).

### Attempted `chooseBestTask` micro-optimizations (2026-06-09) — both failed

Two sample-neutral attempts were made and reverted; the working tree and the
Release binary are pristine again (a before-copy is kept locally at
`build/perf-baseline/deluge-release-before.elf`, gitignored).

The throughput gate is **wall-clock-bound**, so the raw `chooseBestTask`
percentage and total instruction count are *not* valid A/B signals — a cheaper
call simply lets the idle loop spin more times in the same wall-clock window.
The only confound-free metric is **instructions per `chooseBestTask` call**
(`chooseBestTask` total ÷ its entry-block execution count). Baseline ≈ **1300
instr/call**.

1. **Collapse `isReady()` → `isReleased()` after `isRunnable()` + CSE the
   finish-time.** The reasoning is correct (`isRunnable()` already proves
   `state == READY && resourcesAvailable()`, leaving only `isReleased()`), but in
   the idle regime the original `isReady()` calls are **short-circuited to zero
   evaluations** by the `maxInterval`/`idealCallTime` guards that precede them.
   Computing `isReleased()` unconditionally therefore *adds* a 64-bit compare:
   **≈1407 instr/call (+8%)**. Reverted.
2. **O(1) idle early-out via a cached `min(earliestCallTime)`** (invalidated in
   `createSortedList`). Every return path requires
   `isReady → isReleased → currentTime > earliestCallTime`, so if
   `currentTime ≤ min(earliestCallTime)` the scan must return −1. The guard
   **never fired**: the busy-spin is not release-time-bound. The audio task
   ([deluge.cpp](../firmware/DelugeFirmware/src/deluge/deluge.cpp#L524)) is
   priority 0, `RESOURCE_NONE`, with `backOff ≈ 181 µs / target ≈ 1451 µs /
   max ≈ 2902 µs`; overdue tasks keep `min(earliestCallTime) ≤ now`, and
   readiness is gated by `idealCallTime`/`latestCallTime`/state/resources, not by
   `earliestCallTime`. The only correct "is anything ready" test is the O(N) scan
   itself.

**Conclusion:** `chooseBestTask` cannot be made cheaper by a simple cache or
early-out. A real win needs an **event-driven next-wake** redesign (compute the
next time any task transitions to ready and skip dispatch until then) on the
maintainers' core scheduler — a change with genuine timing-behavior risk, so it
is deferred pending explicit direction rather than attempted speculatively.

The sections below are retained for history; read them through this correction.

## Hard constraint: portable wins only

**We will only pursue changes that also help on real Cortex-A9
hardware.** The DelugeFirmware maintainers will not accept emulator-only
changes. Concretely, this rules out:

- softfloat→host-SSE shims or any QEMU-side FP fast path,
- "scalar beats NEON under TCG" rewrites (NEON is emulated per-element via
  helpers, so it can be slower under TCG — but it is faster on real HW),
- any TCG-block / translation-boundary micro-optimization.

The profiling tools below are used **only to rank guest-instruction-count
hotspots**, which is a portable ranking. We do **not** trust TCG for cache or
pipeline timing — it does not model them. A function that is hot in the TCG
profile is a *candidate*; before optimizing it we confirm the win is defensible
on real hardware (instruction-count reductions almost always are; cache/branch
tricks need separate reasoning).

## The metric

**Guest instructions retired per audio buffer** in the render path. Proxy
success signal: measured production crosses **352,800 B/s sustained** (today
~285k). That implies roughly a **≥20–25% instruction reduction** in the audio
path.

## Tools

All plugins are prebuilt in this tree. On Windows/MINGW they are `.dll`
(`qemu/build/contrib/plugins/`); the `.dylib` names in `DEBUGGING.md` are the
macOS equivalents.

| Tool | Where | Role in this effort |
|------|-------|---------------------|
| **`-icount` (`run.sh --icount`)** | built in | Deterministic, instruction-exact, timing-independent runs so a fully-instrumented profile does not perturb results and is byte-for-byte reproducible. Also the lever that **forces CPU saturation** (see the idle-dilution note below). The benchmark backbone. |
| **`libhotblocks.dll`** | `qemu/build/contrib/plugins/` | **The primary hotspot tool.** Per-basic-block execution counts (`pc, tcount, icount, ecount`). Run with `limit=0` for the full table; weight each block by `icount × ecount` and bucket by `nm` symbol to get a gprof-style **ranked-by-%-of-guest-instructions** flat profile. (See `scripts/dz_bucket.py`.) |
| **`libhowvec.dll`** | `qemu/build/contrib/plugins/` | Instruction-class mix (FP vs NEON vs scalar integer). **CAVEAT: its classifier is AArch64 (A64) only.** The Deluge is ARM32 Thumb-2, so every instruction shows "Unclassified" and it falls back to per-opcode counts — decode the top opcodes by hand to settle the FP-vs-fixed question. |
| **`libips.dll`** | `qemu/build/contrib/plugins/` | Instructions-per-second / total instruction throughput for the baseline and for measuring each change. |
| **`libuftrace.dll`** (+ `uftrace_symbols.py`) | `qemu/build/contrib/plugins/` | Call-graph / flamegraph for **inclusive** cost, so we find the heavy subtree, not just leaf functions. |
| **`liblockstep.dll`** | `qemu/build/contrib/plugins/` | Run two builds in lockstep to find the first execution divergence. **Useful for emulator-change determinism, NOT a firmware-change audio gate** — two different firmware builds diverge immediately by construction. See Phase 5 “Validation reality” for the gates that actually work. |
| **`libhwprofile.dll` / `libhotpages.dll`** | `qemu/build/contrib/plugins/` | **NOTE: `hwprofile` is a *device-IO* access profiler, NOT a guest-PC profiler** — do not use it for instruction hotspots (use `hotblocks`). `hotpages` = most-accessed memory pages (cross-check). |
| **`libcache.dll`** | `qemu/build/contrib/plugins/` | *Advisory only* — TCG does not model real A9 timing, but the cache simulator hints at memory-bound spots worth reasoning about for HW. |
| **`DELUGEMU_SSIF_STATS` probe** | SSIF device (built in) | The real-world success signal. Set `DELUGEMU_SSIF_STATS=1` in the environment and the SSIF device prints, once per second of virtual time, the freshly-rendered guest audio production rate (B/s and % of 352,800), primed-FIFO underruns/s, and staging-FIFO occupancy (ms). Production is counted where finished frames enter the staging FIFO, so with `--tx-render-head <addr>` it reflects what the firmware *actually rendered* (not the wall-clock play head). **Caveat:** this is a *wall-clock* delivery gate — on a host fast enough to keep the guest real-time it pins at ~100% even under heavy `--icount`, because the firmware really is meeting real-time. It drops below 100% (with underruns) only when the host cannot render in real time. Use it to confirm a change does not *regress* delivery and to gate on slower hosts / heavier songs; use `libhotblocks`/`libips` (instruction budget) as the primary day-to-day lever on a fast Mac. |
| **`arm-none-eabi-nm` / `-objdump` / `-addr2line`** | toolchain | Map plugin output addresses back to firmware symbols/source. `firmware/deluge.elf` carries full `debug_info` (not stripped); source is in `firmware/DelugeFirmware/src`. |
| **GDB (`-S -s`, `target remote :1234`)** | built in | Watchpoints on specific addresses; live inspection of DSP state and the memory map. |
| **`scripts/press_key.py` + QMP** | in-tree | Scripted, repeatable note input so the benchmark exercises the same load every run. |
| **`-audiodev wav`** | built in | **Does NOT work for this stream:** QEMU's wav backend rejects 32-bit formats (the SSIF voice is S32) and captures *after* the host-timing-dependent output resampler. Use `DELUGEMU_SSIF_DUMP` (raw S32LE, pre-resampler) for a perceptual spot-check instead; see Phase 5. |
| **DelugeFirmware source + `arm-none-eabi` toolchain** | *external (needed)* | Required to *act* on findings. Profiling needs only the existing ELF; rebuilding the firmware needs the source tree and cross-toolchain. |

## Phase 1 — Lock down a reproducible benchmark

- Run under **`--icount`** so the profile is deterministic and instrumentation
  does not perturb timing.
- Fix the inputs: one canonical **worst-case project** on a fixed SD image plus
  a scripted note sequence (`press_key.py` over QMP) for a fixed virtual
  duration.
- Capture the baseline: total guest instructions (`libips`), buffers rendered,
  B/s production and underruns (`DELUGEMU_SSIF_STATS=1`), and a raw render
  capture (`DELUGEMU_SSIF_DUMP=<path>`) for later spectral/by-ear spot-checks
  (see Phase 5 "Validation reality" — this is *not* a byte-exact reference).

### Idle-dilution caveat (important)

On a fast dev host the emulated guest **keeps up** even with a dense song, so it
spends most cycles **idle-spinning in the cooperative task scheduler** waiting
for the next audio buffer. A raw profile is then ~70% scheduler and the DSP is
buried. To get a representative render-path profile you must **saturate the
CPU** so there is no idle: run under `--icount 2` (and cross-check `--icount 4`).
Note that at deep saturation (`--icount 4`+) the firmware's **adaptive
voice-culling** activates — synth voices drop out while always-on master-bus
work (compressor, reverb send) remains — so read both a moderate- and a
deep-saturation profile.

### Reproducible load without encoder navigation

Headless QMP key events (`input-send-event`, qcodes) are enough: boot ~6 s, then
`l` (LOAD, PIC idx 159) → `ret` (SELECT-encoder click, idx 175) loads the
highlighted song — the load UI opens on the first file alphabetically, so with
`FAM1.XML` first that loads the dense 127-instrument worst case — then `spc`
(PLAY, idx 179). `sdcard/SONGS/` worst-case ranking: FAM1 (127 sounds) > FAM3
(129) > FAM2 (10) > SONG010 (2). Build the card with
`./scripts/mksd.sh sdcard build/deluge_sd.img`.

## Phase 2 — Find where the instructions go

1. **`libhotblocks.dll,limit=0`** → per-block counts. Weight by `icount ×
   ecount` and bucket by `arm-none-eabi-nm` symbol ranges on `firmware/
   deluge.elf` to produce a gprof-style **ranked-by-%-of-guest-instructions**
   flat profile.
2. **`libhowvec.dll`** → instruction-class mix (A64-only; decode top Thumb
   opcodes by hand). Resolves the strategic fork:
   - **FP-heavy** → every VFP/NEON op is a softfloat helper; look hard at the
     FPSCR flush-to-zero / default-NaN state (denormals are catastrophic on
     softfloat *and* real VFP/NEON).
   - **Fixed-point/integer-heavy** (the Deluge is historically Q31 — do not
     assume float) → the cost is raw instruction volume; wins are fully
     portable and upstreamable.
3. **`libuftrace.dll`** → inclusive call-graph to find the heavy subtree.
4. **`libcache.dll`** (advisory) → memory-bound hints for HW reasoning only.

## Phase 3 — Interpret (decision tree)

- Take the top 5–10 functions from the flat profile and open the corresponding
  DelugeFirmware source.
- Branch on `howvec`: float-bound → FPSCR/denormal + softfloat-shaped hotspots;
  integer-bound → instruction volume; NEON-bound → leave SIMD alone (it is a HW
  win even though TCG penalizes it).

## First-pass results (Debug ELF — superseded; see Correction above)

> These numbers came from the **Debug** build (`-Og`, no LTO). On the Release
> build that real hardware runs, `Time::operator<=>` / `isReady` / `isRunnable`
> are inlined into `chooseBestTask` and disappear as separate symbols. The
> Release-verified split is in the **Correction** section: ~91% scheduler
> (75% `chooseBestTask`), ~7% all DSP. Treat the items below as the Debug-view
> history only.

Measured against the dense `FAM1.XML` (127 synth instruments) under forced
saturation. The render path is **overwhelmingly fixed-point** (top opcodes are
all integer `cmp`/`ldr`/`b`/`bx`; float is a minor share). Three cost centres,
ranked:

1. **Cooperative task scheduler — the #1 cost (fully portable).** In the Debug
   profile this split as `Time::operator<=>` (19–27%) + `chooseBestTask`
   (15–21%) + `Task::isReady`; at `-O2` it is all one inlined
   `chooseBestTask` (~75%). Root:
   `src/OSLikeStuff/task_scheduler/task_scheduler.cpp`. The dispatcher
   (`start`/`yield`) calls `chooseBestTask` **before every task run**; it is
   `O(numActiveTasks)`, rescans the whole list each time, and does several
   64-bit `Time` comparisons per task. When nothing is ready it then runs two
   more O(N) fallback passes and `checkConditionalTasks`, then loops — a tight
   busy-spin. `Time` is an `int64_t` tick count (`clock_type.h`). The Debug
   `double` soft-float helpers (`__fixdfdi` etc.) noted earlier are **not**
   present at `-O2` (the `double` constants fold at compile time). Candidate
   lever (the one that survives the Release correction): make an idle decision
   cheap — early-out to the always-ready highest-priority task and/or maintain
   the next-deadline incrementally instead of three O(N) rescans — **without
   changing which task is selected** (sample-neutral by construction).
2. **`RMSFeedbackCompressor` (master bus) — always-on but only ~0.5% at `-O2`.**
   `render` + `calcRMS`, driven by `expf` / `logf`. Still a clean LUT candidate
   gated by an offline kernel null-test (Phase 5), but the ceiling is tiny.
3. **Per-voice DSP — ~7% combined at `-O2`.** `Freeverb::process` (~1%),
   `Sound::render` (~0.8%), `Lp`/`HpLadderFilter` (~0.6%), oscillator/wavetable
   kernels, voices. All fixed-point `long` (Q31). Block-processing wins raise
   the polyphony ceiling but cannot move the overall instruction budget much
   while the scheduler is 91%.

Strategic read (corrected): the scheduler is not just the largest win, it is
**almost the entire budget** on real hardware. Tackle `chooseBestTask` first;
the DSP levers are secondary and bounded by their ~7% share.

## Phase 4 — Optimization levers (cheapest × safest first)

1. **FPSCR flush-to-zero / default-NaN** *(if any FP)* — one register bit;
   denormals are catastrophic on both softfloat and real VFP/NEON. Unambiguous
   HW win, potentially large.
2. **Firmware build flags** — confirm the audio path is `-O3`/`-Ofast`, LTO on,
   `-mcpu=cortex-a9 -mfpu=neon`. Pure HW win, no behavioral change beyond normal
   optimization.
3. **Algorithmic (biggest portable wins)** — LUTs for transcendentals,
   block-based vs per-sample processing, **skip idle/silent voices** (often the
   single largest realistic win in a polyphonic synth, and obviously correct on
   HW), reduce inaudible oversampling.
4. **Fixed-point conversion** of a hot float path — *only* where it is a genuine
   HW improvement, bounded by an offline kernel null-test (see Phase 5) that
   quantifies any audible change.

## Phase 5 — Validate every change through two gates

1. **Portability gate** — would this reduce work, or be neutral, on a real A9
   running this firmware? (LUT vs transcendental: yes. Skip-silent-voice: yes.
   Reorder for TCG block boundaries: no — rejected.)
2. **Correctness gate** — does it change the audio output, and by how much? See
   "Validation reality" below for *how* — a whole-system byte-exact `.wav`/raw
   A/B diff does **not** work here, so the gate is per-change: sample-neutral by
   construction (structural changes), or an **offline kernel null-test** with a
   bounded error (intentional approximations like a LUT).

Then re-run the **identical Phase-1 benchmark** and track
guest-instructions-per-buffer and B/s (`DELUGEMU_SSIF_STATS=1`). Target: cross
352,800 B/s sustained.

### Validation reality (what actually works — measured)

The plan above assumed a byte-exact A/B audio diff (two `--icount` captures, or
`liblockstep`). **That does not work for firmware DSP changes here, for three
independently-fatal reasons measured directly:**

1. **QEMU's `wav` audiodev cannot capture this stream.** It rejects 32-bit
   formats (the SSIF voice is `AUDIO_FORMAT_S32`) and it sits *after* the SSIF's
   drift-correcting output resampler, which is host-timing dependent.
2. **The raw render capture is not bit-reproducible run-to-run.** A
   `DELUGEMU_SSIF_DUMP` capture (raw S32LE stereo, taken at the ring sampler
   *before* the resampler) of two identical same-build `--icount` runs is **not
   byte-identical** and not even uniformly lag-alignable (peak normalised
   cross-correlation ≈ 0.69 on the music region, with a drifting lag). Cause:
   `-icount …,sleep=on` **warps virtual time forward during guest idle**, so the
   sampler emits a non-uniform number of play-head frames per real second (a
   ~21 s run yielded ~140 *virtual*-seconds of audio); the warp pattern differs
   between runs. `sleep=off` does not fix it (still ~5× real-time), because the
   render-head clamp (`--tx-render-head`) does not bound the count in the
   headless `--audio none` config — the capture falls back to the wall-clock
   play head. (The per-*virtual*-second production rate is still correct: the
   `DELUGEMU_SSIF_STATS` probe reads a steady 352 800 B/s.)
3. **Even a perfect capture can't byte-match across firmware builds.** Oscillator
   phase accumulators free-run from boot, so any change that shifts note-trigger
   timing by one sample changes every subsequent sample — expected, not a
   regression.

**So the working correctness gates are:**

- **Structural / scheduler changes (9se.3):** sample-neutral *by construction* —
  they change *when* tasks run, not the DSP math or its inputs. Validate by code
  review (DSP call order and arguments unchanged) plus the `DELUGEMU_SSIF_STATS`
  production probe showing no throughput regression.
- **Intentional DSP approximations / LUTs (9se.4, 9se.5):** **offline kernel
  null-test** — drive the *old* and *new* math (e.g. `expf`/`logf` vs the LUT)
  over the full input domain in isolation (a small host harness, no emulator),
  and bound the worst-case error in dB / ULP. This is rigorous, deterministic,
  and is exactly the evidence a DelugeFirmware PR needs. Build the harness
  alongside the change it validates.
- **`DELUGEMU_SSIF_DUMP=<path>`** (raw S32LE stereo render capture) remains
  useful as a **spectral / by-ear spot-check** — confirm a change doesn't
  grossly break the sound — but treat it as perceptual, not byte-exact, and
  capture with `-icount …,sleep=off` to keep it closer to real time.

## Prerequisites / open items

- **DelugeFirmware source tree + `arm-none-eabi` toolchain** wired up locally so
  we can rebuild after each change (profiling alone needs only the existing
  `firmware/deluge.elf`).
- A canonical **worst-case project file** on the SD card that reliably triggers
  the breakup, so the benchmark exercises the true bottleneck.
- Decide the upstreaming path: land wins as DelugeFirmware PRs with the
  offline kernel null-test (or sample-neutrality) evidence attached.
