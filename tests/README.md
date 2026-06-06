# Tests

Emulation and regression tests for delugemu.

## `smoke.sh`

The M0 acceptance check. After building (`scripts/build.sh`) it verifies that:

1. `qemu-system-arm` was produced, and
2. the `deluge` machine type is registered and can be instantiated.

```sh
./scripts/bootstrap.sh   # once
./scripts/integrate.sh   # once (re-run after adding/removing source files)
./scripts/build.sh
./tests/smoke.sh
```

It deliberately does **not** boot firmware — see `boot.sh` for that.

## `boot.sh`

The firmware-boot regression test. It boots a Deluge firmware image headless and
asserts that the boot reaches a healthy steady state:

1. **zero** Data Abort exceptions (a bad/unmapped MMIO access would raise one),
2. **zero** Prefetch Abort exceptions (a bad instruction fetch), and
3. a stream of IRQ exceptions (proves we reached the timer-driven main loop
   instead of spinning at the vector table).

Unimplemented-device-access warnings are **not** failures: several regions
(`rza1l.boot.mirror`, `rza1l.io.mid/high`) are deliberate catch-alls the
firmware probes constantly.

```sh
./tests/boot.sh                      # auto-locate firmware + SD image
DELUGE_FIRMWARE=path ./tests/boot.sh # explicit firmware
BOOT_SECONDS=15 ./tests/boot.sh      # widen the run window
```

Firmware is not shipped with the repo, so the test looks for
`firmware2/deluge.elf` then `firmware/deluge.elf` (override with
`DELUGE_FIRMWARE`) and **skips cleanly** (exit 0) when none is present. If
`build/deluge_sd.img` exists it also boots with the card attached, exercising
the SDHI path. Observed healthy counts: 19 IRQs without an SD image, 183 with
one, and 0 aborts in both cases.

## `usb-midi.sh`

The USB host-MIDI enumeration regression test. It boots a firmware image with a
synthetic full-speed USB-MIDI device attached on the host port
(`-global rza1l-usb.midi=on`) and asserts that the firmware's USB host stack
enumerates it:

1. the control-transfer state machine reaches **SET_CONFIGURATION**
   (`RZA1L_USB_DEBUG` traces `SET_CONFIGURATION 1 -> configured=1`),
2. **zero** Data Abort / Prefetch Abort exceptions, and
3. the USBI interrupt actually fires (a non-trivial IRQ stream).

```sh
./tests/usb-midi.sh
DELUGE_FIRMWARE=path ./tests/usb-midi.sh
RUN_SECONDS=15 ./tests/usb-midi.sh
```

Like `boot.sh`, it **skips cleanly** (exit 0) when no firmware image is present.
Observed healthy counts: 44 IRQs, 0 aborts, 1 SET_CONFIGURATION.

## Adding tests

Keep tests runnable standalone and CI-friendly: exit non-zero on failure, avoid
interactive prompts, and don't depend on a real Deluge firmware image unless the
test explicitly fetches/locates one and skips cleanly when it is absent.
