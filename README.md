# delugemu

A hardware emulator for the [Synthstrom Audible Deluge](https://synthstrom.com/product/deluge/),
built as a custom machine model on top of [QEMU](https://www.qemu.org/).

The goal is to boot unmodified Deluge firmware (the open-source
[DelugeFirmware](https://github.com/SynthstromAudible/DelugeFirmware)) in a
fully software-simulated environment so that development, debugging, automated
testing and CI can happen without physical hardware.

> Status: **early scaffolding**. The repository currently provides the project
> structure, build glue and device-model stubs. None of the peripherals are
> functionally complete yet. See [docs/roadmap.md](docs/roadmap.md).

## Target hardware

The Deluge is built around a **Renesas RZ/A1L** SoC:

| Component        | Detail                                                        |
| ---------------- | ------------------------------------------------------------- |
| CPU              | Arm Cortex-A9, single core, 400 MHz                           |
| On-chip SRAM     | 3 MB (mapped at `0x2000_0000`)                                 |
| External SDRAM   | 64 MB                                                          |
| Display          | OLED (128×48) **or** 7-segment array, depending on revision   |
| Input            | RGB pad matrix + encoders, handled by an auxiliary PIC MCU    |
| Audio            | I²S/SSI codec                                                 |
| Storage          | SD card                                                        |

Firmware runs bare-metal (no OS), in C/C++ with some Arm assembly.

See [docs/hardware.md](docs/hardware.md) for the full hardware breakdown and
[docs/memory-map.md](docs/memory-map.md) for the SoC memory map.

## How it works

QEMU does not ship with an RZ/A1L SoC or a Deluge board model. This project
keeps **vanilla upstream QEMU as a git submodule** and maintains the custom
hardware models under [`src/`](src). A small integration step links our sources
into the QEMU source tree and registers them with QEMU's Meson/Kconfig build, so
upstream stays untouched and easy to rebase.

```
firmware (.elf/.bin)
        │
        ▼
  ┌───────────────────────────────────────────┐
  │ qemu-system-arm -M deluge                  │
  │   ┌─────────────────────────────────────┐ │
  │   │ RZ/A1L SoC model (src/hw/arm)        │ │
  │   │  • Cortex-A9 + GIC + timers          │ │
  │   │  • 3 MB SRAM, 64 MB SDRAM            │ │
  │   │  • SCIF, SSI, SD, INTC               │ │
  │   └─────────────────────────────────────┘ │
  │   ┌─────────────────────────────────────┐ │
  │   │ Deluge board peripherals             │ │
  │   │  • PIC (buttons/pads/encoders)       │ │
  │   │  • OLED / 7-segment display          │ │
  │   │  • audio codec                       │ │
  │   └─────────────────────────────────────┘ │
  └───────────────────────────────────────────┘
```

See [docs/architecture.md](docs/architecture.md) for details.

## Quick start

```sh
# 1. Fetch the QEMU submodule (large, one-time)
./scripts/bootstrap.sh

# 2. Link our device models into the QEMU tree and configure the build
./scripts/integrate.sh

# 3. Build qemu-system-arm with the Deluge machine
./scripts/build.sh

# 4. Run firmware
./scripts/run.sh path/to/deluge_firmware.elf

# Options: attach an SD image, route MIDI to a chardev, add an audio
# backend, or pick a display mode (headless/console/none). See --help.
./scripts/run.sh path/to/deluge_firmware.elf --sd build/deluge_sd.img --display console

# Hear the Deluge on your speakers: route the SSIF (I2S) output to a host
# audio backend. 'auto' picks coreaudio on macOS, pa on Linux, dsound on
# Windows; or name one explicitly (coreaudio / pa / sdl / wav). Play a note in
# an instrument clip to get 44.1 kHz stereo output.
./scripts/run.sh path/to/deluge_firmware.elf --sd build/deluge_sd.img --display console --audio auto
```

## Repository layout

```
delugemu/
├── docs/                Hardware notes, architecture, roadmap
├── scripts/             bootstrap / integrate / build / run helpers
├── src/
│   ├── hw/              QEMU device & machine models (C)
│   │   ├── arm/         deluge board + RZ/A1L SoC container
│   │   ├── display/     OLED / 7-segment models
│   │   └── misc/        PIC and other board glue
│   └── include/hw/      Public headers for the models above
├── tests/               Emulation / regression tests
└── qemu/                Upstream QEMU (git submodule)
```

## License

GPL-2.0-or-later, matching QEMU so the device models can link against it. See
[LICENSE](LICENSE).

The Synthstrom Deluge name and hardware are property of Synthstrom Audible Ltd.
This is an independent, unofficial project and is not affiliated with or
endorsed by Synthstrom Audible.
