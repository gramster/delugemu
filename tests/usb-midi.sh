#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# USB host-MIDI enumeration regression test.
#
# Boots a Deluge firmware image with a synthetic full-speed USB-MIDI device
# presented on the host port (-global rza1l-usb.midi=on) and asserts that the
# firmware's USB host stack enumerates and configures it:
#
#   * the control-transfer state machine reaches SET_CONFIGURATION
#     (RZA1L_USB_DEBUG traces "SET_CONFIGURATION 1 -> configured=1"),
#   * zero Data Abort / Prefetch Abort exceptions occur (a healthy boot),
#   * the USBI interrupt actually fires (IRQ stream is present).
#
# Firmware is not shipped with the repo; the test SKIPS cleanly (exit 0) when no
# image is present, so it is safe to wire into CI.
#
# Usage:
#   ./tests/usb-midi.sh
#   DELUGE_FIRMWARE=path ./tests/usb-midi.sh
#   RUN_SECONDS=15 ./tests/usb-midi.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${REPO_ROOT}/qemu/build/qemu-system-arm"

RUN_SECONDS="${RUN_SECONDS:-12}"

fail() { printf 'USB-MIDI FAIL: %s\n' "$*" >&2; exit 1; }
skip() { printf 'USB-MIDI SKIP: %s\n' "$*"; exit 0; }

[ -x "${BIN}" ] || fail "qemu-system-arm not built at ${BIN} (run scripts/build.sh)"

FIRMWARE="${DELUGE_FIRMWARE:-}"
if [ -z "${FIRMWARE}" ]; then
    for cand in "${REPO_ROOT}/firmware2/deluge.elf" "${REPO_ROOT}/firmware/deluge.elf"; do
        if [ -f "${cand}" ]; then FIRMWARE="${cand}"; break; fi
    done
fi
[ -n "${FIRMWARE}" ] && [ -f "${FIRMWARE}" ] \
    || skip "no firmware image found (set DELUGE_FIRMWARE=path to run)"

INT_LOG=/tmp/delugemu-usbmidi-int.log
DBG_LOG=/tmp/delugemu-usbmidi-dbg.log
rm -f "${INT_LOG}" "${DBG_LOG}"

printf 'USB-MIDI: firmware=%s\n' "${FIRMWARE#${REPO_ROOT}/}"
printf '  booting with USB-MIDI device attached for %ss...\n' "${RUN_SECONDS}"

# RZA1L_USB_DEBUG makes the model trace control-transfer milestones to stderr;
# -d int -D captures the exception stream for the abort/IRQ assertions.
RZA1L_USB_DEBUG=1 "${BIN}" -M deluge -kernel "${FIRMWARE}" -nographic \
    -global rza1l-usb.midi=on -d int -D "${INT_LOG}" >/dev/null 2>"${DBG_LOG}" &
pid=$!
sleep "${RUN_SECONDS}"
kill -INT "${pid}" 2>/dev/null || true
sleep 1
kill -KILL "${pid}" 2>/dev/null || true
wait "${pid}" 2>/dev/null || true

[ -s "${INT_LOG}" ] || fail "qemu produced no interrupt log"

aborts=$(grep -cE 'Taking exception 4 \[' "${INT_LOG}" || true)
prefetch=$(grep -cE 'Taking exception 3 \[' "${INT_LOG}" || true)
irqs=$(grep -cE 'Taking exception 5 \[' "${INT_LOG}" || true)
configured=$(grep -c 'SET_CONFIGURATION 1 -> configured=1' "${DBG_LOG}" || true)

printf '  data-aborts=%s prefetch-aborts=%s irqs=%s set-configuration=%s\n' \
    "${aborts}" "${prefetch}" "${irqs}" "${configured}"

[ "${aborts}" -eq 0 ]   || fail "${aborts} data-abort exception(s) during enumeration"
[ "${prefetch}" -eq 0 ] || fail "${prefetch} prefetch-abort exception(s) during enumeration"
[ "${irqs}" -ge 10 ]    || fail "only ${irqs} IRQs; USB interrupts likely never fired"
[ "${configured}" -ge 1 ] \
    || fail "firmware never reached SET_CONFIGURATION; enumeration did not complete"

printf 'USB-MIDI OK: firmware enumerated and configured the USB-MIDI device\n'
