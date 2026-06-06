#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Run Deluge firmware under the emulator.
#
# Usage:
#   ./scripts/run.sh <firmware.elf> [--sd <image>] [extra qemu args...]
#
# Options:
#   --sd <image>            attach a FAT disk image to the SDHI SD card slot
#
# Common extra args:
#   -d guest_errors,unimp   log unimplemented device accesses
#   -S -s                   freeze at reset and wait for gdb on :1234
#   -serial mon:stdio       multiplex monitor + serial on the terminal

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

BIN="${QEMU_BUILD_DIR}/qemu-system-arm"
[ -x "${BIN}" ] || die "qemu-system-arm not built. Run ./scripts/build.sh first."

FIRMWARE="${1:-}"
[ -n "${FIRMWARE}" ] || die "Usage: $0 <firmware.elf> [--sd <image>] [extra qemu args...]"
[ -f "${FIRMWARE}" ] || die "Firmware not found: ${FIRMWARE}"
shift

SD_ARGS=()
EXTRA_ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
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
        *)
            EXTRA_ARGS+=("$1")
            shift
            ;;
    esac
done

if [ ${#SD_ARGS[@]} -gt 0 ]; then
    log "Attaching SD image"
fi

log "Launching ${DELUGE_MACHINE} machine with ${FIRMWARE}"
exec "${BIN}" \
    -M "${DELUGE_MACHINE}" \
    -kernel "${FIRMWARE}" \
    -serial mon:stdio \
    -nographic \
    "${SD_ARGS[@]}" \
    "${EXTRA_ARGS[@]}"

