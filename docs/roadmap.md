# Roadmap

Rough, ordered plan. Each milestone should leave the tree building and the
smoke test green.

## M0 — Skeleton (current)

- [x] Repo, license, docs, build/integration scripts.
- [x] QEMU submodule wiring (`scripts/bootstrap.sh`, `integrate.sh`).
- [x] Meson/Kconfig glue so `src/` compiles into `arm-softmmu`.
- [x] Stub board (`-M deluge`) + RZ/A1L SoC container with CPU, RAM regions and
      a logging catch-all over the peripheral space.
- [x] Device stubs: SCIF (serial), input PIC, OLED, 7-segment.

## M1 — Boot and trace

- [ ] Fetch a real QEMU revision into the submodule and get the skeleton
      building end-to-end on the developer machine + CI.
- [ ] Load DelugeFirmware ELF and reach the first instructions.
- [ ] Implement enough of CPG/BSC/INTC for the firmware's early init to proceed
      instead of faulting.
- [ ] Make SCIF emit real characters so early `printf`-style debug appears on
      `-serial stdio`.

## M2 — Core peripherals

- [ ] INTC → GIC interrupt routing for the peripherals in use.
- [ ] Timers driving the firmware's scheduler/tick.
- [ ] SD host interface backed by a disk image, enough to mount storage.
- [ ] GPIO model for board signals.

## M3 — Human interface

- [ ] Input PIC protocol: map host keyboard/controller to pad/button/encoder
      events the firmware understands.
- [ ] OLED rendering to a QEMU console; 7-segment rendering as an alternative.
- [ ] LED/PWM feedback path back from firmware to the rendered pad grid.

## M4 — Audio

- [x] SSI/I²S model wired to QEMU's audio backend for real-time output.
- [x] Audio input path.
- [ ] Verify against known firmware audio behaviour.

## M5 — Quality

- [x] Regression test suite booting firmware and asserting on display/serial.
- [x] CI matrix (Linux + macOS) building QEMU and running the suite.
- [x] Snapshot/savestate support for the custom devices (vmstate).
- [x] Documentation of each device's register coverage.

## Open questions

- Exact RZ/A1L peripheral register bases (need the hardware manual).
- Input PIC serial protocol details (reverse-engineer from firmware).
- Display controller command set and timing for the OLED variant.
- Which QEMU release to pin the submodule to for stability vs. features.
