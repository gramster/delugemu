# Firmware performance — findings and tooling

## The problem in one paragraph

Under dense synthesis load the emulated Cortex-A9 can't always render audio at
real time, causing occasional breakup. This is a **TCG throughput limit, not a
buffering one**, and it cannot be closed from the host side: rebuilding QEMU
with `-O3 -march=native` gives **zero** improvement (the audio hot path is
JIT-generated guest code, not statically-compiled QEMU C), and the guest is
single-core so MTTCG can't help. The only lever is the **firmware**: do less
work in the render path. In TCG, cost is ≈ proportional to guest instructions
retired, so reducing guest instructions translates almost 1:1 into speedup. The
real-time target is **44100 × 8 = 352,800 B/s** sustained.

## Hard constraint: portable wins only

We pursue **only** changes that also help (or are neutral) on real Cortex-A9
hardware — the DelugeFirmware maintainers reject emulator-only changes. This
rules out softfloat→host-SSE shims, "scalar beats NEON under TCG" rewrites
(NEON is slower under TCG but faster on HW), and any TCG-block / translation-
boundary trick. Profiling is used **only to rank guest-instruction-count
hotspots**, which is a portable ranking; TCG does not model cache/pipeline
timing.

## Key finding: it's the scheduler, and you must profile the Release build

`firmware/deluge.elf` is byte-identical to `build/Debug/deluge.elf` (`-Og`, no
LTO). **Real hardware ships the Release build** (`-O2 -flto=auto`), which must be
built and profiled separately:

```sh
firmware/DelugeFirmware/toolchain/current/cmake/bin/cmake \
    --build firmware/DelugeFirmware/build --config Release   # ~46s
# -> firmware/DelugeFirmware/build/Release/deluge.elf
```

At `-O2` the scheduler helpers that dominate the *Debug* profile
(`Time::operator<=>`, `Task::isReady`, `Task::isRunnable`) are **fully inlined**
and vanish as symbols, and the `double` time constants fold away (no soft-float
in the hot path). The complete Release profile (dense `FAM1.XML`, no `--icount`,
`libhotblocks,limit=0`, 48 B instructions) is overwhelmingly the cooperative
task scheduler:

| Symbol | % of guest instructions |
|--------|------------------------|
| `TaskManager::chooseBestTask` | **74.9%** |
| `checkConditionalTasks` | 7.5% |
| `getSecondsFromStart` | 7.1% |
| `runHighestPriTask` + `runTask` | ~1.6% |
| **scheduler subtotal** | **~91%** |
| all audio DSP combined (Freeverb, `Sound::render`, compressor, filters, oscillators, voices) | **~7%** |

This holds at `--icount 2` and full host speed, and on both Debug and Release —
it is **not** an idle-throttle artifact. When no task is "ready" the dispatch
loop re-runs `chooseBestTask` (three O(N) passes over all tasks) +
`checkConditionalTasks` (O(25)) + `runHighestPriTask`, then loops — a tight
busy-spin. Source: `src/OSLikeStuff/task_scheduler/task_scheduler.cpp`.

**Consequence:** the DSP levers (compressor LUT, per-voice block processing)
target only a ~7% slice. The single material lever is `chooseBestTask` itself —
see "What we tried and ruled out" before touching it.

## Useful tools (built and proven)

