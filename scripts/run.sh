#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Run Deluge firmware under the emulator.
#
# Usage:
#   ./scripts/run.sh [firmware.elf|firmware.bin] [options] [-- qemu args...]
#
# The firmware image is optional. If omitted, run.sh uses a .bin (or .elf) image
# found in the firmware folder (default: <repo>/firmware; override with the
# DELUGEMU_FIRMWARE_DIR environment variable). If that folder has no image and
# the terminal is interactive, run.sh offers to download the Deluge community
# firmware release from Synthstrom, unzips it into the firmware folder, and uses
# it from then on.
#
# Options:
#   --sd <path>             Attach an SD card to the SDHI slot. <path> may be:
#                             * a raw FAT disk image file (attached directly), or
#                             * a directory, which is snapshotted into a
#                               temporary FAT image at launch. If the directory
#                               name ends in '_rw', changes the guest makes to
#                               the card are written back to the directory when
#                               the emulator exits; otherwise the directory is
#                               left untouched (read-only snapshot).
#                           If omitted, run.sh auto-detects an 'sdcard_rw' or
#                           'sdcard' directory in the current working directory
#                           and uses it (the '_rw' variant takes precedence). If
#                           neither exists and the terminal is interactive,
#                           run.sh offers to download the Synthstrom factory card
#                           contents into ./sdcard and uses that.
#   --midi <chardev>        Back SCIF0 (the DIN MIDI UART) with a QEMU chardev
#                           spec, e.g. --midi udp:127.0.0.1:1999 or --midi pty.
#                           Pass the special value 'coremidi' to expose the
#                           Deluge's DIN MIDI as a real host MIDI in/out port
#                           named "DelugEmu DIN" (macOS). When omitted, SCIF0 is
#                           multiplexed with the monitor on stdio.
#   --usb-midi <chardev>    Attach a host USB-MIDI device backed by a QEMU
#                           chardev spec, e.g. --usb-midi udp:127.0.0.1:1998 or
#                           --usb-midi pty. Pass 'coremidi' to expose it as a
#                           real host MIDI in/out port named "DelugEmu USB"
#                           (macOS). Routed to serial slot 1.
#   --audio <driver>        Select a non-default host audio backend for the
#                           SSIF (I2S) output (-audiodev <driver>). Audio is on
#                           by default using the OS default backend (coreaudio
#                           on macOS, pa on Linux) or sdl on Windows (where the
#                           dsound default is unreliable), so this flag is only
#                           needed to override it, e.g. --audio dsound / wav /
#                           none. Use 'auto' to force the recommended backend
#                           explicitly. Play a note in an instrument clip to
#                           hear 44.1 kHz stereo output.
#   --audio-buffer <ms>     Output buffer cushion in milliseconds (default 15).
#                           Raise it if you hear dropouts; lower it to trim the
#                           delay when playing the emulated Deluge live from
#                           external MIDI.
#   --display <mode>        Display mode:
#                             console   open the front-panel skin window with
#                                       the modelled OLED / pad-grid / 7-seg
#                                       overlays (default)
#                             headless  no GUI; serial+monitor on stdio
#                             none      graphics subsystem present but not shown
#   -h, --help              Show this help and exit.
#
# Anything after a literal `--`, or any unrecognised flag, is passed straight
# through to qemu-system-arm. Useful extras:
#   -d guest_errors,unimp   log unimplemented device accesses
#   -S -s                   freeze at reset and wait for gdb on :1234

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

usage() {
    # Print the banner comment (from the "Run Deluge..." line through the end of
    # the leading comment block), stripping the leading "# ".
    awk 'NR>=4 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "${BASH_SOURCE[0]}"
}

# Resolve a --sd argument that may be either a raw image file or a directory.
# A directory is snapshotted into a temporary FAT image (via mksd.sh) so the
# guest sees a normal card. If its name ends in '_rw', guest changes are copied
# back to the directory on exit (see sd_writeback and the cleanup trap).
sd_setup() {
    local path="$1"
    if [ -d "${path}" ]; then
        SD_FOLDER="${path%/}"
        SD_TMP_IMG="$(mktemp "${TMPDIR:-/tmp}/delugemu-sd.XXXXXX")"
        log "Snapshotting folder into a temporary SD image: ${SD_FOLDER}"
        "${REPO_ROOT}/scripts/mksd.sh" "${SD_FOLDER}" "${SD_TMP_IMG}" \
            || die "failed to build SD image from ${SD_FOLDER}"
        case "${SD_FOLDER}" in
            *_rw)
                SD_WRITEBACK=1
                log "Folder name ends in '_rw': changes will be written back on exit"
                ;;
        esac
        SD_ARGS=(-drive "if=sd,format=raw,file=${SD_TMP_IMG}")
    elif [ -f "${path}" ]; then
        SD_ARGS=(-drive "if=sd,format=raw,file=${path}")
    else
        die "SD image/directory not found: ${path}"
    fi
}

