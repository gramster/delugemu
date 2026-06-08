#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build a self-contained, relocatable bundle of the Deluge emulator suitable for
# attaching to a GitHub Release, so users can download and run it without a
# Homebrew/QEMU/MSYS2 toolchain.
#
# The freshly built qemu-system-arm links against libraries installed by the
# host package manager (Homebrew dylibs on macOS, distro .so files on Linux,
# MSYS2 DLLs on Windows). This script vendors every non-system dynamic library
# next to the binary and fixes up the load paths so the bundle runs on a clean
# machine of the same OS/arch.
#
# Usage:
#   ./scripts/package.sh                 # -> build/release/DelugEmu-<os>-<arch>.<ext>
#   ./scripts/package.sh <output-dir>
#
# The OS is detected automatically (DELUGEMU_OS from common.sh):
#   macOS   -> otool / install_name_tool / codesign, tar.gz
#   Linux   -> ldd / patchelf ($ORIGIN rpath),       tar.gz
#   Windows -> ldd (MSYS2), DLLs beside the .exe,     zip

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

OUT_DIR="${1:-${REPO_ROOT}/build/release}"
ARCH="$(uname -m)"

# ---------------------------------------------------------------------------
# Shared staging: helper scripts, skin image, launcher and README. macOS and
# Linux share the Bash launcher + full run.sh UX; Windows gets a native
# PowerShell launcher (delugemu.ps1) that mirrors run.sh, plus a .cmd wrapper,
# because a clean Windows box has no Bash.
# ---------------------------------------------------------------------------

# Copy the Bash helper scripts and skin image used by the run.sh-based launcher.
stage_unix_helpers() {
    local stage="$1"
    log "Copying helper scripts..."
    mkdir -p "${stage}/scripts"
    cp "${REPO_ROOT}/scripts/run.sh" \
       "${REPO_ROOT}/scripts/common.sh" \
       "${REPO_ROOT}/scripts/mksd.sh" \
       "${REPO_ROOT}/scripts/midi_bridge.c" \
       "${REPO_ROOT}/scripts/midi_route.py" \
       "${stage}/scripts/"
    cp "${REPO_ROOT}/LICENSE" "${stage}/LICENSE" 2>/dev/null || true
    # run.sh resolves the skin as ${REPO_ROOT}/Deluge_Plain.png; inside the
    # bundle the helpers live at <stage>/scripts/, so REPO_ROOT == <stage>.
    cp "${REPO_ROOT}/Deluge_Plain.png" "${stage}/Deluge_Plain.png"

    # Top-level launcher: point run.sh at the vendored binary.
    cat > "${stage}/delugemu" <<'LAUNCH'
#!/usr/bin/env bash
# Self-contained launcher for the packaged Deluge emulator.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export DELUGEMU_QEMU_BIN="${HERE}/bin/qemu-system-arm"
exec "${HERE}/scripts/run.sh" "$@"
LAUNCH
    chmod +x "${stage}/delugemu"
}

# ---------------------------------------------------------------------------
# macOS
# ---------------------------------------------------------------------------

