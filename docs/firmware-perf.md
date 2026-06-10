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
| **`liblockstep.dll`** | `qemu/build/contrib/plugins/` | Run two builds in lockstep to **prove a change is bit-identical** (or locate the first divergence). The evidence a firmware PR needs. |
| **`libhwprofile.dll` / `libhotpages.dll`** | `qemu/build/contrib/plugins/` | **NOTE: `hwprofile` is a *device-IO* access profiler, NOT a guest-PC profiler** — do not use it for instruction hotspots (use `hotblocks`). `hotpages` = most-accessed memory pages (cross-check). |
| **`libcache.dll`** | `qemu/build/contrib/plugins/` | *Advisory only* — TCG does not model real A9 timing, but the cache simulator hints at memory-bound spots worth reasoning about for HW. |
| **`DELUGEMU_SSIF_STATS` probe** | SSIF device (built in) | The real-world success signal. Set `DELUGEMU_SSIF_STATS=1` in the environment and the SSIF device prints, once per second of virtual time, the freshly-rendered guest audio production rate (B/s and % of 352,800), primed-FIFO underruns/s, and staging-FIFO occupancy (ms). Production is counted where finished frames enter the staging FIFO, so with `--tx-render-head <addr>` it reflects what the firmware *actually rendered* (not the wall-clock play head). **Caveat:** this is a *wall-clock* delivery gate — on a host fast enough to keep the guest real-time it pins at ~100% even under heavy `--icount`, because the firmware really is meeting real-time. It drops below 100% (with underruns) only when the host cannot render in real time. Use it to confirm a change does not *regress* delivery and to gate on slower hosts / heavier songs; use `libhotblocks`/`libips` (instruction budget) as the primary day-to-day lever on a fast Mac. |
| **`arm-none-eabi-nm` / `-objdump` / `-addr2line`** | toolchain | Map plugin output addresses back to firmware symbols/source. `firmware/deluge.elf` carries full `debug_info` (not stripped); source is in `firmware/DelugeFirmware/src`. |
| **GDB (`-S -s`, `target remote :1234`)** | built in | Watchpoints on specific addresses; live inspection of DSP state and the memory map. |
| **`scripts/press_key.py` + QMP** | in-tree | Scripted, repeatable note input so the benchmark exercises the same load every run. |
| **`-audiodev wav`** | built in | Capture deterministic `.wav` output for A/B bit-exact comparison (pairs with `--icount`). |
| **DelugeFirmware source + `arm-none-eabi` toolchain** | *external (needed)* | Required to *act* on findings. Profiling needs only the existing ELF; rebuilding the firmware needs the source tree and cross-toolchain. |

## Phase 1 — Lock down a reproducible benchmark

- Run under **`--icount`** so the profile is deterministic and instrumentation
  does not perturb timing.
- Fix the inputs: one canonical **worst-case project** on a fixed SD image plus
  a scripted note sequence (`press_key.py` over QMP) for a fixed virtual
  duration.
- Capture the baseline: total guest instructions (`libips`), buffers rendered,
  B/s production and underruns (`DELUGEMU_SSIF_STATS=1`), and a reference
  `.wav` (`-audiodev wav`) for later bit-exact comparison.

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

## First-pass results (FAM1, `--icount 2` & `--icount 4`)

Measured against the dense `FAM1.XML` (127 synth instruments) under forced
saturation. The render path is **overwhelmingly fixed-point** (top opcodes are
all integer `cmp`/`ldr`/`b`/`bx`; float is a minor share). Three cost centres,
ranked:

1. **Cooperative task scheduler — the #1 cost, ~25–47% of *all* guest
   instructions, persistent across every load level (fully portable).**
   `Time::operator<=>` (19–27%) + `TaskManager::chooseBestTask` (15–21%) +
   `Task::isReady`. Root: `src/OSLikeStuff/task_scheduler/task_scheduler.cpp`.
   The dispatcher (`start`/`yield`) calls `chooseBestTask` **before every task
   run**; it is `O(numActiveTasks)`, rescans the whole list each time, and does
   several 64-bit `Time` comparisons per task. The audio engine yields
   frequently, so this scan runs thousands of times/sec. `Time` is an
   `int64_t` tick count (`clock_type.h`) with a *defaulted* `operator<=>` →
   multi-instruction 64-bit compare on the 32-bit A9. The scheduler also mixes
   `double` time constants (`Time(0.003)`, `*= 0.1`, `operator double`), which
   pull in the libgcc soft-float helpers `__fixdfdi` / `__fixunsdfdi` /
   `__udivmoddi4` (a further ~2–3%). Candidate levers (all portable HW wins):
   maintain the next-deadline incrementally instead of an O(N) rescan; remove
   `double` from the hot path (precompute `Time` constants once); render a
   larger audio block before yielding to amortize dispatch (trades latency).
2. **`RMSFeedbackCompressor` (master bus) — ~3–11%, always-on (portable).**
   `render` + `calcRMS` + `AbsValueFollower::calcApproxRMS`, driven by `expf` /
   `logf`. It runs on the master output regardless of voice count, so it is not
   culled. Classic LUT/fixed-point-approximation candidate; gate the audible
   delta with a lockstep/wav diff.
3. **Per-voice DSP — cullable, dominates at realistic polyphony.** `Freeverb`
   (`Comb::process` + `Freeverb::process`, the single biggest DSP item ~10% at
   `--icount 2`), `Lp`/`HpLadderFilter::doFilter`/`doFilterStereo` (~7%),
   `Voice::render` (~3%), and the oscillator/wavetable kernels
   (`renderWave`/`waveRenderingFunctionGeneral`/`renderOsc`/`renderPulseWave`).
   All fixed-point `long` (Q31). Wins here = raw instruction volume
   (block-based vs per-sample), and they raise the polyphony ceiling.

Strategic read: the scheduler is the largest single win and is independent of
the synth workload, so it should be tackled first; the compressor is a clean,
contained LUT win; the per-voice kernels are where instruction-volume work
raises the voice count the box can sustain.

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
   HW improvement, bounded by a lockstep/wav diff that quantifies any audible
   change.

## Phase 5 — Validate every change through two gates

1. **Portability gate** — would this reduce work, or be neutral, on a real A9
   running this firmware? (LUT vs transcendental: yes. Skip-silent-voice: yes.
   Reorder for TCG block boundaries: no — rejected.)
2. **Bit-exactness gate** — does it change the audio output? Diff two `--icount`
   `.wav` captures (or use `liblockstep`) to prove a change is sample-identical,
   or to quantify the delta for an intentional approximation (e.g. a LUT). This
   is exactly the evidence a firmware PR needs.

Then re-run the **identical Phase-1 benchmark** and track
guest-instructions-per-buffer and B/s. Target: cross 352,800 B/s sustained.

## Prerequisites / open items

- **DelugeFirmware source tree + `arm-none-eabi` toolchain** wired up locally so
  we can rebuild after each change (profiling alone needs only the existing
  `firmware/deluge.elf`).
- A canonical **worst-case project file** on the SD card that reliably triggers
  the breakup, so the benchmark exercises the true bottleneck.
- Decide the upstreaming path: land wins as DelugeFirmware PRs with the
  lockstep/wav evidence attached.
