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

## SoC peripherals (RZ/A1L) — modelled

These are the device models the SoC currently installs over the peripheral
windows (each mapped as an overlay above the "unimplemented device" catch-all,
which still logs any unmodelled access). Per-register coverage — and which
registers are real vs. shadow/stub — is documented in
[register-coverage.md](register-coverage.md).

| Block                         | Base         | Size     | Model | Notes |
| ----------------------------- | ------------ | -------- | ----- | ----- |
| Interrupt controller (INTC)   | `0xE8201000` (dist), `0xE8202000` (cpu) | — | `arm_gic` (rev 1) | RZ/A1 INTC layout matches GICv1 |
| PL310 L2 cache controller     | `0x3FFFF000` | —        | `l2x0` | no-op cache |
| Clock pulse generator (CPG)   | `0xFCFE0010` | `0x1000` | `rza1l-cpg` | clock/standby shadow; CPUSTS=0 |
| Watchdog (WDT)                | `0xFCFE0000` | `0x10`   | `rza1l-wdt` | keyed kicks; never expires |
| Bus state controller (BSC)    | `0x3FFFC000` | `0x3000` | `rza1l-bsc` | CS/SDRAM init shadow |
| SCIF0 (serial, UART)          | `0xE8007000` | `0x100`  | `rza1l-scif` | MIDI UART, chardev-backed |
| SCIF1 (serial, UART)          | `0xE8007800` | `0x100`  | `rza1l-scif` | wired to the input PIC |
| DMAC                          | `0xE8200000` | `0x800`  | `rza1l-dmac` | 16 channels, synchronous |
| OSTM (OS timer)               | `0xFCFEC000` | `0x800`  | `rza1l-ostm` | 2 free-running channels |
| MTU2 (timer)                  | `0xFCFF0000` | `0x400`  | `rza1l-mtu2` | free-running counters |
| GPIO ports                    | `0xFCFE3004` | `0x4F00` | `rza1l-gpio` | shadow + PPR loopback |
| RSPI0 (SPI)                   | `0xE800C800` | `0x100`  | `rza1l-rspi` | OLED + CV/gate DAC link |
| SPIBSC (SPI flash)            | `0x3FEFA000` | `0x100`  | `rza1l-spibsc` | stub; no backing flash |
| SD host interface (SDHI)      | `0xE804E800` | `0x100`  | `rza1l-sdhi` | full SD host controller |
| ADC (S12AD)                   | `0xE8005800` | `0x100`  | `rza1l-adc` | battery-sense stub |
| RTC                           | `0xFCFF1000` | `0x40`   | `rza1l-rtc` | fixed-time stub |
| SSIF0 (I²S audio)             | `0xE820B000` | `0x800`  | `rza1l-ssif` | Codec link; serviced via DMA ch6/7 |

## Board-level (Deluge) devices

These hang off SoC buses (GPIO/serial/SSI) rather than occupying their own
physical window, so they appear here as connections rather than addresses.

| Device         | Attaches via         | Notes                               |
| -------------- | -------------------- | ----------------------------------- |
| Input PIC      | Serial channel       | Pads, buttons, encoders ↔ LED/PWM   |
| OLED display   | Serial/parallel bus  | 128×48 monochrome                   |
| 7-seg display  | GPIO/serial          | Digit + indicator LEDs              |
| Audio codec    | SSI + GPIO enable    | Stereo I²S; enabled by a GPIO pin (no I²C config) |

> **Audio codec, SCUX and RIIC.** The Deluge codec is a hardware-strapped I²S
> DAC/ADC: the firmware drives it purely as an I²S stream over SSIF0 and toggles
> a single GPIO enable/reset line (`CODEC` pin), so there is **no I²C codec
> configuration** at runtime. The on-chip sampling-rate converter (SCUX,
> `0xE8208000`) and all RIIC/I²C controllers (`0xFCFEE000`+) are held in
> module-standby by `CPG.STBCR8`/`STBCR9` and never accessed, so they need no
> device model — the logging catch-all over their windows is sufficient.

## Conventions

- All addresses are **guest physical**.
- Sizes are byte counts (`0x00300000` = 3 MiB).
- When adding a peripheral, add a row here and a matching `#define` in the
  device's header (`src/include/hw/...`), keeping the two in sync.
