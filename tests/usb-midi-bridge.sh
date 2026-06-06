#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# USB-MIDI data-bridge regression test (host <-> firmware, both directions).
#
# Boots a Deluge firmware image with the synthetic USB-MIDI device enabled
# (-global rza1l-usb.midi=on) and the device's MIDI data port bridged to a host
# UNIX-socket chardev. It then exercises the full bulk pipe data path:
#
#   * RX (host -> device): a Universal SysEx "Identity Request"
#     (F0 7E 7F 06 01 F7) is written to the socket. The model frames it into
#     32-bit USB-MIDI packets and delivers it on a bulk IN pipe (BRDY).
#   * TX (device -> host): the firmware's MIDI engine answers via
#     midiSysexReceived() with an "Identity Reply" (F0 7E 7F 06 02 ...),
#     which the model deframes from the bulk OUT pipe (CFIFO/BVAL -> BEMP) and
#     writes back to the socket.
#
# Asserting that the reply (which spans several BEMP-gated transfers) arrives
# byte-for-byte proves both the receive framer and the transmit deframer,
# including the per-pipe BRDY/BEMP transfer interrupts.
#
# Firmware is not shipped with the repo; the test SKIPS cleanly (exit 0) when no
# image (or python3) is present, so it is safe to wire into CI.
#
# Usage:
#   ./tests/usb-midi-bridge.sh
#   DELUGE_FIRMWARE=path ./tests/usb-midi-bridge.sh
#   RUN_SECONDS=12 ./tests/usb-midi-bridge.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${REPO_ROOT}/qemu/build/qemu-system-arm"

RUN_SECONDS="${RUN_SECONDS:-10}"

fail() { printf 'USB-MIDI-BRIDGE FAIL: %s\n' "$*" >&2; exit 1; }
skip() { printf 'USB-MIDI-BRIDGE SKIP: %s\n' "$*"; exit 0; }

[ -x "${BIN}" ] || fail "qemu-system-arm not built at ${BIN} (run scripts/build.sh)"
command -v python3 >/dev/null 2>&1 || skip "python3 not available"

FIRMWARE="${DELUGE_FIRMWARE:-}"
if [ -z "${FIRMWARE}" ]; then
    for cand in "${REPO_ROOT}/firmware2/deluge.elf" "${REPO_ROOT}/firmware/deluge.elf"; do
        if [ -f "${cand}" ]; then FIRMWARE="${cand}"; break; fi
    done
fi
[ -n "${FIRMWARE}" ] && [ -f "${FIRMWARE}" ] \
    || skip "no firmware image found (set DELUGE_FIRMWARE=path to run)"

printf 'USB-MIDI-BRIDGE: firmware=%s\n' "${FIRMWARE#${REPO_ROOT}/}"
printf '  booting with USB-MIDI device bridged to a UNIX socket for up to %ss...\n' \
    "${RUN_SECONDS}"

# The whole exchange is driven from python3: it boots QEMU with the device's
# MIDI port on a UNIX-socket chardev, sends the Identity Request, and reads back
# whatever the firmware transmits. slot 0 (-serial null) keeps SCIF0's DIN MIDI
# from claiming the socket; slot 1 (-serial chardev:um) is the USB MIDI bridge.
SOCK="$(mktemp -u /tmp/delugemu-usbmidi-bridge.XXXXXX.sock)"
export DELUGEMU_BIN="${BIN}"
export DELUGEMU_FW="${FIRMWARE}"
export DELUGEMU_SOCK="${SOCK}"
export DELUGEMU_RUN_SECONDS="${RUN_SECONDS}"

python3 - <<'PY'
import os, re, socket, subprocess, sys, time

binp = os.environ["DELUGEMU_BIN"]
fw   = os.environ["DELUGEMU_FW"]
sock = os.environ["DELUGEMU_SOCK"]
run_seconds = int(os.environ["DELUGEMU_RUN_SECONDS"])

try:
    os.unlink(sock)
except FileNotFoundError:
    pass

cmd = [binp, "-M", "deluge", "-kernel", fw, "-display", "none",
       "-global", "rza1l-usb.midi=on",
       "-serial", "null",
       "-chardev", f"socket,id=um,path={sock},server=on,wait=off",
       "-serial", "chardev:um"]

p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
got = b""
try:
    # Wait for QEMU to create the listening socket, then for the firmware to
    # enumerate the device and arm its first receive transfer.
    deadline = time.time() + run_seconds
    while not os.path.exists(sock) and time.time() < deadline:
        time.sleep(0.1)
    if not os.path.exists(sock):
        print("BRIDGE FAIL: qemu never created the MIDI socket", file=sys.stderr)
        sys.exit(2)
    time.sleep(5)

    c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    c.connect(sock)
    c.setblocking(False)

    # Universal SysEx Identity Request.
    c.sendall(bytes([0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7]))

    end = time.time() + 4
    while time.time() < end:
        try:
            chunk = c.recv(4096)
            if chunk:
                got += chunk
        except BlockingIOError:
            time.sleep(0.05)
        if got.hex().startswith("f07e7f0602") and got.endswith(b"\xf7"):
            break
finally:
    p.terminate()
    try:
        p.wait(timeout=5)
    except Exception:
        p.kill()
    try:
        os.unlink(sock)
    except FileNotFoundError:
        pass

hexs = got.hex()
print(f"  device->host reply: {hexs or '(none)'}")

# An Identity Reply starts F0 7E 7F 06 02 and ends F7. Receiving it proves the
# request reached the firmware (RX framer) and the multi-transfer reply came
# back intact (TX deframer + per-pipe BEMP continuation).
if not got:
    print("BRIDGE FAIL: firmware sent no MIDI in reply to the Identity Request",
          file=sys.stderr)
    sys.exit(2)
if not hexs.startswith("f07e7f0602"):
    print("BRIDGE FAIL: reply is not a SysEx Identity Reply (F0 7E 7F 06 02 ..)",
          file=sys.stderr)
    sys.exit(2)
if not got.endswith(b"\xf7"):
    print("BRIDGE FAIL: Identity Reply truncated (no F7 terminator) -- a "
          "BEMP-gated transfer likely stalled", file=sys.stderr)
    sys.exit(2)

print("  reply is a complete SysEx Identity Reply (multi-transfer TX intact)")
PY
rc=$?

[ "${rc}" -eq 0 ] || fail "MIDI data bridge did not round-trip the SysEx exchange"

printf 'USB-MIDI-BRIDGE OK: bidirectional bulk MIDI bridge round-tripped a SysEx exchange\n'