# Mirror the (possibly guest-modified) temporary SD image back into the source
# directory. Only called for '_rw' folders. Best-effort: warns on failure
# rather than aborting, since the emulator has already run.
sd_writeback() {
    [ -n "${SD_TMP_IMG}" ] || return 0
    [ -d "${SD_FOLDER}" ] || return 0
    log "Writing SD changes back to ${SD_FOLDER}"
    case "$(uname -s)" in
        Darwin)
            local dev mp
            dev="$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage \
                    "${SD_TMP_IMG}" 2>/dev/null | head -1 | awk '{print $1}')"
            if [ -z "${dev}" ]; then
                warn "could not attach SD image for write-back; ${SD_FOLDER} left unchanged"
                return 0
            fi
            if ! diskutil mount "${dev}" >/dev/null 2>&1; then
                warn "could not mount SD image for write-back; ${SD_FOLDER} left unchanged"
                hdiutil detach "${dev}" >/dev/null 2>&1 || true
                return 0
            fi
            mp="$(diskutil info "${dev}" | awk -F': *' '/Mount Point/ {print $2}')"
            if [ -n "${mp}" ]; then
                rsync -a --delete \
                    --exclude '.fseventsd' --exclude '.Spotlight-V100' \
                    --exclude '.Trashes' --exclude '.TemporaryItems' \
                    "${mp}/" "${SD_FOLDER}/" \
                    || warn "write-back to ${SD_FOLDER} reported errors"
            else
                warn "could not locate mount point for write-back"
            fi
            diskutil unmount "${dev}" >/dev/null 2>&1 || true
            hdiutil detach "${dev}" >/dev/null 2>&1 || true
            ;;
        *)
            if ! command -v mcopy >/dev/null 2>&1; then
                warn "mtools (mcopy) not found; cannot write SD changes back to ${SD_FOLDER}"
                return 0
            fi
            local tmpdir
            tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/delugemu-sdout.XXXXXX")"
            if mcopy -i "${SD_TMP_IMG}" -s -n -m "::/*" "${tmpdir}/" 2>/dev/null; then
                if command -v rsync >/dev/null 2>&1; then
                    rsync -a --delete "${tmpdir}/" "${SD_FOLDER}/" \
                        || warn "write-back to ${SD_FOLDER} reported errors"
                else
                    cp -a "${tmpdir}/." "${SD_FOLDER}/" \
                        || warn "write-back to ${SD_FOLDER} reported errors"
                fi
            else
                warn "could not read SD image for write-back; ${SD_FOLDER} left unchanged"
            fi
            rm -rf "${tmpdir}"
            ;;
    esac
    return 0
}

# Where auto-detected and downloaded firmware images live. Overridable so a user
# can point at an existing collection without copying it in.
FIRMWARE_DIR="${DELUGEMU_FIRMWARE_DIR:-${REPO_ROOT}/firmware}"

# The community firmware release offered when no image is present. This is the
# unmodified open-source build published by Synthstrom; we only download and
# unzip it, we do not redistribute it.
COMMUNITY_FW_URL="https://github.com/SynthstromAudible/DelugeFirmware/releases/download/release_1_2_1/deluge-community-release-1_2_1.zip"
COMMUNITY_FW_NAME="Deluge community firmware 1.2.1 (Chopin)"

# The Synthstrom factory SD-card contents offered when no card is given and no
# default sdcard folder exists. Downloaded and unzipped into SD_DIR, not
# redistributed. SD_DIR (overridable) is also the folder auto-detected below.
FACTORY_SD_URL="https://synthstrom-audible-deluge.s3.us-east-2.amazonaws.com/Deluge+OLED+V4p1p0+factory+card+contents.zip"
FACTORY_SD_NAME="Deluge OLED V4.1.0 factory card contents"
SD_DIR="${DELUGEMU_SD_DIR:-sdcard}"