package_macos() {
    for tool in otool install_name_tool codesign; do
        command -v "${tool}" >/dev/null 2>&1 \
            || die "${tool} not found (install Xcode command line tools)."
    done

    local name="DelugEmu-macos-${ARCH}"
    local stage="${OUT_DIR}/${name}"
    local bindir="${stage}/bin" libdir="${stage}/lib"
    local bin_src="${QEMU_BUILD_DIR}/qemu-system-arm"
    [ -x "${bin_src}" ] || die "qemu-system-arm not built. Run ./scripts/build.sh first."

    log "Staging bundle at ${stage}"
    rm -rf "${stage}"
    mkdir -p "${bindir}" "${libdir}"
    cp "${bin_src}" "${bindir}/qemu-system-arm"
    chmod u+w "${bindir}/qemu-system-arm"

    # A Mach-O load path outside the system dirs (Homebrew, MacPorts,
    # /usr/local) that must be vendored.
    is_vendor_lib_macos() {
        case "$1" in
            /usr/lib/*|/System/*) return 1 ;;   # system, always present
            @*) return 1 ;;                      # already relative (@rpath/...)
            /*) return 0 ;;                      # any other absolute path
            *) return 1 ;;
        esac
    }

    # Recursively copy a Mach-O's vendor dependencies into libdir and rewrite the
    # referring file's load commands to resolve them next to it.
    process_macho() {
        local file="$1" prefix="$2" dep base
        while IFS= read -r dep; do
            is_vendor_lib_macos "${dep}" || continue
            base="$(basename "${dep}")"
            if [ ! -e "${libdir}/${base}" ]; then
                cp "${dep}" "${libdir}/${base}"
                chmod u+w "${libdir}/${base}"
                install_name_tool -id "@loader_path/${base}" "${libdir}/${base}"
                process_macho "${libdir}/${base}" "@loader_path"
            fi
            install_name_tool -change "${dep}" "${prefix}/${base}" "${file}"
        done < <(otool -L "${file}" | tail -n +2 | awk '{print $1}')
    }

    log "Vendoring dynamic libraries..."
    process_macho "${bindir}/qemu-system-arm" "@executable_path/../lib"
    install_name_tool -add_rpath "@executable_path/../lib" \
        "${bindir}/qemu-system-arm" 2>/dev/null || true

    log "Re-signing (ad-hoc)..."
    find "${libdir}" -type f -name '*.dylib' -print0 | while IFS= read -r -d '' lib; do
        codesign --remove-signature "${lib}" 2>/dev/null || true
        codesign --force --sign - "${lib}"
    done
    codesign --remove-signature "${bindir}/qemu-system-arm" 2>/dev/null || true
    codesign --force --sign - "${bindir}/qemu-system-arm"

    stage_unix_helpers "${stage}"
    write_readme_unix "${stage}" "macOS ${ARCH}" macos

    log "Verifying the bundle is self-contained..."
    local leftover
    leftover="$(otool -L "${bindir}/qemu-system-arm" "${libdir}"/*.dylib 2>/dev/null \
        | awk '{print $1}' | grep -E '^/(opt|usr/local)/' || true)"
    if [ -n "${leftover}" ]; then
        warn "Unvendored absolute dependencies remain:"
        printf '%s\n' "${leftover}" >&2
        die "Bundle is not self-contained."
    fi

    smoke_test "${bindir}/qemu-system-arm"
    local n; n="$(find "${libdir}" -name '*.dylib' | wc -l | tr -d ' ')"
    log "Bundle: 1 binary + ${n} vendored libraries."
    archive_targz "${name}"
}

# ---------------------------------------------------------------------------
# Linux
# ---------------------------------------------------------------------------

# Library basenames that must NOT be vendored: the core C/C++ runtime, the
# dynamic loader, and the graphics/driver/display stack, which must come from
# the host so the bundle matches the running kernel/GPU/X11/Wayland. Mirrors the
# well-known linuxdeploy exclude list. Anything else (glib, gtk, pixman, png,
# cairo, pango, harfbuzz, ...) is vendored.
linux_lib_excluded() {
    case "$1" in
        ld-linux*|libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|\
        libresolv.so*|libnsl.so*|libutil.so*|libgcc_s.so*|libstdc++.so*|\
        libGL.so*|libGLX.so*|libGLdispatch.so*|libEGL.so*|libOpenGL.so*|\
        libGLU.so*|libdrm.so*|libgbm.so*|libglapi.so*|\
        libX11.so*|libX11-xcb.so*|libxcb*.so*|libXext.so*|libXrender.so*|\
        libXrandr.so*|libXi.so*|libXfixes.so*|libXcursor.so*|libXinerama.so*|\
        libXdamage.so*|libXcomposite.so*|libXau.so*|libXdmcp.so*|libxshmfence.so*|\
        libwayland*.so*|libxkbcommon*.so*|\
        libselinux.so*|libsystemd.so*|libudev.so*|libcap.so*|\
        libz.so*) return 0 ;;
        *) return 1 ;;
    esac
}

package_linux() {
    command -v ldd >/dev/null 2>&1 || die "ldd not found."
    command -v patchelf >/dev/null 2>&1 \
        || die "patchelf not found (install it: apt-get install patchelf)."

    local name="DelugEmu-linux-${ARCH}"
    local stage="${OUT_DIR}/${name}"
    local bindir="${stage}/bin" libdir="${stage}/lib"
    local bin_src="${QEMU_BUILD_DIR}/qemu-system-arm"
    [ -x "${bin_src}" ] || die "qemu-system-arm not built. Run ./scripts/build.sh first."

    log "Staging bundle at ${stage}"
    rm -rf "${stage}"
    mkdir -p "${bindir}" "${libdir}"
    cp "${bin_src}" "${bindir}/qemu-system-arm"
    chmod u+w "${bindir}/qemu-system-arm"

    # Resolve the full dependency closure with ldd, copying every non-excluded
    # library into libdir. ldd already gives the transitive closure, so no
    # recursion is needed.
    log "Vendoring shared libraries..."
    local line base path
    while IFS= read -r line; do
        # Lines look like:  libfoo.so.1 => /path/to/libfoo.so.1 (0x...)
        case "${line}" in
            *"=>"*) path="${line#*=> }"; path="${path%% (0x*}" ;;
            *) continue ;;
        esac
        [ -n "${path}" ] && [ -e "${path}" ] || continue
        base="$(basename "${path}")"
        linux_lib_excluded "${base}" && continue
        [ -e "${libdir}/${base}" ] && continue
        cp -L "${path}" "${libdir}/${base}"
        chmod u+w "${libdir}/${base}"
    done < <(ldd "${bindir}/qemu-system-arm" 2>/dev/null)

    # Point the binary at its vendored libs first, then fall back to the system.
    patchelf --set-rpath '$ORIGIN/../lib' "${bindir}/qemu-system-arm"
    # Vendored libs find their siblings via $ORIGIN.
    find "${libdir}" -type f -name '*.so*' -exec \
        patchelf --set-rpath '$ORIGIN' {} \; 2>/dev/null || true

    stage_unix_helpers "${stage}"
    write_readme_unix "${stage}" "Linux ${ARCH}" linux

    smoke_test "${bindir}/qemu-system-arm"
    local n; n="$(find "${libdir}" -name '*.so*' | wc -l | tr -d ' ')"
    log "Bundle: 1 binary + ${n} vendored libraries."
    archive_targz "${name}"
}

# ---------------------------------------------------------------------------
# Windows (MSYS2/MinGW)
# ---------------------------------------------------------------------------

package_windows() {
    command -v ldd >/dev/null 2>&1 || die "ldd not found (run inside MSYS2)."
    local mingw="${MINGW_PREFIX:-/mingw64}"

    local name="DelugEmu-windows-${ARCH}"
    local stage="${OUT_DIR}/${name}"
    local bin_src="${QEMU_BUILD_DIR}/qemu-system-arm.exe"
    [ -x "${bin_src}" ] || die "qemu-system-arm.exe not built. Run ./scripts/build.sh first."

    log "Staging bundle at ${stage}"
    rm -rf "${stage}"
    mkdir -p "${stage}"
    cp "${bin_src}" "${stage}/qemu-system-arm.exe"

    # On Windows the loader resolves DLLs from the executable's own directory, so
    # we copy every MSYS2/MinGW DLL dependency next to the .exe. ldd gives the
    # transitive closure; we keep only DLLs under the MinGW prefix (the OS DLLs
    # in C:\Windows\System32 are always present).
    log "Vendoring DLLs from ${mingw}..."
    local line path
    while IFS= read -r line; do
        case "${line}" in
            *"=>"*) path="${line#*=> }"; path="${path%% (0x*}" ;;
            *) continue ;;
        esac
        path="$(printf '%s' "${path}" | sed 's/[[:space:]]*$//')"
        [ -n "${path}" ] || continue
        # Only vendor DLLs that live under the MinGW prefix.
        case "${path}" in
            "${mingw}"/*|/mingw64/*|/ucrt64/*|/clang64/*|/mingw32/*) ;;
            *) continue ;;
        esac
        [ -e "${path}" ] || continue
        cp -n "${path}" "${stage}/" 2>/dev/null || true
    done < <(ldd "${stage}/qemu-system-arm.exe" 2>/dev/null)

    # GTK needs its GdkPixbuf image loaders to render the skin PNG. Copy the
    # loader modules best-effort; without them the front-panel window still
    # opens but may not show the photo background.
    if [ -d "${mingw}/lib/gdk-pixbuf-2.0" ]; then
        log "Bundling GdkPixbuf loaders..."
        mkdir -p "${stage}/lib"
        cp -R "${mingw}/lib/gdk-pixbuf-2.0" "${stage}/lib/" 2>/dev/null || true
    fi

    cp "${REPO_ROOT}/Deluge_Plain.png" "${stage}/Deluge_Plain.png"
    cp "${REPO_ROOT}/LICENSE" "${stage}/LICENSE" 2>/dev/null || true

    # Vendor the SD-folder helpers so the native launcher can build a FAT image
    # from a directory without MSYS2: mkfs.fat (dosfstools, a MinGW .exe) and
    # mcopy (mtools, an MSYS .exe that needs msys-2.0.dll alongside it). Both are
    # best-effort: if absent, the launcher falls back to raw-image SD support.
    stage_windows_tool() {
        local tool="$1" found
        found="$(command -v "${tool}" 2>/dev/null || true)"
        if [ -n "${found}" ] && [ -e "${found}" ]; then
            cp -n "${found}" "${stage}/" 2>/dev/null || true
            log "Vendored ${tool}"
            # Bundle any DLLs the tool itself needs (e.g. msys-2.0.dll).
            local tline tpath
            while IFS= read -r tline; do
                case "${tline}" in
                    *"=>"*) tpath="${tline#*=> }"; tpath="${tpath%% (0x*}" ;;
                    *) continue ;;
                esac
                tpath="$(printf '%s' "${tpath}" | sed 's/[[:space:]]*$//')"
                [ -n "${tpath}" ] || continue
                case "${tpath}" in
                    /usr/bin/*|/mingw64/*|/ucrt64/*|/clang64/*|/mingw32/*|"${mingw}"/*) ;;
                    *) continue ;;
                esac
                [ -e "${tpath}" ] && cp -n "${tpath}" "${stage}/" 2>/dev/null || true
            done < <(ldd "${found}" 2>/dev/null)
        else
            log "WARNING: ${tool} not found; SD *folder* support disabled in the bundle (raw .img still works)."
        fi
    }
    stage_windows_tool mkfs.fat.exe
    stage_windows_tool mcopy.exe

    # Native launcher: delugemu.ps1 delivers the full run.sh UX on Windows
    # (optional firmware + auto-download, SD images and folders with write-back,
    # MIDI/USB-MIDI chardevs, audio backends, display modes). delugemu.cmd is a
    # thin wrapper so users can double-click or drag a firmware .bin onto it.
    cp "${REPO_ROOT}/scripts/delugemu.ps1" "${stage}/delugemu.ps1"
    cat > "${stage}/delugemu.cmd" <<'LAUNCH'
@echo off
rem Thin wrapper that runs the native PowerShell launcher next to this file.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0delugemu.ps1" %*
LAUNCH

    write_readme_windows "${stage}"

    smoke_test "${stage}/qemu-system-arm.exe"
    local n; n="$(find "${stage}" -maxdepth 1 -name '*.dll' | wc -l | tr -d ' ')"
    log "Bundle: 1 binary + ${n} vendored DLLs."
    archive_zip "${name}"
}

# ---------------------------------------------------------------------------
# Shared helpers
# ---------------------------------------------------------------------------

# Confirm the relocated binary still registers the deluge machine.
smoke_test() {
    local bin="$1"
    if "${bin}" -M help 2>/dev/null | grep -qi deluge; then
        log "Smoke test OK: deluge machine registered."
    else
        die "Relocated binary failed to list the deluge machine."
    fi
}

archive_targz() {
    local name="$1" tarball="${OUT_DIR}/$1.tar.gz" size
    log "Creating ${tarball}"
    tar -czf "${tarball}" -C "${OUT_DIR}" "${name}"
    size="$(du -h "${tarball}" | cut -f1 | tr -d ' ')"
    log "Release archive ready: ${tarball} (${size})"
    log "Attach it to a GitHub Release; users extract and run ./delugemu."
}

archive_zip() {
    local name="$1" zip="${OUT_DIR}/$1.zip" size
    log "Creating ${zip}"
    rm -f "${zip}"
    if command -v zip >/dev/null 2>&1; then
        ( cd "${OUT_DIR}" && zip -qr "$1.zip" "${name}" )
    elif command -v powershell >/dev/null 2>&1 || command -v powershell.exe >/dev/null 2>&1; then
        powershell.exe -NoProfile -Command \
            "Compress-Archive -Path '${OUT_DIR}/${name}' -DestinationPath '${zip}' -Force" \
            || die "Compress-Archive failed"
    else
        die "no zip or PowerShell available to create the archive"
    fi
    size="$(du -h "${zip}" 2>/dev/null | cut -f1 | tr -d ' ')"
    log "Release archive ready: ${zip} (${size})"
    log "Attach it to a GitHub Release; users extract and run delugemu.cmd."
}

write_readme_unix() {
    local stage="$1" label="$2" os="$3" gatekeeper=""
    if [ "${os}" = "macos" ]; then
        gatekeeper="
Gatekeeper: this build is ad-hoc signed, not notarized. On first launch macOS
may block it; allow it under System Settings > Privacy & Security, or run:
  xattr -dr com.apple.quarantine \"\$(pwd)\"
"
    fi
    cat > "${stage}/README.txt" <<EOF
DelugEmu — Synthstrom Deluge emulator (relocatable ${label} build)

Run firmware (opens the front-panel window by default):
  ./delugemu path/to/deluge_firmware.elf

No firmware image? Run with no arguments and DelugEmu will look for a .bin/.elf
in the firmware/ folder, and if none is found, offer to download the open-source
Deluge community firmware release from Synthstrom and use it from then on:
  ./delugemu

Attach an SD card image and hear audio (44.1 kHz stereo on your speakers):
  ./delugemu path/to/deluge_firmware.elf --sd deluge_sd.img

--sd also accepts a folder, snapshotted into a card image for you. Name the
folder ending in '_rw' to have the guest's changes written back to it on exit:
  ./delugemu path/to/deluge_firmware.elf --sd path/to/card_folder_rw

Run without a window (serial + monitor on the terminal):
  ./delugemu path/to/deluge_firmware.elf --display headless

See all options:
  ./delugemu --help

Build an SD card image from a content directory (factory folders SONGS/
SYNTHS/ KITS/ SAMPLES/), sized to a power of two as the SD device requires:
  ./scripts/mksd.sh path/to/content deluge_sd.img
${gatekeeper}
This is an independent, unofficial project, not affiliated with Synthstrom
Audible. Licensed GPL-2.0-or-later (see LICENSE).
EOF
}

write_readme_windows() {
    local stage="$1"
    cat > "${stage}/README.txt" <<'EOF'
DelugEmu - Synthstrom Deluge emulator (relocatable Windows build)

Quick start (opens the front-panel window):
  delugemu.cmd path\to\deluge_firmware.bin

The firmware image is optional. If you run delugemu.cmd with no firmware, it
uses a .bin/.elf from the bundled "firmware" folder, or offers to download the
Deluge community firmware release and use that. You can also drag a firmware
.bin onto delugemu.cmd in Explorer.

This bundle includes a native PowerShell launcher (delugemu.ps1) that gives the
full experience without MSYS2:
  delugemu.cmd [firmware.bin] [options]
    --sd <img|folder>     attach a raw FAT image, or snapshot a folder into one
                          (folders ending in _rw are written back on exit). With
                          no --sd, an sdcard_rw/sdcard folder is used if present,
                          else you are offered the Synthstrom factory card.
    --midi <chardev>      route DIN MIDI, e.g. --midi udp:127.0.0.1:1999
    --usb-midi <chardev>  attach a host USB-MIDI device on a chardev
    --audio <driver>      pick an audio backend (default sdl)
    --display <mode>      console (default) | headless | none
  delugemu.cmd --help     full option list

SD *folder* snapshotting uses the bundled mkfs.fat and mcopy tools. If they are
missing, attach a raw .img with --sd instead. 'coremidi' MIDI is macOS-only;
on Windows use a chardev spec (with loopMIDI + a UDP/RTP bridge) - see
docs/windows.md:
  https://github.com/gramster/delugemu/blob/main/docs/windows.md

SmartScreen: this build is unsigned, so Windows may warn on first launch.
Choose "More info" then "Run anyway" if you trust this download.

This is an independent, unofficial project, not affiliated with Synthstrom
Audible. Licensed GPL-2.0-or-later (see LICENSE).
EOF
}

# ---------------------------------------------------------------------------
# Dispatch on the host OS.
# ---------------------------------------------------------------------------

case "${DELUGEMU_OS}" in
    macos)   package_macos ;;
    linux)   package_linux ;;
    windows) package_windows ;;
    *)       die "package.sh does not support this OS (DELUGEMU_OS=${DELUGEMU_OS})." ;;
esac
