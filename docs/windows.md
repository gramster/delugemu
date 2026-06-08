<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Building and running delugemu on Windows

The delugemu build scripts are Bash scripts that drive QEMU's own Meson/Ninja
build. On Windows they run inside **MSYS2 / MinGW**, which is the toolchain the
QEMU project itself uses to build the official Windows binaries. WSL is also an
option, but these instructions cover the native MSYS2 path so you get a real
Windows `qemu-system-arm.exe` with a native GUI window.

> Outgoing audio uses the `dsound` backend by default on Windows. The
> `coremidi` host-MIDI bridge (`--midi coremidi` / `--usb-midi coremidi`) is
> macOS-only — see [MIDI on Windows](#midi-on-windows) for what works instead.

## Prebuilt bundle (no MSYS2 needed)

Tagged releases publish a relocatable `DelugEmu-windows-*.zip` containing
`qemu-system-arm.exe`, its DLLs, and a native PowerShell launcher
(`delugemu.ps1`) wrapped by `delugemu.cmd`. Download it from the
[Releases page](https://github.com/gramster/delugemu/releases), unzip, and run:

```bat
delugemu.cmd                          :: auto-detect/offer to download firmware
delugemu.cmd path\to\firmware.bin     :: boot a specific firmware image
delugemu.cmd --help                   :: full option list
```

The launcher mirrors `run.sh`: optional firmware with auto-download of the
community release, raw SD images **and** SD folders (`--sd <dir>`, snapshotted
into a FAT image, with write-back for `_rw` folders), MIDI/USB-MIDI chardevs,
audio backend selection, and display modes. SD-folder support uses the bundled
`mkfs.fat` and `mcopy`; raw `.img` SD cards work even without them. Build the
bundle yourself with `./scripts/package.sh` (steps below).

The rest of this document covers building from source under MSYS2.

## 1. Install MSYS2

Download and run the installer from <https://www.msys2.org/> and follow its
first-run update steps. Then open the **"MSYS2 MINGW64"** shell (not the plain
"MSYS2 MSYS" shell — the MinGW64 environment is the one that produces native
Windows binaries).

## 2. Install the toolchain and dependencies

In the MINGW64 shell:

```sh
pacman -Syu        # update; may ask you to close and reopen the shell, then:
pacman -Su

# Build toolchain + QEMU's build dependencies
pacman -S --needed \
    base-devel git python ninja \
    mingw-w64-x86_64-toolchain \
    mingw-w64-x86_64-meson \
    mingw-w64-x86_64-pkgconf \
    mingw-w64-x86_64-glib2 \
    mingw-w64-x86_64-pixman \
    mingw-w64-x86_64-gtk3 \
    mingw-w64-x86_64-SDL2 \
    mingw-w64-x86_64-libpng \
    mingw-w64-x86_64-zstd

# Optional: needed only for folder-backed SD cards (--sd <dir>) and write-back
pacman -S --needed dosfstools mingw-w64-x86_64-mtools
```

`gtk3` / `SDL2` provide the front-panel skin window, `glib2` and `pixman` are
QEMU core dependencies, and `libpng` lets the skin device load
`Deluge_Plain.png`.

## 3. (Recommended) Enable real symlinks

`scripts/integrate.sh` links the device models into the QEMU source tree. If
symbolic links are available it uses them, so edits under `src/` are picked up
on the next `./scripts/build.sh`. If they are not available it falls back to
**copying** the sources — the build still works, but you must re-run
`./scripts/integrate.sh` after editing anything in `src/`.

To get live symlinks:

1. Enable **Developer Mode** in Windows (Settings → Privacy & security → For
   developers), or run the MSYS2 shell as Administrator.
2. Tell MSYS2 to create native symlinks by adding this to `~/.bashrc` (or
   exporting it for the session):

   ```sh
   export MSYS=winsymlinks:nativestrict
   ```

This is optional. If you only want to build and run, you can skip it.

## 4. Build

From the repository root in the MINGW64 shell:

```sh
./scripts/bootstrap.sh     # one-time: fetch the QEMU submodule (large)
./scripts/integrate.sh     # link/copy src/ into qemu/ and configure
./scripts/build.sh         # build qemu-system-arm.exe
```

The resulting binary is `qemu/build/qemu-system-arm.exe`.

## 5. Run

```sh
# Front-panel skin window (default display)
./scripts/run.sh path/to/deluge_firmware.elf --sd build/deluge_sd.img

# No window (serial + monitor on the terminal)
./scripts/run.sh path/to/deluge_firmware.elf --display headless
```

Audio plays through `dsound` by default. Use `--audio <driver>` (e.g.
`--audio sdl`, `--audio wav`, `--audio none`) only to override it.

## Paths

The scripts accept normal MSYS2/Unix-style paths (e.g.
`/c/Users/you/firmware/deluge.elf`). Windows drive paths like
`C:\Users\...` also work in most cases, but forward-slash MSYS paths are the
least surprising.

## MIDI on Windows

The `coremidi` shortcut (`--midi coremidi` and `--usb-midi coremidi`) creates a
virtual MIDI port your DAW can see, but it is implemented with Apple's CoreMIDI
framework (`scripts/midi_bridge.c`) and is **macOS-only**. On Windows `run.sh`
rejects it:

```sh
# This FAILS on Windows:
./scripts/run.sh fw.elf --midi coremidi
#   [delugemu] 'coremidi' MIDI bridging is currently macOS-only
```

There is no built-in equivalent that publishes a Windows MME/WinMM virtual port,
so the Deluge's MIDI does not automatically appear in your DAW the way it does on
macOS. The helper scripts `scripts/midi_route.py` and `scripts/midi_bridge.c`
are also macOS-specific and are not used on Windows.

What **does** work is QEMU's generic chardev MIDI routing — `--midi` and
`--usb-midi` accept any QEMU chardev spec, which carries the raw MIDI byte
stream over a socket or pipe:

```sh
# DIN MIDI over UDP (send/receive raw MIDI bytes from another process):
./scripts/run.sh fw.elf --midi udp:127.0.0.1:1999

# USB-MIDI over UDP:
./scripts/run.sh fw.elf --usb-midi udp:127.0.0.1:1998
```

To connect that byte stream to a real or virtual Windows MIDI port you need an
external bridge — for example a small Python script using
[python-rtmidi](https://pypi.org/project/python-rtmidi/) that forwards between
the UDP socket and a Windows MIDI device, together with a virtual-port driver
such as [loopMIDI](https://www.tobias-erichsen.de/software/loopmidi.html). A
turnkey Windows MIDI bridge is not yet provided; contributions welcome.

## Troubleshooting

- **`configure` cannot find a dependency** — re-check the `pacman -S` list
  above; the `mingw-w64-x86_64-*` packages must be installed from the
  **MINGW64** shell, not the MSYS shell.
- **No GUI window appears** — ensure `mingw-w64-x86_64-gtk3` (and/or `SDL2`) is
  installed; without a display backend QEMU has no window to open.
- **`mcopy` / `mkfs.fat` not found** when using `--sd <folder>` — install
  `mingw-w64-x86_64-mtools` and `dosfstools` (see step 2). A raw `.img` passed
  to `--sd` does not need these.
- **Edits to `src/` are ignored after a rebuild** — symlinks fell back to
  copies; either re-run `./scripts/integrate.sh`, or enable real symlinks
  (step 3).