# Echo the path of a usable firmware image already present in FIRMWARE_DIR (or
# nothing). Prefer an OLED .bin (the emulator's OLED can also render the 7-seg
# UI), then any .bin, then an .elf, so an existing firmware/deluge.elf dev setup
# keeps working too. `|| true` guards against SIGPIPE under `set -o pipefail`.
find_local_firmware() {
    [ -d "${FIRMWARE_DIR}" ] || return 0
    local f
    f="$(find "${FIRMWARE_DIR}" -type f -iname '*.bin' ! -iname '*7seg*' \
            2>/dev/null | LC_ALL=C sort | head -n1 || true)"
    [ -n "${f}" ] || f="$(find "${FIRMWARE_DIR}" -type f -iname '*.bin' \
            2>/dev/null | LC_ALL=C sort | head -n1 || true)"
    [ -n "${f}" ] || f="$(find "${FIRMWARE_DIR}" -type f -iname '*.elf' \
            2>/dev/null | LC_ALL=C sort | head -n1 || true)"
    [ -n "${f}" ] && printf '%s\n' "${f}"
    return 0
}

# Download a zip archive and unzip it into a directory. Uses curl or wget to
# fetch and unzip or python's zipfile module to extract, so it works on macOS,
# Linux and Windows/MSYS2 without extra tooling.
download_and_unzip() {
    local url="$1" dir="$2" name="$3" tmp zip
    mkdir -p "${dir}"
    tmp="$(mktemp -d "${TMPDIR:-/tmp}/delugemu-dl.XXXXXX")" || die "mktemp failed"
    zip="${tmp}/archive.zip"

    log "Downloading ${name}..."
    if command -v curl >/dev/null 2>&1; then
        curl -fL --progress-bar -o "${zip}" "${url}" \
            || { rm -rf "${tmp}"; die "download failed (curl): ${url}"; }
    elif command -v wget >/dev/null 2>&1; then
        wget -q --show-progress -O "${zip}" "${url}" \
            || { rm -rf "${tmp}"; die "download failed (wget): ${url}"; }
    else
        rm -rf "${tmp}"
        die "neither curl nor wget found; cannot download ${name}"
    fi

    log "Extracting into ${dir}"
    if command -v unzip >/dev/null 2>&1; then
        unzip -o -q "${zip}" -d "${dir}" \
            || { rm -rf "${tmp}"; die "failed to unzip archive"; }
    elif command -v python3 >/dev/null 2>&1; then
        python3 -m zipfile -e "${zip}" "${dir}" \
            || { rm -rf "${tmp}"; die "failed to extract archive (python3)"; }
    elif command -v python >/dev/null 2>&1; then
        python -m zipfile -e "${zip}" "${dir}" \
            || { rm -rf "${tmp}"; die "failed to extract archive (python)"; }
    else
        rm -rf "${tmp}"
        die "no unzip or python found to extract the archive"
    fi
    rm -rf "${tmp}"
}

BIN="${QEMU_SYSTEM_BIN}"
[ -x "${BIN}" ] || die "qemu-system-arm not built. Run ./scripts/build.sh first."

# Firmware is optional. Treat the first argument as a firmware path only when it
# is present and not an option (an option, or nothing, means "auto-detect").
FIRMWARE=""
case "${1:-}" in
    -h|--help) usage; exit 0 ;;
    ""|-*) ;;
    *) FIRMWARE="$1"; shift ;;
esac

# Resolve the firmware image when none was given on the command line: use one
# from the firmware folder, or offer to download the community release.
if [ -z "${FIRMWARE}" ]; then
    FIRMWARE="$(find_local_firmware)"
    if [ -n "${FIRMWARE}" ]; then
        log "No firmware given; using ${FIRMWARE#"${REPO_ROOT}/"}"
    elif [ -t 0 ]; then
        printf '%s\n' "No firmware specified and none found in ${FIRMWARE_DIR}." >&2
        printf '%s' "Download ${COMMUNITY_FW_NAME} (~900 KB) from Synthstrom and use it? [y/N] " >&2
        read -r reply || reply=""
        case "${reply}" in
            [yY]|[yY][eE][sS])
                download_and_unzip "${COMMUNITY_FW_URL}" "${FIRMWARE_DIR}" "${COMMUNITY_FW_NAME}"
                FIRMWARE="$(find_local_firmware)"
                [ -n "${FIRMWARE}" ] \
                    || die "firmware download/extract produced no .bin image"
                log "Using ${FIRMWARE#"${REPO_ROOT}/"}"
                ;;
            *)
                usage
                die "no firmware. Pass a firmware path or place a .bin in ${FIRMWARE_DIR}."
                ;;
        esac
    else
        usage
        die "no firmware given and none found in ${FIRMWARE_DIR} (run interactively to fetch the community firmware, or pass a firmware path)"
    fi
