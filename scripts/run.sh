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
#   --usb-midi <chardev>    Attach a host USB-MIDI device backed by a QEMU
#                           chardev spec, e.g. --usb-midi udp:127.0.0.1:1998 or
#                           --usb-midi pty. Bridges the firmware's bulk USB-MIDI
#                           pipes to that backend (routed to serial slot 1).
#   --audio <driver>        Route the SSIF (I2S) output to a host audio backend
#                           (-audiodev <driver>). Use 'auto' to pick the OS
#                           default (coreaudio on macOS, pa on Linux, dsound on
#                           Windows); or name one explicitly, e.g. --audio
#                           coreaudio / pa / sdl / wav / none. Play a note in an
#                           instrument clip to hear 44.1 kHz stereo output.
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
    sed -n '4,32p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
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
USB_MIDI=""
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

# Host USB-MIDI device: present the device on USB200 and route its bulk pipes
# to the second serial chardev (serial_hd(1)), which the SoC binds to usb0.
USB_MIDI_ARGS=()
if [ -n "${USB_MIDI}" ]; then
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
    *) die "unknown --display mode '${DISPLAY_MODE}' (headless|console|none)" ;;
esac

# Optional audio backend, routed to the SSIF (I2S) sink. The SoC builds the
# SSIF internally, so -global binds the audiodev to the device's property.
# 'auto' resolves to the recommended host driver for this OS.
AUDIO_ARGS=()
if [ -n "${AUDIO}" ]; then
    if [ "${AUDIO}" = "auto" ]; then
        case "$(uname -s)" in
            Darwin)  AUDIO="coreaudio" ;;
            Linux)   AUDIO="pa" ;;
            MINGW*|MSYS*|CYGWIN*) AUDIO="dsound" ;;
            *)       AUDIO="sdl" ;;
        esac
        log "Audio backend auto-selected for $(uname -s): ${AUDIO}"
    fi
    AUDIO_ARGS=(-audiodev "${AUDIO},id=deluge0"
                -global "rza1l-ssif.audiodev=deluge0")
    log "Routing SSIF audio to backend: ${AUDIO}"
fi

[ ${#SD_ARGS[@]} -gt 0 ] && log "Attaching SD image"

log "Launching ${DELUGE_MACHINE} machine with ${FIRMWARE} (display=${DISPLAY_MODE})"
exec "${BIN}" \
    -M "${DELUGE_MACHINE}" \
    -kernel "${FIRMWARE}" \
    "${SERIAL_ARGS[@]}" \
    "${USB_MIDI_ARGS[@]+"${USB_MIDI_ARGS[@]}"}" \
    "${DISPLAY_ARGS[@]+"${DISPLAY_ARGS[@]}"}" \
    "${AUDIO_ARGS[@]+"${AUDIO_ARGS[@]}"}" \
    "${SD_ARGS[@]+"${SD_ARGS[@]}"}" \
    "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"

