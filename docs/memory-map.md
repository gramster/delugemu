# Memory map

Consolidated guest physical address map for the emulated Deluge. Entries marked
**(fw)** are confirmed from the firmware linker script
(`linker_script_rz_a1l.ld`); entries marked **(TBD)** are placeholders to be
confirmed against the Renesas RZ/A1L hardware manual as each device is
implemented.

## RAM and storage windows

| Region              | Base          | Size          | Notes                          | Source |
| ------------------- | ------------- | ------------- | ------------------------------ | ------ |
| External SDRAM      | `0x0C000000`  | `0x04000000`  | 64 MB device on bus CS3        | (fw)   |
| SPI flash (ROM)     | `0x18000000`  | `0x02000000`  | Memory-mapped SPI flash window | (fw)   |
| On-chip SRAM        | `0x20000000`  | `0x00300000`  | 3 MB; MMU tables, stacks, code | (fw)   |

## SoC peripherals (RZ/A1L) — to confirm

The following are the standard Renesas RZ/A1 peripheral groupings. Concrete
register bases will be filled in (and split into per-channel rows) as the SoC
model gains each block. Until then the SoC model installs an "unimplemented
device" catch-all over the peripheral region so firmware accesses are logged
rather than aborting.

| Block                         | Base   | Notes                                | Source |
| ----------------------------- | ------ | ------------------------------------ | ------ |
| Cortex-A9 private peripherals | (TBD)  | GIC distributor/CPU interface, timers| (TBD)  |
| PL310 L2 cache controller     | (TBD)  | Modelled as no-op cache              | (TBD)  |
| Clock pulse generator (CPG)   | (TBD)  | Clock/standby control                | (TBD)  |
| Bus state controller (BSC)    | (TBD)  | CS configuration, SDRAM timing       | (TBD)  |
| Interrupt controller (INTC)   | (TBD)  | Maps peripheral IRQs to the GIC      | (TBD)  |
| SCIF (serial, UART)           | (TBD)  | Debug/console serial                 | (TBD)  |
| SSI (I²S audio)               | (TBD)  | Audio codec link                     | (TBD)  |
| SD host interface             | (TBD)  | SD card                              | (TBD)  |
| GPIO ports                    | (TBD)  | Misc board signals                   | (TBD)  |
| RSPI / SPIBSC                 | (TBD)  | SPI, incl. mapped flash              | (TBD)  |

## Board-level (Deluge) devices

These hang off SoC buses (GPIO/serial/SSI) rather than occupying their own
physical window, so they appear here as connections rather than addresses.

| Device         | Attaches via         | Notes                               |
| -------------- | -------------------- | ----------------------------------- |
| Input PIC      | Serial channel       | Pads, buttons, encoders ↔ LED/PWM   |
| OLED display   | Serial/parallel bus  | 128×48 monochrome                   |
| 7-seg display  | GPIO/serial          | Digit + indicator LEDs              |
| Audio codec    | SSI                  | Stereo in/out                       |

## Conventions

- All addresses are **guest physical**.
- Sizes are byte counts (`0x00300000` = 3 MiB).
- When adding a peripheral, add a row here and a matching `#define` in the
  device's header (`src/include/hw/...`), keeping the two in sync.