fi

[ -f "${FIRMWARE}" ] || die "Firmware not found: ${FIRMWARE}"

MIDI=""
USB_MIDI=""
AUDIO=""
AUDIO_BUFFER=""
DISPLAY_MODE="console"

SD_ARGS=()
SD_FOLDER=""
SD_TMP_IMG=""
SD_WRITEBACK=0
EXTRA_ARGS=()

while [ $# -gt 0 ]; do
    case "$1" in
        --)
            shift
            EXTRA_ARGS+=("$@")
            break
            ;;
        --sd)
            [ -n "${2:-}" ] || die "--sd requires an image path or directory"
            sd_setup "$2"
            shift 2
            ;;
        --sd=*)
            sd_setup "${1#--sd=}"
            shift
            ;;
        --midi)
            [ -n "${2:-}" ] || die "--midi requires a chardev spec"
            MIDI="$2"; shift 2
            ;;
        --midi=*) MIDI="${1#--midi=}"; shift ;;
        --usb-midi)
            [ -n "${2:-}" ] || die "--usb-midi requires a chardev spec"
            USB_MIDI="$2"; shift 2
            ;;
        --usb-midi=*) USB_MIDI="${1#--usb-midi=}"; shift ;;
        --audio)
            [ -n "${2:-}" ] || die "--audio requires a driver name"
            AUDIO="$2"; shift 2
            ;;
        --audio=*) AUDIO="${1#--audio=}"; shift ;;
        --audio-buffer)
            [ -n "${2:-}" ] || die "--audio-buffer requires a value in ms"
            AUDIO_BUFFER="$2"; shift 2
            ;;
        --audio-buffer=*) AUDIO_BUFFER="${1#--audio-buffer=}"; shift ;;
        --display)
            [ -n "${2:-}" ] || die "--display requires a mode"
            DISPLAY_MODE="$2"; shift 2
            ;;
        --display=*) DISPLAY_MODE="${1#--display=}"; shift ;;
        -h|--help) usage; exit 0 ;;
        *)
            EXTRA_ARGS+=("$1")
            shift
            ;;
    esac
done

# Default SD card: if no --sd was given, look for an '<sdcard>_rw' or '<sdcard>'
# directory in the current working directory and use it automatically (the
# '_rw' variant is preferred, so guest changes are written back; otherwise the
# read-only snapshot is used). If neither exists and the terminal is
# interactive, offer to download the Synthstrom factory card contents into
# ./${SD_DIR} and use that.
if [ ${#SD_ARGS[@]} -eq 0 ]; then
    for default_sd in "${SD_DIR}_rw" "${SD_DIR}"; do
        if [ -d "${default_sd}" ]; then
            log "No --sd given; defaulting to ./${default_sd}"
            sd_setup "${default_sd}"
            break
        fi
    done
fi
if [ ${#SD_ARGS[@]} -eq 0 ] && [ -t 0 ]; then
    printf '%s\n' "No SD card specified and no ./${SD_DIR} folder found." >&2
    printf '%s' "Download ${FACTORY_SD_NAME} from Synthstrom and use it? [y/N] " >&2
    read -r reply || reply=""
    case "${reply}" in
        [yY]|[yY][eE][sS])
            download_and_unzip "${FACTORY_SD_URL}" "${SD_DIR}" "${FACTORY_SD_NAME}"
            if [ -d "${SD_DIR}" ]; then
                sd_setup "${SD_DIR}"
            else
                warn "factory card download produced no ./${SD_DIR} folder; continuing without an SD card"
            fi
            ;;
        *)
            log "Continuing without an SD card"
            ;;
    esac
fi

# 'coremidi' bridges a transport to a real host MIDI port via a standalone
# helper (scripts/midi_bridge.c) connected to a QEMU UNIX-socket chardev.
BRIDGE_SPECS=()
DIN_SOCK="${TMPDIR:-/tmp}/delugemu-din.$$.sock"
USB_SOCK="${TMPDIR:-/tmp}/delugemu-usb.$$.sock"
MIDI_BRIDGE_SRC="${REPO_ROOT}/scripts/midi_bridge.c"
MIDI_BRIDGE_BIN="${REPO_ROOT}/build/midi_bridge"

ensure_midi_bridge() {
    case "$(uname -s)" in
        Darwin) ;;
        *) die "'coremidi' MIDI bridging is currently macOS-only" ;;
    esac
    if [ ! -x "${MIDI_BRIDGE_BIN}" ] \
        || [ "${MIDI_BRIDGE_SRC}" -nt "${MIDI_BRIDGE_BIN}" ]; then
        log "Building MIDI bridge helper"
        mkdir -p "$(dirname "${MIDI_BRIDGE_BIN}")"
        cc -O2 -Wall -o "${MIDI_BRIDGE_BIN}" "${MIDI_BRIDGE_SRC}" \
            -framework CoreMIDI -framework CoreFoundation \
            || die "failed to build MIDI bridge helper"
    fi
}

