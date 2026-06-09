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
| **`-icount` (`run.sh --icount`)** | built in | Deterministic, instruction-exact, timing-independent runs so a fully-instrumented profile does not perturb results and is byte-for-byte reproducible. The benchmark backbone. |
| **`libhwprofile.dll`** | `qemu/build/contrib/plugins/` | Per-address execution counts → bucketed into a **ranked per-function instruction profile**. The single most actionable artifact. |
| **`libhowvec.dll`** | `qemu/build/contrib/plugins/` | Instruction-class mix (FP vs NEON vs scalar integer). Settles the float-vs-fixed-point question that decides the strategy. |
| **`libips.dll`** | `qemu/build/contrib/plugins/` | Instructions-per-second / total instruction throughput for the baseline and for measuring each change. |
| **`libuftrace.dll`** (+ `uftrace_symbols.py`) | `qemu/build/contrib/plugins/` | Call-graph / flamegraph for **inclusive** cost, so we find the heavy subtree, not just leaf functions. |
| **`liblockstep.dll`** | `qemu/build/contrib/plugins/` | Run two builds in lockstep to **prove a change is bit-identical** (or locate the first divergence). The evidence a firmware PR needs. |
| **`libhotblocks.dll` / `libhotpages.dll`** | `qemu/build/contrib/plugins/` | Cross-checks: hottest basic blocks and most-accessed pages. |
| **`libcache.dll`** | `qemu/build/contrib/plugins/` | *Advisory only* — TCG does not model real A9 timing, but the cache simulator hints at memory-bound spots worth reasoning about for HW. |
| **`DELUGEMU_SSIF_STATS` probe** | in-tree (SSIF device) | Measures actual guest audio production (B/s), underruns, FIFO occupancy — the real-world success signal. |
| **`arm-none-eabi-nm` / `-objdump` / `-addr2line`** | toolchain | Map plugin output addresses back to firmware symbols/source. `firmware2/deluge.elf` carries debug symbols. |
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
  B/s production and underruns (`DELUGEMU_SSIF_STATS`), and a reference `.wav`
  (`-audiodev wav`) for later bit-exact comparison.

## Phase 2 — Find where the instructions go

1. **`libhwprofile.dll`** → per-address counts. Bucket addresses by symbol
   ranges from `arm-none-eabi-nm`/`objdump` on `firmware2/deluge.elf` to produce
   a gprof-style **ranked-by-%-of-guest-instructions** flat profile.
2. **`libhowvec.dll`** → instruction-class mix. Resolves the strategic fork:
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
  `firmware2/deluge.elf`).
- A canonical **worst-case project file** on the SD card that reliably triggers
  the breakup, so the benchmark exercises the true bottleneck.
- Decide the upstreaming path: land wins as DelugeFirmware PRs with the
  lockstep/wav evidence attached.
