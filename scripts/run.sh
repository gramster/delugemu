#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Run Deluge firmware under the emulator.
#
# Usage:
#   ./scripts/run.sh <firmware.elf> [options] [-- extra qemu args...]
#
# Options:
#   --sd <image>            Attach a FAT disk image to the SDHI SD card slot.
#   --midi <chardev>        Back SCIF0 (the MIDI UART) with a QEMU chardev spec,
#                           e.g. --midi udp:127.0.0.1:1999 or --midi pty. When
#                           omitted, SCIF0 is multiplexed with the monitor on
#                           stdio (-serial mon:stdio).
#   --audio <driver>        Add a QEMU audio backend (-audiodev <driver>),
#                           e.g. --audio coreaudio / pa / sdl / none. (No audio
#                           device consumes it yet; reserved for the M5 SSI sink.)
#   --display <mode>        Display mode:
#                             headless  no GUI; serial+monitor on stdio (default)
#                             console   open a window with the modelled OLED /
#                                       pad-grid / 7-seg consoles
#                             none      graphics subsystem present but not shown
#   -h, --help              Show this help and exit.
#
# Anything after a literal `--`, or any unrecognised flag, is passed straight
# through to qemu-system-arm. Useful extras:
#   -d guest_errors,unimp   log unimplemented device accesses
#   -S -s                   freeze at reset and wait for gdb on :1234

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

usage() {
    sed -n '4,28p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

BIN="${QEMU_BUILD_DIR}/qemu-system-arm"
[ -x "${BIN}" ] || die "qemu-system-arm not built. Run ./scripts/build.sh first."

FIRMWARE="${1:-}"
case "${FIRMWARE}" in
    -h|--help) usage; exit 0 ;;
esac
[ -n "${FIRMWARE}" ] || { usage; die "missing <firmware.elf>"; }
[ -f "${FIRMWARE}" ] || die "Firmware not found: ${FIRMWARE}"
shift

MIDI=""
AUDIO=""
DISPLAY_MODE="headless"

SD_ARGS=()
EXTRA_ARGS=()

while [ $# -gt 0 ]; do
    case "$1" in
        --)
            shift
            EXTRA_ARGS+=("$@")
            break
            ;;
        --sd)
            [ -n "${2:-}" ] || die "--sd requires an image path"
            [ -f "$2" ] || die "SD image not found: $2"
            SD_ARGS=(-drive "if=sd,format=raw,file=$2")
            shift 2
            ;;
        --sd=*)
            sd_img="${1#--sd=}"
            [ -f "${sd_img}" ] || die "SD image not found: ${sd_img}"
            SD_ARGS=(-drive "if=sd,format=raw,file=${sd_img}")
            shift
            ;;
        --midi)
            [ -n "${2:-}" ] || die "--midi requires a chardev spec"
            MIDI="$2"; shift 2
            ;;
        --midi=*) MIDI="${1#--midi=}"; shift ;;
        --audio)
            [ -n "${2:-}" ] || die "--audio requires a driver name"
            AUDIO="$2"; shift 2
            ;;
        --audio=*) AUDIO="${1#--audio=}"; shift ;;
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

# SCIF0 (MIDI) and the monitor. With a MIDI backend, SCIF0 takes that chardev
# and the monitor moves to its own stdio; otherwise the two share stdio.
SERIAL_ARGS=()
if [ -n "${MIDI}" ]; then
    SERIAL_ARGS=(-serial "${MIDI}" -monitor stdio)
    log "Routing SCIF0/MIDI to chardev: ${MIDI}"
else
    SERIAL_ARGS=(-serial mon:stdio)
fi

# Display mode.
DISPLAY_ARGS=()
case "${DISPLAY_MODE}" in
    headless) DISPLAY_ARGS=(-nographic) ;;
    console)  DISPLAY_ARGS=() ;;                 # let QEMU open its default GUI
    none)     DISPLAY_ARGS=(-display none) ;;
    *) die "unknown --display mode '${DISPLAY_MODE}' (headless|console|none)" ;;
esac

# Optional audio backend (no consumer device yet; reserved for the SSI sink).
AUDIO_ARGS=()
if [ -n "${AUDIO}" ]; then
    AUDIO_ARGS=(-audiodev "${AUDIO},id=deluge0")
    log "Adding audio backend: ${AUDIO}"
fi

[ ${#SD_ARGS[@]} -gt 0 ] && log "Attaching SD image"

log "Launching ${DELUGE_MACHINE} machine with ${FIRMWARE} (display=${DISPLAY_MODE})"
exec "${BIN}" \
    -M "${DELUGE_MACHINE}" \
    -kernel "${FIRMWARE}" \
    "${SERIAL_ARGS[@]}" \
    "${DISPLAY_ARGS[@]+"${DISPLAY_ARGS[@]}"}" \
    "${AUDIO_ARGS[@]+"${AUDIO_ARGS[@]}"}" \
    "${SD_ARGS[@]+"${SD_ARGS[@]}"}" \
    "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"

