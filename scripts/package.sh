#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build a self-contained, relocatable bundle of the Deluge emulator suitable for
# attaching to a GitHub Release, so users can download and run it without a
# Homebrew/QEMU toolchain.
#
# The committed source binary links against Homebrew dylibs by absolute path
# (/opt/homebrew/...), so it will not run on a machine that lacks those exact
# packages. This script vendors every non-system dynamic library next to the
# binary and rewrites the load commands to @loader_path/@executable_path, then
# re-signs everything ad-hoc.
#
# Usage:
#   ./scripts/package.sh                 # -> build/release/DelugEmu-<os>-<arch>.tar.gz
#   ./scripts/package.sh <output-dir>
#
# macOS only for now (uses otool / install_name_tool / codesign). A Linux
# variant would use ldd + patchelf + an RPATH of $ORIGIN.

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

[ "$(uname -s)" = "Darwin" ] || die "package.sh currently supports macOS only."

BIN_SRC="${QEMU_BUILD_DIR}/qemu-system-arm"
[ -x "${BIN_SRC}" ] || die "qemu-system-arm not built. Run ./scripts/build.sh first."

for tool in otool install_name_tool codesign; do
    command -v "${tool}" >/dev/null 2>&1 || die "${tool} not found (install Xcode command line tools)."
done

OUT_DIR="${1:-${REPO_ROOT}/build/release}"
ARCH="$(uname -m)"
NAME="DelugEmu-macos-${ARCH}"
STAGE="${OUT_DIR}/${NAME}"
LIBDIR="${STAGE}/lib"
BINDIR="${STAGE}/bin"

log "Staging bundle at ${STAGE}"
rm -rf "${STAGE}"
mkdir -p "${BINDIR}" "${LIBDIR}"

cp "${BIN_SRC}" "${BINDIR}/qemu-system-arm"
chmod u+w "${BINDIR}/qemu-system-arm"