| Tool | Where / how | Why it's useful |
|------|-------------|-----------------|
| **`libhotblocks`** | `qemu/build/contrib/plugins/`; pass via `run.sh … -- -plugin <path>/libhotblocks.dylib,inline=on,limit=0 -d plugin -D <log>` | **The** hotspot tool. Per-basic-block `pc, tcount, icount, ecount`; weight `icount × ecount`, bucket by `nm` symbol for a gprof-style flat profile. |
| **`scripts/dz_bucket.py`** | `dz_bucket.py <hb.txt> <deluge.elf> [top_n]` | Buckets a hotblocks report into a ranked %-of-guest-instructions table by symbol (uses `arm-none-eabi-nm -n` + `c++filt`). |
| **`scripts/dz_topblocks.py`** | `dz_topblocks.py <hb.txt> <deluge.elf> [top_n]` | Maps the hottest individual blocks to source lines via `addr2line` (Python — macOS `awk` lacks `strtonum`). Use to see *which lines* inside a hot function dominate. |
| **`scripts/dz_play.py`** | `dz_play.py /tmp/dz_qmp.sock "<timed key seq>"` | Scripted QMP key driver for a repeatable load. Canonical run: `"6.0:_wait 0:l:tap 3.0:ret:tap 8.0:spc:tap 14.0:_quit"` (boot 6 s, LOAD `l`, SELECT-click `ret` loads highlighted `FAM1`, PLAY `spc`). |
| **`--icount N`** (`run.sh --icount`) | built in | Deterministic, instruction-exact runs **and** the lever that forces CPU saturation (see idle dilution below). Use `--icount 2`; cross-check `4`. |
| **`DELUGEMU_SSIF_STATS=1`** | SSIF device, built in | Per-virtual-second audio production rate (B/s, % of 352,800), underruns/s, FIFO occupancy. Counted at the staging FIFO, so with `--tx-render-head <addr>` it reflects what was *actually rendered*. **Caveat: wall-clock delivery gate** — on a fast host it pins ~100% even under load. Use it to confirm no *regression*, not as the primary lever. |
| **`DELUGEMU_SSIF_DUMP=<path>`** | SSIF device, built in | Raw S32LE stereo capture **before** the resampler, for spectral / by-ear spot-checks. Perceptual only (see "Validation reality"). |
| **`arm-none-eabi-nm` / `-objdump` / `-addr2line` / `-c++filt`** | `/opt/homebrew/bin` | Map addresses to symbols/source. The ELF carries full `debug_info`. Re-derive the render head per build: `nm <elf> \| grep i2sTXBufferPos`. |
| **before-copy A/B** | `build/perf-baseline/deluge-release-before.elf` (gitignored) | Keep a pristine Release ELF when editing firmware so you can profile old vs new on the **same** workload. |

### Reproducing a profile

1. Build Release (above). Derive the render head:
   `arm-none-eabi-nm firmware/DelugeFirmware/build/Release/deluge.elf | grep i2sTXBufferPos`
   (currently `0x20031f2c`; the Debug ELF differs).
2. Launch headless with hotblocks:
   ```sh
   ./scripts/run.sh <elf> --display headless --audio none \
     --tx-render-head 0x20031f2c --sd build/deluge_sd.img \
     -- -qmp unix:/tmp/dz_qmp.sock,server,nowait \
     -plugin qemu/build/contrib/plugins/libhotblocks.dylib,inline=on,limit=0 \
     -d plugin -D /tmp/hb.txt </dev/null >/tmp/run.log 2>&1 &
   ```
3. Drive the load: `python3 scripts/dz_play.py /tmp/dz_qmp.sock "6.0:_wait 0:l:tap 3.0:ret:tap 8.0:spc:tap 14.0:_quit"`.
4. Bucket: `python3 scripts/dz_bucket.py /tmp/hb.txt <elf> 20`.

SD card: `./scripts/mksd.sh sdcard build/deluge_sd.img`. Worst-case song ranking:
`FAM1` (127 sounds) > `FAM3` (129) > `FAM2` (10) > `SONG010` (2).

**Idle-dilution caveat:** on a fast host the guest keeps up and idle-spins in
the scheduler, so an unsaturated profile is ~70% scheduler with DSP buried. Run
under `--icount 2` to saturate. At `--icount 4`+ the firmware's adaptive
voice-culling activates (synth voices drop, master-bus work remains) — read both
a moderate and a deep-saturation profile.

## Not useful here (don't reach for these)

- **`libhwprofile` / `libhotpages`** — `hwprofile` is a *device-IO* access
  profiler, not a guest-PC profiler. Wrong tool for instruction hotspots.
- **`libhowvec`** — its classifier is **AArch64-only**. The Deluge is ARM32
  Thumb-2, so everything shows "Unclassified"; you must decode top opcodes by
  hand. (We already know the render path is overwhelmingly fixed-point/integer.)
- **`liblockstep`** — finds the first execution divergence; useful for
  *emulator* determinism, **useless as a firmware-change audio gate** (two
  different firmware builds diverge immediately by construction).
- **`libcache`** — advisory only; TCG does not model real A9 cache/timing.
- **QEMU `-audiodev wav`** — rejects 32-bit formats (the SSIF voice is S32) and
  sits *after* the host-timing-dependent output resampler. Use `DELUGEMU_SSIF_DUMP`
  instead.
