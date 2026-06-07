#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Automated pad-click input test.
#
# Starts the Deluge emulator with a QMP socket, injects absolute-pointer and
# left-button events via a single persistent QMP connection to simulate clicking
# six pads, and verifies the input handler and PIC both logged the expected
# events.
#
# Usage:
#   ./tests/pad-click.sh
#   DELUGE_FIRMWARE=path ./tests/pad-click.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${REPO_ROOT}/qemu/build/qemu-system-arm"

QMP_SOCK="/tmp/deluge_pad_click_qmp.sock"
TRACE_FILE="/tmp/deluge_pad_click_trace.log"

BOOT_SECONDS="${BOOT_SECONDS:-12}"

fail() { printf 'PAD-CLICK FAIL: %s\n' "$*" >&2; exit 1; }
skip() { printf 'PAD-CLICK SKIP: %s\n' "$*"; exit 0; }

[ -x "${BIN}" ] || fail "qemu-system-arm not built"

FIRMWARE="${DELUGE_FIRMWARE:-}"
if [ -z "${FIRMWARE}" ]; then
    for cand in "${REPO_ROOT}/firmware2/deluge.elf" "${REPO_ROOT}/firmware/deluge.elf"; do
        [ -f "${cand}" ] && FIRMWARE="${cand}" && break
    done
fi
[ -n "${FIRMWARE}" ] && [ -f "${FIRMWARE}" ] \
    || skip "no firmware image found (set DELUGE_FIRMWARE=path to run)"

SD_ARGS=()
for cand in "${REPO_ROOT}/build/deluge_sd.img"; do
    [ -f "${cand}" ] && SD_ARGS=(-drive "if=sd,format=raw,file=${cand}") && break
done

rm -f "${QMP_SOCK}" "${TRACE_FILE}"

# Start QEMU with a display console so QMP input-send-event has a console to route to.
"${BIN}" \
    -M deluge \
    -kernel "${FIRMWARE}" \
    "${SD_ARGS[@]+"${SD_ARGS[@]}"}" \
    -display cocoa,zoom-to-fit=off,show-cursor=on \
    -serial mon:stdio \
    -qmp "unix:${QMP_SOCK},server,nowait" \
    >/dev/null 2>"${TRACE_FILE}" &
QEMU_PID=$!
printf '  started qemu pid=%d, waiting %ss for boot...\n' "${QEMU_PID}" "${BOOT_SECONDS}"

cleanup() {
    kill -KILL "${QEMU_PID}" 2>/dev/null || true
    wait "${QEMU_PID}" 2>/dev/null || true
    rm -f "${QMP_SOCK}"
}
trap cleanup EXIT

sleep "${BOOT_SECONDS}"
kill -0 "${QEMU_PID}" 2>/dev/null || fail "qemu exited before we could inject events"

for i in $(seq 1 20); do
    [ -S "${QMP_SOCK}" ] && break
    sleep 0.2
done
[ -S "${QMP_SOCK}" ] || fail "QMP socket never appeared"

# Inject 6 pad clicks over a single persistent Python QMP connection and verify.
python3 - "${QMP_SOCK}" "${TRACE_FILE}" << 'PYEOF'
import sys, socket, json, time

sock_path  = sys.argv[1]
trace_file = sys.argv[2]

W, H    = 2256, 1584
ABS_MAX = 0x7fff

def to_abs(sx, sy):
    return round(sx * ABS_MAX / (W - 1)), round(sy * ABS_MAX / (H - 1))

# Pad centres: X = 98 + col*119, Y = 632 + row*119
pads = [
    (0, 0, "pad(0,0)"), (2, 0, "pad(2,0)"), (0, 2, "pad(0,2)"),
    (4, 2, "pad(4,2)"), (3, 5, "pad(3,5)"), (7, 6, "pad(7,6)"),
]

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sock_path)
s.settimeout(10)

def recv_line():
    buf = b""
    while True:
        c = s.recv(1)
        if not c:
            break
        buf += c
        if c == b"\n":
            break
    return buf.decode().strip()

def send_cmd(obj):
    s.sendall((json.dumps(obj) + "\n").encode())
    return recv_line()

recv_line()  # greeting
send_cmd({"execute": "qmp_capabilities"})

for col, row, label in pads:
    sx = 98  + col * 119
    sy = 632 + row * 119
    ax, ay = to_abs(sx, sy)
    print(f"  injecting click: {label} skin=({sx},{sy}) abs=({ax},{ay})")
    r = send_cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
        {"type": "btn", "data": {"down": True,  "button": "left"}},
        {"type": "btn", "data": {"down": False, "button": "left"}},
    ]}})
    if "error" in r:
        print(f"  QMP error for {label}: {r}", file=sys.stderr)
    time.sleep(0.35)

s.close()
time.sleep(0.5)

with open(trace_file) as f:
    lines = f.readlines()

taps  = sum(1 for l in lines if "deluge_input: tap "     in l)
hits  = sum(1 for l in lines if "deluge_input: tap hit"  in l)
miss  = sum(1 for l in lines if "deluge_input: tap miss" in l)
pic_e = sum(1 for l in lines if "deluge_pic: send_event" in l)
pic_c = sum(1 for l in lines if "deluge_pic: SET_COLOUR" in l)

print(f"  taps={taps} hits={hits} misses={miss} pic_events={pic_e} set_colour={pic_c}")
for l in lines:
    if any(t in l for t in ["deluge_input: tap", "deluge_pic: send_event", "deluge_pic: SET_COLOUR"]):
        print(" ", l.rstrip())

ok = True
if taps < 6:
    print(f"FAIL: only {taps}/6 taps reached handler", file=sys.stderr); ok = False
if hits < 6:
    print(f"FAIL: only {hits}/6 hits (miss={miss})", file=sys.stderr); ok = False
if pic_e < 12:
    print(f"FAIL: only {pic_e}/12 PIC events (expected press+release x6)", file=sys.stderr); ok = False
if ok:
    print("PAD-CLICK OK: all 6 clicks registered and delivered to firmware")
    sys.exit(0)
else:
    sys.exit(1)
PYEOF