# A Mach-O load path that points outside the system dirs (Homebrew, MacPorts,
# /usr/local) and therefore must be vendored.
is_vendor_lib() {
    case "$1" in
        /usr/lib/*|/System/*) return 1 ;;   # system, always present
        @*) return 1 ;;                      # already relative (@rpath/...)
        /*) return 0 ;;                      # any other absolute path
        *) return 1 ;;
    esac
}

# Recursively copy a Mach-O's vendor dependencies into LIBDIR and rewrite the
# referring file's load commands to resolve them next to it.
#   $1 = path to the Mach-O file whose deps we are resolving
#   $2 = the @-prefix the *referring* file should use to find LIBDIR
#        (@executable_path/../lib for the binary, @loader_path for a vendored lib)
process_macho() {
    local file="$1" prefix="$2" dep base
    # otool -L lists the file's own id on line 1 (for dylibs) then its deps.
    while IFS= read -r dep; do
        is_vendor_lib "${dep}" || continue
        base="$(basename "${dep}")"
        if [ ! -e "${LIBDIR}/${base}" ]; then
            cp "${dep}" "${LIBDIR}/${base}"
            chmod u+w "${LIBDIR}/${base}"
            # A vendored lib finds its own siblings via @loader_path, and its
            # install id becomes @rpath-relative.
            install_name_tool -id "@loader_path/${base}" "${LIBDIR}/${base}"
            # Recurse before rewriting so newly copied libs are themselves fixed.
            process_macho "${LIBDIR}/${base}" "@loader_path"
        fi
        install_name_tool -change "${dep}" "${prefix}/${base}" "${file}"
    done < <(otool -L "${file}" | tail -n +2 | awk '{print $1}')
}

log "Vendoring dynamic libraries..."
process_macho "${BINDIR}/qemu-system-arm" "@executable_path/../lib"

# Give the binary an rpath to its lib dir for any @rpath-style ids that remain.
install_name_tool -add_rpath "@executable_path/../lib" "${BINDIR}/qemu-system-arm" 2>/dev/null || true

log "Re-signing (ad-hoc)..."
# Sign libs first, then the binary, so the binary's signature covers final libs.
find "${LIBDIR}" -type f -name '*.dylib' -print0 | while IFS= read -r -d '' lib; do
    codesign --remove-signature "${lib}" 2>/dev/null || true
    codesign --force --sign - "${lib}"
done
codesign --remove-signature "${BINDIR}/qemu-system-arm" 2>/dev/null || true
codesign --force --sign - "${BINDIR}/qemu-system-arm"

# Helper scripts so the bundle keeps the full run.sh UX (SD/MIDI/audio/display).
log "Copying helper scripts..."
mkdir -p "${STAGE}/scripts"
cp "${REPO_ROOT}/scripts/run.sh" \
   "${REPO_ROOT}/scripts/common.sh" \
   "${REPO_ROOT}/scripts/mksd.sh" \
   "${REPO_ROOT}/scripts/midi_bridge.c" \
   "${STAGE}/scripts/"
cp "${REPO_ROOT}/LICENSE" "${STAGE}/LICENSE" 2>/dev/null || true

# Front-panel skin image. run.sh resolves it as ${REPO_ROOT}/Deluge_Plain.png,
# and inside the bundle the helper scripts live at <stage>/scripts/, so
# REPO_ROOT resolves to <stage>. Place the PNG at the bundle root so the skin
# loads with no extra configuration.
cp "${REPO_ROOT}/Deluge_Plain.png" "${STAGE}/Deluge_Plain.png"

# Top-level launcher: point the run helper at the vendored binary.
cat > "${STAGE}/delugemu" <<'LAUNCH'
#!/usr/bin/env bash
# Self-contained launcher for the packaged Deluge emulator.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export DELUGEMU_QEMU_BIN="${HERE}/bin/qemu-system-arm"
exec "${HERE}/scripts/run.sh" "$@"
LAUNCH
chmod +x "${STAGE}/delugemu"

cat > "${STAGE}/README.txt" <<EOF
DelugEmu — Synthstrom Deluge emulator (relocatable macOS ${ARCH} build)

Run firmware (opens the front-panel window by default):
  ./delugemu path/to/deluge_firmware.elf

Attach an SD card image and hear audio (44.1 kHz stereo on your speakers):
  ./delugemu path/to/deluge_firmware.elf --sd deluge_sd.img

Run without a window (serial + monitor on the terminal):
  ./delugemu path/to/deluge_firmware.elf --display headless

See all options:
  ./delugemu --help

Build an SD card image from a content directory (factory folders SONGS/
SYNTHS/ KITS/ SAMPLES/), sized to a power of two as the SD device requires:
  ./scripts/mksd.sh path/to/content deluge_sd.img

Gatekeeper: this build is ad-hoc signed, not notarized. On first launch macOS
may block it; allow it under System Settings > Privacy & Security, or run:
  xattr -dr com.apple.quarantine "\$(pwd)"

This is an independent, unofficial project, not affiliated with Synthstrom
Audible. Licensed GPL-2.0-or-later (see LICENSE).
EOF

# Verify nothing still points at Homebrew/MacPorts.
log "Verifying the bundle is self-contained..."
leftover="$(otool -L "${BINDIR}/qemu-system-arm" "${LIBDIR}"/*.dylib 2>/dev/null \
    | awk '{print $1}' | grep -E '^/(opt|usr/local)/' || true)"
if [ -n "${leftover}" ]; then
    warn "Unvendored absolute dependencies remain:"
    printf '%s\n' "${leftover}" >&2
    die "Bundle is not self-contained."
fi

# Smoke test: the relocated binary must still register the deluge machine.
if "${BINDIR}/qemu-system-arm" -M help 2>/dev/null | grep -qi deluge; then
    log "Smoke test OK: deluge machine registered."
else
    die "Relocated binary failed to list the deluge machine."
fi

lib_count="$(find "${LIBDIR}" -name '*.dylib' | wc -l | tr -d ' ')"
log "Bundle: 1 binary + ${lib_count} vendored libraries."

TARBALL="${OUT_DIR}/${NAME}.tar.gz"
log "Creating ${TARBALL}"
tar -czf "${TARBALL}" -C "${OUT_DIR}" "${NAME}"

size="$(du -h "${TARBALL}" | cut -f1 | tr -d ' ')"
log "Release archive ready: ${TARBALL} (${size})"
log "Attach it to a GitHub Release; users extract and run ./delugemu."
