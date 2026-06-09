# Debugging & Profiling

This guide covers the introspection, debugging, and profiling tools available to
firmware developers running the Deluge firmware under `delugemu`'s custom QEMU
build.

All examples assume you have built the emulator (`./scripts/build.sh`) and have a
firmware image at `firmware2/deluge.elf` and an SD image at
`build/deluge_sd.img`. The launcher [scripts/run.sh](scripts/run.sh) forwards
anything after a literal `--` straight to `qemu-system-arm`, so most QEMU flags
below are shown after `--`.

> **Functional, not cycle-accurate.** QEMU's TCG engine emulates *behaviour*,
> not the Cortex-A9 pipeline. Instruction/block counts are reliable for finding
> hotspots and relative regressions, but QEMU does **not** model cache latency,
> pipeline stalls, or true wall-clock cycles. Treat timing as approximate.

## Table of contents

- [GDB source-level debugging](#gdb-source-level-debugging)
- [Logging & exception tracing](#logging--exception-tracing)
- [Device-model debug output](#device-model-debug-output)
- [Performance profiling (TCG plugins)](#performance-profiling-tcg-plugins)
- [Memory profiling](#memory-profiling)
- [Deterministic execution (-icount)](#deterministic-execution--icount)
- [QEMU trace events](#qemu-trace-events)
- [Monitor / QMP introspection](#monitor--qmp-introspection)
- [What is not available](#what-is-not-available)

## GDB source-level debugging

The QEMU gdbstub is built in. Launch with `-S` (freeze the CPU at reset) and/or
`-s` (open a gdbstub on `localhost:1234`):

```sh
# Freeze at reset and wait for a debugger on :1234
./scripts/run.sh firmware2/deluge.elf --sd build/deluge_sd.img -S -s
```

`firmware2/deluge.elf` is **unstripped with full C++ debug info**, so symbolic
breakpoints and backtraces resolve demangled names. A debugger is available on
this machine at `/opt/homebrew/bin/gdb` (Homebrew GDB); `lldb` also works.

```sh
# In a second terminal
gdb firmware2/deluge.elf \
    -ex 'target remote :1234' \
    -ex 'break AudioEngine::routine' \
    -ex 'continue'
```

Useful once attached:

| Command | Purpose |
|---------|---------|
| `break <symbol>` / `break *0xADDR` | Set a breakpoint (symbolic or by address) |
| `watch *(uint32_t*)0xADDR` | Data watchpoint — break when memory changes |
| `stepi` / `nexti` | Single-step one instruction |
| `info registers` | Dump ARM core registers |
| `bt` | Backtrace |
| `x/64wx 0x0C000000` | Examine 64 words of SDRAM |
| `monitor <hmp-cmd>` | Run any HMP monitor command (see below) |

Memory bases for `x`/watchpoints (from
[src/include/hw/arm/rza1l_soc.h](src/include/hw/arm/rza1l_soc.h)):

- On-chip **SRAM**: `0x20000000` (firmware loads/executes here)
- External **SDRAM**: `0x0C000000`

### Scripted (non-interactive) GDB

For repeatable inspection, drive GDB in batch mode against a detached emulator:

```sh
# Launch detached with the gdbstub open
qemu/build/qemu-system-arm -M deluge -kernel firmware2/deluge.elf \
    -drive if=sd,format=raw,file=build/deluge_sd.img \
    -display none -s &

# Run a canned script (only one GDB client may attach at a time)
gdb -nx -batch -x my_probe.gdb
```

## Logging & exception tracing

QEMU's `-d` flag selects log categories; `-D <file>` redirects them. List all
categories with `qemu-system-arm -d help`. The most useful:

| Category | Shows |
|----------|-------|
| `int` | Interrupts/exceptions in short form (used by the boot test) |
| `unimp` | Accesses to unimplemented device functionality |
| `guest_errors` | Guest doing something invalid (e.g. bad MMIO) |
| `in_asm` | Target disassembly per translation block (verbose) |
| `exec` | Trace before each executed block (very verbose) |
| `cpu` | CPU registers before each block (very verbose) |

```sh
# Log exceptions + bad accesses to a file
./scripts/run.sh firmware2/deluge.elf --sd build/deluge_sd.img -- \
    -d int,unimp,guest_errors -D /tmp/deluge.log
```

The boot regression test [tests/boot.sh](tests/boot.sh) uses exactly this. It
counts exception lines to assert a healthy boot — the ARM exception numbers it
greps for are:

- `Taking exception 3 [` — Prefetch Abort (bad instruction fetch)
- `Taking exception 4 [` — Data Abort (bad/unmapped data access)
- `Taking exception 5 [` — IRQ (proves the timer-driven main loop is running)

A healthy boot shows **zero** aborts and a steady stream of IRQs.

> Several regions (`rza1l.boot.mirror`, `rza1l.io.mid/high`) are deliberate
> catch-alls the firmware probes constantly, so `unimp` warnings about them are
> expected and not failures.

## Device-model debug output

Some custom device models emit verbose tracing to stderr. The USB host model is
gated behind an environment variable (see
[src/hw/usb/rza1l_usb.c](src/hw/usb/rza1l_usb.c)):

```sh
# Trace USB control transfers, register access, and state transitions
RZA1L_USB_DEBUG=1 ./scripts/run.sh firmware2/deluge.elf --usb-midi pty
```

The PIC input model and the panel skin/input layer also `fprintf` events
(pad/button presses, encoder steps, latch toggles) to stderr during normal runs,
which is handy when verifying that host input reaches the firmware.

## Performance profiling (TCG plugins)

The TCG plugin system is **compiled and enabled** (`CONFIG_PLUGIN` is defined and
the binary accepts `-plugin`). Prebuilt plugins live in
`qemu/build/contrib/plugins/` (and test plugins in
`qemu/build/tests/tcg/plugins/`). Load them with `-plugin`:

```sh
# Hottest basic blocks — where execution time concentrates
./scripts/run.sh firmware2/deluge.elf --sd build/deluge_sd.img -- \
    -plugin qemu/build/contrib/plugins/libhotblocks.dylib

# Instructions per second (throughput)
./scripts/run.sh firmware2/deluge.elf -- \
    -plugin qemu/build/contrib/plugins/libips.dylib

# Per-address execution profile (hwprofile) + control-flow graph (cflow)
./scripts/run.sh firmware2/deluge.elf -- \
    -plugin qemu/build/contrib/plugins/libhwprofile.dylib \
    -plugin qemu/build/contrib/plugins/libcflow.dylib
```

Plugins available in `qemu/build/contrib/plugins/`:

| Plugin | Purpose |
|--------|---------|
| `libhotblocks.dylib` | Most-executed basic blocks |
| `libhotpages.dylib` | Most-accessed memory pages |
| `libhwprofile.dylib` | Per-address execution counts |
| `libips.dylib` | Instructions per second |
| `libcache.dylib` | Cache hit/miss simulation |
| `libcflow.dylib` | Control-flow graph |
| `libbbv.dylib` | Basic-block vectors (SimPoint) |
| `libdrcov.dylib` | Coverage in DynamoRIO `drcov` format |
| `libexeclog.dylib` | Per-instruction execution log (very verbose) |
| `libuftrace.dylib` | uftrace-compatible call traces |
| `liblockstep.dylib` | Compare two instances in lockstep |
| `libstoptrigger.dylib` | Halt on a condition |
| `libtraps.dylib` | Trap/syscall accounting |

Because the firmware ELF carries debug symbols, the address-based plugin output
can be mapped back to functions with `addr2line`/`objdump`:

```sh
arm-none-eabi-objdump -d firmware2/deluge.elf | less   # cross-reference hot addrs
```

## Memory profiling

QEMU-side memory introspection:

```sh
# Memory access hotspots, grouped by page
./scripts/run.sh firmware2/deluge.elf --sd build/deluge_sd.img -- \
    -plugin qemu/build/contrib/plugins/libhotpages.dylib

# Full load/store trace (test plugin)
./scripts/run.sh firmware2/deluge.elf -- \
    -plugin qemu/build/tests/tcg/plugins/libmem.dylib
```

The firmware uses its own allocator ([firmware2/malloc.c](firmware2/malloc.c))
and does not export a heap meter to the host. To watch allocations or specific
structures, use **GDB watchpoints** on the relevant addresses, or inspect the
memory map live:

```sh
gdb firmware2/deluge.elf -ex 'target remote :1234'
(gdb) monitor info mtree     # device + memory-region tree
(gdb) watch *(uint32_t*)0x20000000
```

## Deterministic execution (-icount)

`-icount` ties the virtual clock to an instruction count for reproducible,
deterministic runs (useful when a bug depends on precise timing). `run.sh` has
an `--icount [shift]` convenience flag that wires it up with `sleep=on`:

```sh
./scripts/run.sh firmware2/deluge.elf --sd build/deluge_sd.img --icount 0
# equivalent to the raw form:
./scripts/run.sh firmware2/deluge.elf --sd build/deluge_sd.img -- -icount shift=0
```

`shift=0` runs one virtual-clock tick per instruction; larger shifts trade
precision for speed (`--icount auto` / `shift=auto` lets QEMU pick).

Beyond determinism, `--icount` also paces the guest to a virtual clock locked to
real time, which makes audio internally consistent — no stale-ring distortion or
dropouts. The trade-off is that it caps the guest to **≤ real time**, so under
heavy DSP load playback runs slow (lower pitch/tempo) instead of breaking up.
That makes it good for clean offline capture, but not for live external-MIDI
play. For live use, prefer the default free-running clock with the render-head
clamp (`--tx-render-head`) and a generous `--audio-buffer`.

## QEMU trace events

QEMU's structured trace system is enabled (log backend). List events with the
`info trace-events` monitor command, and enable them with `-trace`:

```sh
# Trace all memory-region op events
./scripts/run.sh firmware2/deluge.elf -- -trace 'memory_region_ops_*' -D /tmp/trace.log

# Enable several patterns from a file (one glob per line)
printf '%s\n' 'gdbstub_*' 'memory_region_ops_*' > /tmp/patterns
./scripts/run.sh firmware2/deluge.elf -- -trace events=/tmp/patterns
```

The custom device models in `src/` do not yet define their own `trace-events`
(they use `fprintf` debug output instead), but the infrastructure is available
if you want to add structured trace points. See
[qemu/docs/devel/tracing.rst](qemu/docs/devel/tracing.rst).

## Monitor / QMP introspection

The HMP (human monitor) gives live machine state. Reach it via `gdb`'s
`monitor` passthrough, or expose `-serial mon:stdio` / a dedicated `-monitor`.

| HMP command | Shows |
|-------------|-------|
| `info registers` | CPU register state |
| `info mtree` | Memory-region tree (peripherals + RAM) |
| `info qtree` | QOM device tree |
| `info tlb` | MMU translation entries |
| `x/32wx 0x20000000` | Examine guest memory |
| `info trace-events` | Available trace events |

QMP (the JSON control channel) is the scriptable equivalent and is what the
panel-automation helpers use — injecting input and capturing screenshots. See
the [QMP section in tests/README.md](tests/README.md#driving-the-panel-over-qmp)
for how to expose `-qmp`, the `input-send-event` / `screendump` commands, and the
`scripts/press_key.py` / `scripts/enc_test.py` helpers.

## What is not available

- **Cycle-accurate timing / Cortex-A9 PMU cycle counts.** TCG is functional;
  there is no per-cycle model. Use instruction/block counts as a proxy.
- **A firmware-side CPU-load or heap meter exposed to the host.** The SEGGER RTT
  sources in `firmware2/src/RTT/` exist but are not wired up for QEMU; profiling
  is done from the QEMU side (plugins, GDB, logs).
