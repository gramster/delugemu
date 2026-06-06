#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Run Deluge firmware under the emulator.
#
# Usage:
#   ./scripts/run.sh <firmware.elf> [extra qemu args...]
#
# Common extra args:
#   -d guest_errors,unimp   log unimplemented device accesses
#   -S -s                   freeze at reset and wait for gdb on :1234
#   -serial mon:stdio       multiplex monitor + serial on the terminal

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

BIN="${QEMU_BUILD_DIR}/qemu-system-arm"
[ -x "${BIN}" ] || die "qemu-system-arm not built. Run ./scripts/build.sh first."

FIRMWARE="${1:-}"
[ -n "${FIRMWARE}" ] || die "Usage: $0 <firmware.elf> [extra qemu args...]"
[ -f "${FIRMWARE}" ] || die "Firmware not found: ${FIRMWARE}"
shift

log "Launching ${DELUGE_MACHINE} machine with ${FIRMWARE}"
exec "${BIN}" \
    -M "${DELUGE_MACHINE}" \
    -kernel "${FIRMWARE}" \
    -serial mon:stdio \
    -nographic \
    "$@"
