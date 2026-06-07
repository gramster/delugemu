#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# CoreMIDI bridge regression test (macOS).
#
# Validates scripts/midi_bridge.c end to end WITHOUT needing QEMU or firmware:
# the test plays both roles around the bridge. It acts as
#
#   * QEMU  — a UNIX-socket server on the path the bridge connects to (exactly
#             the `-chardev socket,...,server=on,wait=off` contract run.sh uses),
#             reading/writing the raw MIDI byte stream the firmware would, and
#   * a DAW — opening the bridge's virtual CoreMIDI ports via python-rtmidi.
#
# It checks both directions and the platform-agnostic stream parser:
#   host -> Deluge : send a CC on the virtual OUTPUT port, expect the raw bytes
#                    to arrive on the socket.
#   Deluge -> host : write raw bytes (incl. running status, a real-time byte and
#                    a SysEx) to the socket, expect framed messages on the
#                    virtual INPUT port.
#
# Skips cleanly (exit 0) when not macOS or when python-rtmidi is unavailable, so
# it is safe to wire into CI.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${REPO_ROOT}/scripts/midi_bridge.c"
BIN="${REPO_ROOT}/build/midi_bridge"

fail() { printf 'MIDI-COREMIDI FAIL: %s\n' "$*" >&2; exit 1; }
skip() { printf 'MIDI-COREMIDI SKIP: %s\n' "$*"; exit 0; }

[ "$(uname -s)" = "Darwin" ] || skip "CoreMIDI bridge is macOS-only"
command -v python3 >/dev/null 2>&1 || skip "python3 not available"
python3 -c 'import rtmidi' 2>/dev/null || skip "python-rtmidi not installed"

# Build the bridge if needed.
if [ ! -x "${BIN}" ] || [ "${SRC}" -nt "${BIN}" ]; then
    mkdir -p "$(dirname "${BIN}")"
    cc -O2 -Wall -o "${BIN}" "${SRC}" \
        -framework CoreMIDI -framework CoreFoundation \
        || fail "failed to build midi_bridge"
fi

export DELUGEMU_BRIDGE_BIN="${BIN}"

python3 - <<'PY'
import os, socket, subprocess, sys, time
import rtmidi

BIN  = os.environ["DELUGEMU_BRIDGE_BIN"]
NAME = "DelugEmu Test"
SOCK = f"/tmp/delugemu-coremidi-test.{os.getpid()}.sock"

def find_port(mid, name):
    for i, n in enumerate(mid.get_ports()):
        if name in n:
            return i
    return None

def wait_port(mid, name, timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        i = find_port(mid, name)
        if i is not None:
            return i
        time.sleep(0.1)
    return None

try:
    os.unlink(SOCK)
except FileNotFoundError:
    pass

# Play QEMU's role: be the socket server the bridge connects to.
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(SOCK)
srv.listen(1)
srv.settimeout(8)

bridge = subprocess.Popen([BIN, f"{NAME}={SOCK}"],
                          stderr=subprocess.DEVNULL)
conn = None
midiout = midiin = None
rc = 0
try:
    conn, _ = srv.accept()
    conn.settimeout(4)

    # The bridge advertises a virtual OUTPUT (Deluge IN) and INPUT (Deluge OUT),
    # both named NAME.
    midiout = rtmidi.MidiOut()
    midiin = rtmidi.MidiIn()
    # Receive SysEx and real-time messages too.
    midiin.ignore_types(sysex=False, timing=False, active_sense=False)

    oi = wait_port(midiout, NAME)
    ii = wait_port(midiin, NAME)
    if oi is None or ii is None:
        print(f"  virtual ports not found (out={oi} in={ii})", file=sys.stderr)
        sys.exit(2)
    midiout.open_port(oi)
    midiin.open_port(ii)
    time.sleep(0.3)  # let the input connection settle

    # --- host -> Deluge: CC #7 = 127 on channel 1 -> raw B0 07 7F on socket ---
    midiout.send_message([0xB0, 0x07, 0x7F])
    got = b""
    end = time.time() + 3
    while len(got) < 3 and time.time() < end:
        try:
            got += conn.recv(64)
        except socket.timeout:
            break
    print(f"  host->Deluge bytes: {got.hex() or '(none)'}")
    if got[:3] != bytes([0xB0, 0x07, 0x7F]):
        print("  FAIL: CC did not reach the socket as raw MIDI", file=sys.stderr)
        sys.exit(2)

    # --- Deluge -> host: exercise the stream parser ---
    received = []
    midiin.set_callback(lambda ev, d: received.append(ev[0]))
    # Two note-ons via RUNNING STATUS, a real-time CLOCK byte spliced in the
    # middle, then a SysEx. A correct parser yields four discrete messages.
    conn.sendall(bytes([0x90, 0x3C, 0x64,        # note on 60
                        0xF8,                     # real-time clock (interleaved)
                        0x3E, 0x64,               # running-status note on 62
                        0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7]))  # SysEx
    end = time.time() + 3
    while len(received) < 4 and time.time() < end:
        time.sleep(0.05)
    print(f"  Deluge->host messages: {received}")

    want = [[0x90, 0x3C, 0x64], [0xF8], [0x90, 0x3E, 0x64],
            [0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7]]
    if received != want:
        print(f"  FAIL: parser output {received} != {want}", file=sys.stderr)
        sys.exit(2)

    print("  both directions + running-status/real-time/SysEx parsing OK")
finally:
    try:
        if midiin is not None:
            midiin.close_port()
        if midiout is not None:
            midiout.close_port()
    except Exception:
        pass
    bridge.terminate()
    try:
        bridge.wait(timeout=4)
    except Exception:
        bridge.kill()
    if conn is not None:
        conn.close()
    srv.close()
    try:
        os.unlink(SOCK)
    except FileNotFoundError:
        pass
PY
rc=$?

[ "${rc}" -eq 0 ] || fail "CoreMIDI bridge round-trip failed"
printf 'MIDI-COREMIDI OK: virtual ports round-trip MIDI in both directions\n'
