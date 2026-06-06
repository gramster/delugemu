# Deluge hardware reference

Notes on the Synthstrom Deluge hardware, as far as is needed to emulate it.
Where a fact is confirmed from the open-source firmware (linker script, register
defines) it is marked **(fw)**; values still to be verified against the Renesas
RZ/A1L hardware manual are marked **(TBD)**.

## SoC: Renesas RZ/A1L (R7S721031)

| Property        | Value                                                       |
| --------------- | ----------------------------------------------------------- |
| Core            | Arm Cortex-A9 (single core)                                 |
| Clock           | 400 MHz                                                      |
| On-chip RAM     | 3 MB **(fw)**                                               |
| Cache           | L1 I/D + PL310 L2 cache controller                          |
| Interrupts      | Arm GIC (PL390-style) integrated in the Cortex-A9 MPCore    |
| MMU             | ARMv7-A with L1 translation table set up by firmware **(fw)** |

The RZ/A1L is the smaller-SRAM sibling of the RZ/A1H (10 MB). The Deluge uses
the 3 MB part; the firmware's `linker_script_rz_a1l.ld` defines the on-chip RAM
region `RAM012L` as `ORIGIN = 0x20000000, LENGTH = 0x00300000`.

## Board-level components

| Component        | Connection            | Notes                                  |
| ---------------- | --------------------- | -------------------------------------- |
| External SDRAM   | Bus state controller, CS3 | 64 MB device; firmware maps a 64 MB window at `0x0C000000` **(fw)** |
| SPI flash (ROM)  | SPIBSC, mapped        | `0x18000000`, 32 MB window **(fw)**    |
| Display          | OLED 128×48 *or* 7-segment | Build/runtime selectable          |
| Input PIC        | Serial link to main SoC | Scans pad matrix + encoders, reports events |
| Audio codec      | SSI / I²S             | Stereo audio in/out                    |
| SD card          | SD host interface     | Sample + project storage               |
| MIDI / USB / CV/gate | Various             | Out of scope for the initial skeleton  |

### Input PIC

Button, pad and encoder scanning is offloaded to a small PIC microcontroller.
It communicates with the main SoC over a serial channel, delivering input events
and receiving LED/PWM updates for the RGB pads. In the emulator this is modelled
as a single device that bridges host input (keyboard/UI) to the protocol the
firmware expects, rather than emulating the PIC's own instruction set.

### Display

Two display variants exist across hardware revisions:

- **OLED** — 128×48 monochrome, driven over a serial/parallel bus.
- **7-segment** — an array of digits with indicator LEDs.

The emulator will model both behind a common interface and render to a QEMU
`Chardev`/console surface.

## Address map (firmware-confirmed)

See [memory-map.md](memory-map.md) for the consolidated table. The values below
come straight from the firmware linker script:

| Region          | Base          | Size          | Source |
| --------------- | ------------- | ------------- | ------ |
| On-chip SRAM    | `0x20000000`  | `0x00300000`  | **(fw)** linker `RAM012L` |
| External SDRAM  | `0x0C000000`  | `0x04000000`  | **(fw)** linker `SDRAM` (CS3) |
| SPI ROM window  | `0x18000000`  | `0x02000000`  | **(fw)** linker `ROM` |

The Cortex-A9 private peripheral region (GIC distributor/CPU interface, global
and private timers) and the PL310 L2 cache controller sit in the SoC's internal
peripheral space; exact bases are **(TBD)** pending the RZ/A1 hardware manual and
will be filled in as the SoC model is built.

## Boot

`ENTRY(start)` — execution begins at the `start` symbol (in `start.S`). The
reset code sets up MMU translation tables in on-chip RAM, copies select sections
into SDRAM, and jumps into the C/C++ application. For emulation we load the
firmware ELF via `-kernel`; the ELF's entry point and segment addresses drive
initial PC and memory population.

## Sources

- DelugeFirmware repository (linker script, register headers, board init).
- Renesas RZ/A1L User's Manual: Hardware (for peripheral register layouts) —
  to be cross-referenced as peripherals are implemented.
