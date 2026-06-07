#!/usr/bin/env python3
"""Drive a Deluge rotary encoder over QMP and capture screenshots.

Usage: enc_test.py <x> <y> <label> [updelta] [downdelta]
"""
import json
import os
import socket
import sys
import time

SOCK = "/tmp/dz_qmp.sock"
W, H, MAX = 2256, 1584, 0x7fff


def ax(x):
    return round(x * MAX / (W - 1))


def ay(y):
    return round(y * MAX / (H - 1))


def main():
    x = int(sys.argv[1])
    y = int(sys.argv[2])
    label = sys.argv[3]
    nup = int(sys.argv[4]) if len(sys.argv) > 4 else 8
    ndn = int(sys.argv[5]) if len(sys.argv) > 5 else 8

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    f = s.makefile("rw")

    def cmd(obj):
        f.write(json.dumps(obj) + "\n")
        f.flush()
        return f.readline()

    f.readline()  # greeting
    cmd({"execute": "qmp_capabilities"})

    def move(px, py):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": ax(px)}},
            {"type": "abs", "data": {"axis": "y", "value": ay(py)}}]}})
        time.sleep(0.1)

    def wheel(up, n=1):
        btn = "wheel-up" if up else "wheel-down"
        for _ in range(n):
            cmd({"execute": "input-send-event", "arguments": {"events": [
                {"type": "btn", "data": {"down": True, "button": btn}}]}})
            cmd({"execute": "input-send-event", "arguments": {"events": [
                {"type": "btn", "data": {"down": False, "button": btn}}]}})
            time.sleep(0.15)

    def shot(name):
        path = f"/tmp/enc_{name}.ppm"
        cmd({"execute": "screendump", "arguments": {"filename": path}})
        time.sleep(0.2)
        os.system(f"sips -s format png {path} --out /tmp/enc_{name}.png "
                  ">/dev/null 2>&1")

    move(x, y)
    shot(f"{label}_before")
    wheel(True, nup)
    time.sleep(0.5)
    shot(f"{label}_up")
    wheel(False, ndn)
    time.sleep(0.5)
    shot(f"{label}_dn")
    s.close()
    print("done")


if __name__ == "__main__":
    main()
