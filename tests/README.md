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

## `midi-coremidi.sh`

Validates the host CoreMIDI bridge (`scripts/midi_bridge.c`) end to end on
macOS, **without** QEMU or firmware. The test plays both roles around the
bridge: it is the UNIX-socket server the bridge connects to (the same
`-chardev socket,...,server=on,wait=off` contract `run.sh` uses for a `coremidi`
transport), and it is the "DAW" opening the bridge's virtual CoreMIDI ports via
`python-rtmidi`. It checks both directions and the platform-agnostic stream
parser:

1. a CC sent to the virtual **output** port arrives on the socket as raw MIDI,
2. raw bytes written to the socket — using **running status**, an interleaved
   **real-time** byte and a **SysEx** — are framed into the correct discrete
   messages on the virtual **input** port.

```sh
./tests/midi-coremidi.sh
```

It **skips cleanly** (exit 0) when not on macOS or when `python-rtmidi` is not
installed.

## Adding tests

Keep tests runnable standalone and CI-friendly: exit non-zero on failure, avoid
interactive prompts, and don't depend on a real Deluge firmware image unless the
test explicitly fetches/locates one and skips cleanly when it is absent.

## Driving the panel over QMP

QMP (the **QEMU Machine Protocol**) is QEMU's JSON control channel: a socket on
which you send commands to and query a running instance. We use it to drive the
emulated front panel programmatically — injecting mouse/keyboard input and
capturing the framebuffer — so panel behaviour can be exercised without a human
at the window.

**Expose the socket.** Pass a `-qmp` chardev through `run.sh` (anything after
`--` goes straight to QEMU). A Unix socket under `/tmp` is convenient and is
what the helper scripts expect:

```sh
./scripts/run.sh firmware2/deluge.elf --sd build/deluge_sd.img \
    --display console -- -qmp unix:/tmp/dz_qmp.sock,server,nowait
```

The socket is **single-client** (`nowait`): only one tool may be attached at a
time, so close one connection before opening the next.

**Talk to it.** Connect to the socket and exchange newline-delimited JSON. After
the greeting you must send `qmp_capabilities` once to leave negotiation, then
issue commands:

```sh
printf '%s\r\n' \
  '{"execute":"qmp_capabilities"}' \
  '{"execute":"human-monitor-command","arguments":{"command-line":"screendump /tmp/x.ppm"}}' \
  | nc -U -w1 /tmp/dz_qmp.sock
sips -s format png /tmp/x.ppm --out /tmp/x.png    # PPM -> PNG (macOS)
```

The two commands that matter for panel testing are:

- **`input-send-event`** — inject synthetic input. An `abs` x/y event moves the
  pointer (axis values are scaled `0..0x7fff` across the 2256×1584 skin), a
  `btn` `left` down/up pair is a click, `wheel-up`/`wheel-down` turn an encoder,
  and a `key` event with a `qcode` presses a bound key. To latch multiple
  controls, send a `key` `alt` down, click several pads/buttons, then `alt` up.
- **`screendump`** (via `human-monitor-command`, or the QMP `screendump`
  command) — write the current framebuffer to a PPM you can inspect or diff.

**Helper scripts.** Two ready-made drivers live in `scripts/` and assume the
socket at `/tmp/dz_qmp.sock`:

```sh
./scripts/press_key.py <qcode> <name>          # press a key, then screendump
./scripts/enc_test.py  <x> <y> <label> [up] [dn] # wheel an encoder at (x,y), screendump
```

