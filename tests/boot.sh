#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Firmware-boot regression test.
#
# Boots a Deluge firmware image under the emulator headless and asserts that the
# boot reaches a healthy steady state:
#
#   * zero Data Abort  exceptions (would indicate a bad/unmapped MMIO access),
#   * zero Prefetch Abort exceptions (would indicate a bad instruction fetch),
#   * a stream of IRQ exceptions (proves we got past reset into the timer-driven
#     main loop rather than spinning at the vector table).
#
# Unimplemented-device-access warnings are NOT treated as failures: several
# regions (rza1l.boot.mirror, rza1l.io.mid/high) are deliberate catch-alls that
# the firmware probes constantly. We only care that nothing actually faults.
#
# Firmware is not shipped with the repo. The test locates an image and SKIPS
# cleanly (exit 0) when none is present, so it is safe to wire into CI.
#
# Usage:
#   ./tests/boot.sh                 # auto-locate firmware; SD image if built
#   DELUGE_FIRMWARE=path ./tests/boot.sh
#   BOOT_SECONDS=15 ./tests/boot.sh # override the run window

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# On Windows (MSYS2/MinGW) the emulator is qemu-system-arm.exe.
case "$(uname -s 2>/dev/null || echo unknown)" in
    MINGW*|MSYS*|CYGWIN*) EXE_SUFFIX=".exe" ;;
    *)                    EXE_SUFFIX="" ;;
esac
BIN="${REPO_ROOT}/qemu/build/qemu-system-arm${EXE_SUFFIX}"

# Minimum IRQ exceptions expected from a healthy boot. The firmware programs
# several periodic timers (OSTM/MTU2) during init; a stuck/early-faulting boot
# produces far fewer. Observed: 19 without an SD image, 183 with one.
MIN_IRQS="${MIN_IRQS:-10}"

# How long to let the firmware run before sampling the log.
BOOT_SECONDS="${BOOT_SECONDS:-12}"

fail() { printf 'BOOT FAIL: %s\n' "$*" >&2; exit 1; }
skip() { printf 'BOOT SKIP: %s\n' "$*"; exit 0; }

[ -x "${BIN}" ] || fail "qemu-system-arm not built at ${BIN} (run scripts/build.sh)"

# Locate a firmware image: explicit override, then the usual build outputs.
FIRMWARE="${DELUGE_FIRMWARE:-}"
if [ -z "${FIRMWARE}" ]; then
    for cand in "${REPO_ROOT}/firmware2/deluge.elf" "${REPO_ROOT}/firmware/deluge.elf"; do
        if [ -f "${cand}" ]; then FIRMWARE="${cand}"; break; fi
    done
fi
[ -n "${FIRMWARE}" ] && [ -f "${FIRMWARE}" ] \
    || skip "no firmware image found (set DELUGE_FIRMWARE=path to run)"

# Run one boot, logging interrupts to $2; $1 is a human label, remaining args
# are extra qemu args. The firmware never exits on its own, so we run it in the
# background, let it boot for BOOT_SECONDS, then SIGINT it for a clean shutdown
# (which flushes the -D log) before falling back to SIGKILL.
run_boot() {
    local label="$1"; shift
    local log="$1"; shift
    rm -f "${log}"
    printf '  booting (%s) for %ss...\n' "${label}" "${BOOT_SECONDS}"
    "${BIN}" -M deluge -kernel "${FIRMWARE}" -nographic \
        -d int -D "${log}" "$@" >/dev/null 2>&1 &
    local pid=$!
    sleep "${BOOT_SECONDS}"
    kill -INT "${pid}" 2>/dev/null || true
    sleep 1
    kill -KILL "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
    [ -s "${log}" ] || fail "${label}: qemu produced no interrupt log"
}

# Assert health on an interrupt log produced by run_boot.
assert_healthy() {
    local label="$1"; local log="$2"
    local aborts prefetch irqs
    aborts=$(grep -cE 'Taking exception 4 \[' "${log}" || true)
    prefetch=$(grep -cE 'Taking exception 3 \[' "${log}" || true)
    irqs=$(grep -cE 'Taking exception 5 \[' "${log}" || true)

    printf '  %s: data-aborts=%s prefetch-aborts=%s irqs=%s\n' \
        "${label}" "${aborts}" "${prefetch}" "${irqs}"

    [ "${aborts}" -eq 0 ]   || fail "${label}: ${aborts} data-abort exception(s) during boot"
    [ "${prefetch}" -eq 0 ] || fail "${label}: ${prefetch} prefetch-abort exception(s) during boot"
    [ "${irqs}" -ge "${MIN_IRQS}" ] \
        || fail "${label}: only ${irqs} IRQs (expected >= ${MIN_IRQS}); boot likely stalled"
}

printf 'BOOT: firmware=%s\n' "${FIRMWARE#${REPO_ROOT}/}"

# 1) Baseline boot, no SD card.
run_boot "no-sd" /tmp/delugemu-boot-nosd.log
assert_healthy "no-sd" /tmp/delugemu-boot-nosd.log

# 2) If an SD image has been built, boot with it attached too. The storage stack
#    raises many more IRQs, so this also exercises the SDHI path under CI.
SD_IMG="${DELUGE_SD_IMG:-${REPO_ROOT}/build/deluge_sd.img}"
if [ -f "${SD_IMG}" ]; then
    run_boot "with-sd" /tmp/delugemu-boot-sd.log \
        -drive "if=sd,format=raw,file=${SD_IMG}"
    assert_healthy "with-sd" /tmp/delugemu-boot-sd.log
else
    printf '  (no SD image at %s; skipping the with-sd case)\n' "${SD_IMG#${REPO_ROOT}/}"
fi

printf 'BOOT OK: firmware reached a healthy steady state\n'