- **Host QEMU compiler flags (`-O3 -march=native`)** — zero effect on guest
  (JIT) code.

## Validation reality (what actually gates a change)

A whole-system byte-exact `.wav`/raw A/B diff **does not work here**, for three
independently-fatal reasons (all measured): the `wav` audiodev can't capture the
S32 stream; the `DELUGEMU_SSIF_DUMP` capture isn't bit-reproducible run-to-run
(`-icount …,sleep=on` warps virtual time during guest idle, so the sampler emits
a non-uniform frame count); and oscillator phase accumulators free-run from boot
so any one-sample timing shift changes every subsequent sample across builds.

Working gates instead:

- **Structural / scheduler changes:** sample-neutral *by construction* (they
  change *when* tasks run, not the DSP math or its inputs). Gate = code review
  (DSP call order and arguments unchanged) + `DELUGEMU_SSIF_STATS` showing no
  throughput regression.
- **Intentional DSP approximations / LUTs:** **offline kernel null-test** — drive
  old vs new math over the full input domain in a small host harness (no
  emulator) and bound the worst-case error in dB / ULP. Build the harness
  alongside the change.
- **`DELUGEMU_SSIF_DUMP`:** spectral / by-ear spot-check only; capture with
  `sleep=off` to stay closer to real time.

## What we tried and ruled out (do not retry)

- **Hand-inline the scheduler helpers** (`Time::operator<=>`, `isReady`, …) —
  **already done by `-O2`** on real hardware; redundant and would be rejected
  upstream. (Only appears as a win in the misleading *Debug* profile.)
- **`chooseBestTask`: collapse `isReady()`→`isReleased()` after `isRunnable()`
  + CSE the finish-time** — sound in theory, but in the idle regime the original
  `isReady()` calls are short-circuited to *zero* evaluations, so computing
  `isReleased()` unconditionally *adds* work: ~1300 → ~1407 instr/call (**+8%**).
- **`chooseBestTask`: O(1) idle early-out via cached `min(earliestCallTime)`** —
  the guard never fires. The spin is **not release-time-bound**: the audio task
  (priority 0, `RESOURCE_NONE`, `backOff ≈ 181 µs`) plus overdue tasks keep
  `min(earliestCallTime) ≤ now`, and readiness is gated by
  `idealCallTime`/`latestCallTime`/state/resources, not `earliestCallTime`. The
  only correct "is anything ready" test is the O(N) scan itself.

**A/B metric note:** the throughput gate is wall-clock-bound, so raw
`chooseBestTask` % and total instruction count are **not** valid A/B signals (a
cheaper call just lets the idle loop spin more). The only confound-free metric
is **instructions per `chooseBestTask` call** = (its total instructions) ÷ (its
entry-block execution count, col 4 of the hotblocks report at the symbol PC).

**Open lever (not attempted):** a real win needs an **event-driven next-wake**
redesign — compute the next time any task transitions to ready and skip dispatch
until then — on the maintainers' core scheduler. That carries genuine
timing-behavior risk, so it is deferred pending explicit direction.

## DSP levers (secondary, bounded by the ~7% share)

- **`RMSFeedbackCompressor`** (master bus, always-on) — `render` + `calcRMS`
  driven by `expf`/`logf`. Clean LUT candidate gated by an offline kernel
  null-test, but only ~0.5% of instructions.
- **Per-voice DSP** — `Freeverb::process` (~1%), `Sound::render` (~0.8%),
  `Lp`/`HpLadderFilter` (~0.6%), oscillator/wavetable kernels, voices. All
  fixed-point `long` (Q31). Block-processing raises the polyphony ceiling but
  cannot move the overall budget while the scheduler is ~91%.

Other clean HW-portable wins if/when the DSP becomes the bottleneck: FPSCR
flush-to-zero / default-NaN (if any FP appears — denormals are catastrophic on
softfloat *and* real VFP/NEON), skipping idle/silent voices, and reducing
inaudible oversampling.

## Prerequisites

- DelugeFirmware source + `arm-none-eabi` toolchain are wired up locally
  (profiling alone needs only the existing ELF; acting on findings needs the
  rebuild).
- Upstreaming path: land wins as DelugeFirmware PRs with sample-neutrality or
  offline-null-test evidence attached.
