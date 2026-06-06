# Architecture

How delugemu is structured and how it plugs into QEMU.

## Design goals

1. **Boot unmodified firmware.** The emulator targets the real, open-source
   DelugeFirmware ELF — no special build of the firmware required.
2. **Keep upstream QEMU pristine.** QEMU is a git submodule we never hand-edit.
   All device models live in this repo and are linked in at build time. This
   keeps rebasing onto new QEMU releases cheap.
3. **Incremental fidelity.** Start with the SoC skeleton (CPU + RAM + a logging
   catch-all for peripherals) so firmware can be loaded and traced, then flesh
   out peripherals one at a time, guided by what the firmware actually touches.

## QEMU integration model

```
                    delugemu repo                         QEMU submodule
        ┌──────────────────────────────────┐      ┌──────────────────────────┐
        │ src/                              │      │ qemu/                    │
        │   hw/arm/deluge.c    (board)      │      │   hw/   meson.build  ◄───┼─┐
        │   hw/arm/rza1l_soc.c (SoC)        │      │   target/arm/...         │ │
        │   hw/display/...                  │      │   system/, accel/, ...   │ │
        │   hw/misc/deluge_pic.c            │      │                          │ │
        │   include/hw/...                  │      │                          │ │
        │   meson.build  ──────────────────┼──────┼─► (compiled into          │ │
        └──────────────────────────────────┘      │     arm-softmmu target)  │ │
                    ▲                              └──────────────────────────┘ │
                    │ symlinked as qemu/hw/deluge, plus one guarded             │
                    │ `subdir('deluge')` line appended to hw/meson.build  ──────┘
              scripts/integrate.sh
```

`scripts/integrate.sh`:

1. Symlinks `src/` to `qemu/hw/deluge`.
2. Symlinks our public headers into `qemu/include/hw/<category>/` so they
   resolve on the normal compiler include path.
3. Appends a single, marker-guarded `subdir('deluge')` to `qemu/hw/meson.build`.
4. Appends a single, marker-guarded `source deluge/Kconfig` to `qemu/hw/Kconfig`.

Our `src/meson.build` then adds the model sources to the `arm-softmmu` target's
source set, conditional on a `CONFIG_DELUGE` Kconfig switch
(`src/Kconfig`). Running `integrate.sh --undo` removes the symlinks and the
injected lines, restoring a pristine tree.

### Keeping the submodule working tree clean

QEMU has no out-of-tree device build, so the four steps above necessarily touch
files *inside* the submodule. Left unmanaged that shows up as a dirty submodule
(`m qemu` in the parent repo) plus a pile of untracked symlinks — both easy to
stage, commit, or clobber by accident. To keep design goal #2 honest,
`integrate.sh` makes the injection invisible to git, locally and reversibly:

- The two appended tracked files (`hw/meson.build`, `hw/Kconfig`) are marked
  `git update-index --skip-worktree`, so git ignores our local edits and the
  submodule reports clean.
- The injected symlinks (`hw/deluge`, the `include/hw/*/*.h` headers) are added
  to the submodule's `.git/info/exclude`, so they never appear as untracked and
  cannot be `git add`ed into QEMU.

Both are local to the checkout (never committed anywhere) and the recorded
submodule commit is never changed. `integrate.sh --undo` clears the
skip-worktree bits, strips the exclude block, and removes the appended lines
byte-for-byte, returning the submodule to a pristine state. Because
skip-worktree pins those two files, run `integrate.sh --undo` before bumping the
pinned QEMU revision (otherwise the checkout of `hw/meson.build`/`hw/Kconfig`
would be blocked).

## Object model (QOM)

QEMU models hardware with the QEMU Object Model. The intended hierarchy:

```
TYPE_DELUGE_MACHINE            (hw/arm/deluge.c)        — the board / -M deluge
   └── owns TYPE_RZA1L_SOC     (hw/arm/rza1l_soc.c)     — SoC container
          ├── Cortex-A9 CPU    (QEMU built-in)
          ├── GIC + timers     (QEMU built-in, wired by the SoC)
          ├── on-chip SRAM     (MemoryRegion, 3 MB @ 0x20000000)
          ├── SDRAM            (MemoryRegion, 64 MB @ 0x0C000000)
          ├── unimplemented    (catch-all over the peripheral region)
          ├── TYPE_RZA1L_SCIF  (serial)        ── stub
          └── ...              (SSI, SD, INTC, CPG, BSC — added incrementally)

   Board-level (created by the machine, wired to SoC buses):
      ├── TYPE_DELUGE_PIC      (hw/misc/deluge_pic.c)   — input bridge   — stub
      ├── TYPE_DELUGE_OLED     (hw/display/deluge_oled.c)               — stub
      └── TYPE_DELUGE_SEG      (hw/display/deluge_segment.c)            — stub
```

Each model follows the standard QEMU device pattern: a `TypeInfo` registered via
`type_init()`, an instance struct embedding `SysBusDevice`/`DeviceState`,
`realize`/`reset` methods, and (for MMIO devices) a `MemoryRegion` with
`MemoryRegionOps` read/write callbacks.

## Boot flow

1. The machine creates the SoC, which creates the CPU and maps RAM.
2. The machine loads the firmware via `-kernel` using QEMU's ELF loader; the
   ELF entry point sets the CPU reset PC.
3. The CPU starts executing `start` from on-chip SRAM, sets up the MMU, and runs
   the application.
4. Peripheral accesses hit either a real model or the logging catch-all
   (visible with `-d unimp,guest_errors`), which guides which device to model
   next.

## Why a SoC container device

Wrapping the CPU, interrupt controller, timers and RAM in a single
`TYPE_RZA1L_SOC` device (rather than building everything directly in the
machine) keeps the board file small, makes the SoC reusable, and mirrors how
QEMU models other SoCs (e.g. `bcm2836`, `stm32f405`). The board file then only
deals with what is specific to the Deluge: the PIC, the display and audio.

## Testing

See [`tests/`](../tests). The first milestone is a smoke test that builds
`qemu-system-arm`, confirms `-M deluge` is registered, and boots far enough to
emit early serial output. Functional tests grow alongside peripheral fidelity.
