#!/usr/bin/env python3
"""Drive a timed sequence of QMP key events, then quit QEMU.

Usage: dz_play.py <qmp_sock> "<t:qcode[:down|up|tap] ...>"
Each token is "<delay_s>:<qcode>[:action]". delay is seconds to sleep BEFORE
the event (relative to previous). action defaults to 'tap' (down+up). 'hold' =
down only, 'rel' = up only. A token "<delay>:_quit" quits QEMU. "<delay>:_wait"
just sleeps.
"""
import json
import socket
import sys
import time


def main():
    sock = sys.argv[1]
    seq = sys.argv[2].split()

    s = None
    for _ in range(300):
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(sock)
            break
        except OSError:
            time.sleep(0.1)
            s = None
    if s is None:
        print("no QMP", file=sys.stderr)
        sys.exit(1)
    f = s.makefile("rw")

    def cmd(obj):
        f.write(json.dumps(obj) + "\n")
        f.flush()
        return f.readline()

    f.readline()
    cmd({"execute": "qmp_capabilities"})

    def key(qcode, down):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "key", "data": {"down": down,
             "key": {"type": "qcode", "data": qcode}}}]}})

    for tok in seq:
        parts = tok.split(":")
        delay = float(parts[0])
        what = parts[1]
        action = parts[2] if len(parts) > 2 else "tap"
        time.sleep(delay)
        if what == "_quit":
            print("quit", flush=True)
            cmd({"execute": "quit"})
            return
        if what == "_wait":
            continue
        print(f"{what} {action}", flush=True)
        if action == "tap":
            key(what, True)
            time.sleep(0.08)
            key(what, False)
        elif action == "hold":
            key(what, True)
        elif action == "rel":
            key(what, False)


if __name__ == "__main__":
    main()