# SCIF0 (DIN MIDI) and the monitor. With a MIDI backend, SCIF0 takes that
# chardev and the monitor moves to its own stdio; otherwise the two share stdio.
SERIAL_ARGS=()
if [ "${MIDI}" = "coremidi" ]; then
    rm -f "${DIN_SOCK}"
    SERIAL_ARGS=(-chardev "socket,id=din,path=${DIN_SOCK},server=on,wait=off" \
                 -serial chardev:din -monitor stdio)
    BRIDGE_SPECS+=("DelugEmu DIN=${DIN_SOCK}")
    log "Bridging SCIF0/DIN-MIDI to a host CoreMIDI port (\"DelugEmu DIN\")"
elif [ -n "${MIDI}" ]; then
    SERIAL_ARGS=(-serial "${MIDI}" -monitor stdio)
    log "Routing SCIF0/MIDI to chardev: ${MIDI}"
else
    SERIAL_ARGS=(-serial mon:stdio)
fi

# Host USB-MIDI device: present the device on USB200 and route its bulk pipes
# to the second serial chardev (serial_hd(1)), which the SoC binds to usb0.
USB_MIDI_ARGS=()
if [ "${USB_MIDI}" = "coremidi" ]; then
    rm -f "${USB_SOCK}"
    USB_MIDI_ARGS=(-chardev "socket,id=usbm,path=${USB_SOCK},server=on,wait=off" \
                   -serial chardev:usbm -global "rza1l-usb.midi=on")
    BRIDGE_SPECS+=("DelugEmu USB=${USB_SOCK}")
    log "Bridging USB-MIDI to a host CoreMIDI port (\"DelugEmu USB\")"
elif [ -n "${USB_MIDI}" ]; then
    USB_MIDI_ARGS=(-serial "${USB_MIDI}" -global "rza1l-usb.midi=on")
    log "Attaching host USB-MIDI device on chardev: ${USB_MIDI}"
fi

# Display mode.
DISPLAY_ARGS=()
case "${DISPLAY_MODE}" in
    headless) DISPLAY_ARGS=(-nographic) ;;
    console)
        case "$(uname -s)" in
            Darwin)
                DISPLAY_ARGS=(-display cocoa,zoom-to-fit=off,show-cursor=on)
                ;;
            *)
                DISPLAY_ARGS=()    # let QEMU open its default GUI
                ;;
        esac
        ;;
    none)     DISPLAY_ARGS=(-display none) ;;
    *) die "unknown --display mode '${DISPLAY_MODE}' (console|headless|none)" ;;
esac

# Front-panel skin image. The skin device loads "Deluge_Plain.png" from the
# working directory by default, which breaks when run.sh is invoked from another
# directory (or from a relocatable bundle). Resolve it next to the repo/bundle
# root (REPO_ROOT, set by common.sh) and pass an absolute path so it loads
# regardless of cwd. DELUGEMU_SKIN overrides the location.
SKIN_ARGS=()
SKIN_IMAGE="${DELUGEMU_SKIN:-${REPO_ROOT}/Deluge_Plain.png}"
if [ "${DISPLAY_MODE}" = "console" ]; then
    if [ -f "${SKIN_IMAGE}" ]; then
        SKIN_ARGS=(-global "deluge-skin.image=${SKIN_IMAGE}")
    else
        warn "skin image not found at ${SKIN_IMAGE}; the panel will render without its photo background"
    fi
fi

