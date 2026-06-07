#!/usr/bin/env python3
"""Press a key (qcode) over QMP and screendump. Usage: press_key.py <qcode> <name>"""
import json
import socket
import sys
import time

SOCK = "/tmp/dz_qmp.sock"


def main():
    qcode = sys.argv[1]
    name = sys.argv[2] if len(sys.argv) > 2 else qcode
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    f = s.makefile("rw")

    def cmd(obj):
        f.write(json.dumps(obj) + "\n")
        f.flush()
        return f.readline()

    f.readline()
    cmd({"execute": "qmp_capabilities"})
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "key", "data": {"down": True,
         "key": {"type": "qcode", "data": qcode}}}]}})
    time.sleep(0.08)
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "key", "data": {"down": False,
         "key": {"type": "qcode", "data": qcode}}}]}})
    time.sleep(0.6)
    cmd({"execute": "human-monitor-command",
         "arguments": {"command-line": f"screendump /tmp/press_{name}.ppm"}})
    print(f"pressed {qcode} -> /tmp/press_{name}.ppm")


if __name__ == "__main__":
    main()