# Optional audio backend override, routed to the SSIF (I2S) sink. The device
# opens the OS default backend on its own, so this is normally only needed to
# select a non-default driver. The SoC builds the SSIF internally, so -global
# binds the audiodev to the device's property. 'auto' resolves to the
# recommended host driver for this OS.
#
# On Windows the implicit default (dsound) is unreliable and frequently silent,
# so when no --audio is given there we force the bundled SDL backend, which
# works well. Other platforms keep the device's own default unless overridden.
if [ -z "${AUDIO}" ]; then
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*)
            AUDIO="sdl"
            log "Defaulting Windows audio backend to sdl (dsound is unreliable)"
            ;;
    esac
fi
AUDIO_ARGS=()
if [ -n "${AUDIO}" ]; then
    if [ "${AUDIO}" = "auto" ]; then
        case "$(uname -s)" in
            Darwin)  AUDIO="coreaudio" ;;
            Linux)   AUDIO="pa" ;;
            MINGW*|MSYS*|CYGWIN*) AUDIO="sdl" ;;
            *)       AUDIO="sdl" ;;
        esac
        log "Audio backend auto-selected for $(uname -s): ${AUDIO}"
    fi
    AUDIO_ARGS=(-audiodev "${AUDIO},id=deluge0"
                -global "rza1l-ssif.audiodev=deluge0")
    log "Routing SSIF audio to backend: ${AUDIO}"
fi

# Optional output buffer cushion override (milliseconds), bound to the SSIF's
# prime-ms property. Lower = less perceived delay when playing the emulator
# live; raise it if you hear dropouts.
if [ -n "${AUDIO_BUFFER}" ]; then
    case "${AUDIO_BUFFER}" in
        ''|*[!0-9]*) die "--audio-buffer must be a non-negative integer (ms)" ;;
    esac
    AUDIO_ARGS+=(-global "rza1l-ssif.prime-ms=${AUDIO_BUFFER}")
    log "SSIF output buffer cushion: ${AUDIO_BUFFER} ms"
fi

[ ${#SD_ARGS[@]} -gt 0 ] && log "Attaching SD image"

# Post-run cleanup: reap the MIDI bridge and its sockets, write back a '_rw'
# folder-backed SD, and remove the temporary SD image. Idempotent: clears its
# own traps so it runs at most once across EXIT/INT/TERM.
cleanup() {
    trap - EXIT INT TERM
    if [ -n "${BRIDGE_PID}" ]; then
        kill "${BRIDGE_PID}" 2>/dev/null || true
        rm -f "${DIN_SOCK}" "${USB_SOCK}"
    fi
    if [ -n "${SD_TMP_IMG}" ]; then
        if [ "${SD_WRITEBACK}" = "1" ]; then
            sd_writeback || true
        fi
        rm -f "${SD_TMP_IMG}"
    fi
}

# Launch the host MIDI bridge for any 'coremidi' transports. It connects to the
# QEMU sockets once they appear, so it can start before QEMU.
BRIDGE_PID=""
if [ ${#BRIDGE_SPECS[@]} -gt 0 ]; then
    ensure_midi_bridge
    "${MIDI_BRIDGE_BIN}" "${BRIDGE_SPECS[@]}" &
    BRIDGE_PID=$!
fi

log "Launching ${DELUGE_MACHINE} machine with ${FIRMWARE} (display=${DISPLAY_MODE})"
QEMU_CMD=("${BIN}"
    -M "${DELUGE_MACHINE}"
    -kernel "${FIRMWARE}"
    "${SERIAL_ARGS[@]}"
    "${USB_MIDI_ARGS[@]+"${USB_MIDI_ARGS[@]}"}"
    "${DISPLAY_ARGS[@]+"${DISPLAY_ARGS[@]}"}"
    "${SKIN_ARGS[@]+"${SKIN_ARGS[@]}"}"
    "${AUDIO_ARGS[@]+"${AUDIO_ARGS[@]}"}"
    "${SD_ARGS[@]+"${SD_ARGS[@]}"}"
    "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}")

if [ -n "${BRIDGE_PID}" ] || [ -n "${SD_TMP_IMG}" ]; then
    # Keep this shell alive so the cleanup trap can reap the bridge and/or write
    # back and remove the temporary SD image.
    trap cleanup EXIT INT TERM
    "${QEMU_CMD[@]}"
else
    exec "${QEMU_CMD[@]}"
fi

