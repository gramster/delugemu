#!/usr/bin/env python3
"""Bidirectional CoreMIDI router + monitor for testing delugemu against real gear.

macOS does not auto-connect the virtual CoreMIDI ports that delugemu's bridge
creates ("DelugEmu DIN" / "DelugEmu USB") to a physical instrument. This helper
wires two endpoints together by name and prints every message that crosses, so
you can play your hardware into the emulated Deluge and vice-versa and watch the
traffic.

Given two name fragments A and B it connects, bidirectionally:

    A out (source)  -->  B in  (destination)
    B out (source)  -->  A in  (destination)

So with the defaults it routes your Summit's keyboard into the Deluge and the
Deluge's MIDI out back to the Summit.

Usage:
    ./scripts/midi_route.py                 # A="Summit"  B="DelugEmu"
    ./scripts/midi_route.py Summit "DelugEmu DIN"
    ./scripts/midi_route.py --list          # list ports and exit

Names are matched as case-insensitive substrings of the CoreMIDI port name.
Requires python-rtmidi (`pip install python-rtmidi`). Ctrl-C to stop.
"""
import sys
import time

try:
    import rtmidi
except ImportError:
    sys.exit("python-rtmidi not installed: pip install python-rtmidi")


def list_ports():
    mi, mo = rtmidi.MidiIn(), rtmidi.MidiOut()
    print("INPUTS  (sources — MIDI OUT of a device, we read from these):")
    for i, n in enumerate(mi.get_ports()):
        print(f"  in[{i}] {n}")
    print("OUTPUTS (destinations — MIDI IN of a device, we send to these):")
    for i, n in enumerate(mo.get_ports()):
        print(f"  out[{i}] {n}")


def find(ports, fragment):
    frag = fragment.lower()
    for i, n in enumerate(ports):
        if frag in n.lower():
            return i, n
    return None, None


def find_index(ports, exact_name):
    for i, n in enumerate(ports):
        if n == exact_name:
            return i
    return None


def resolve_endpoint(fragment):
    """Pick ONE port name matching `fragment` and open its source (input) and
    destination (output) by that exact name, so both directions use the same
    physical port. Returns (midi_in, midi_out, name) with None for any side the
    device doesn't expose."""
    in_ports = rtmidi.MidiIn().get_ports()
    out_ports = rtmidi.MidiOut().get_ports()
    frag = fragment.lower()
    in_matches = [n for n in in_ports if frag in n.lower()]
    out_matches = [n for n in out_ports if frag in n.lower()]

    # Prefer a name that exists on BOTH sides; fall back to whichever side has
    # a match. This guarantees we never bind input to one port and output to a
    # differently-named one (e.g. "DelugEmu USB" vs "DelugEmu DIN").
    common = [n for n in in_matches if n in out_matches]
    candidates = common or in_matches or out_matches
    if not candidates:
        return None, None, None

    all_names = sorted(set(in_matches) | set(out_matches))
    if len(all_names) > 1:
        print(f"note: '{fragment}' matches multiple ports {all_names}; "
              f"using '{candidates[0]}' (pass a more specific name to override)",
              flush=True)
    name = candidates[0]

    midi_in = None
    ii = find_index(in_ports, name)
    if ii is not None:
        midi_in = rtmidi.MidiIn()
        # Forward everything, including SysEx and real-time.
        midi_in.ignore_types(sysex=False, timing=False, active_sense=False)
        midi_in.open_port(ii)

    midi_out = None
    oi = find_index(out_ports, name)
    if oi is not None:
        midi_out = rtmidi.MidiOut()
        midi_out.open_port(oi)

    return midi_in, midi_out, name


def describe(msg):
    if not msg:
        return "(empty)"
    st = msg[0]
    hi, ch = st & 0xF0, (st & 0x0F) + 1
    if hi == 0x90 and len(msg) >= 3 and msg[2] > 0:
        return f"NoteOn  ch{ch:<2} note={msg[1]:<3} vel={msg[2]}"
    if hi == 0x80 or (hi == 0x90 and len(msg) >= 3 and msg[2] == 0):
        return f"NoteOff ch{ch:<2} note={msg[1]}"
    if hi == 0xB0 and len(msg) >= 3:
        return f"CC      ch{ch:<2} #{msg[1]}={msg[2]}"
    if hi == 0xC0:
        return f"Program ch{ch:<2} {msg[1]}"
    if hi == 0xE0 and len(msg) >= 3:
        return f"PitchBend ch{ch:<2} {((msg[2] << 7) | msg[1]) - 8192:+d}"
    if st == 0xF0:
        return f"SysEx   {len(msg)} bytes"
    if st in (0xF8, 0xFA, 0xFB, 0xFC, 0xFE, 0xFF):
        return f"Realtime 0x{st:02X}"
    return " ".join(f"{b:02X}" for b in msg)


def make_handler(dest, arrow, dest_name):
    def handler(event, _data=None):
        msg, _delta = event
        dest.send_message(msg)
        print(f"{arrow} {dest_name:<16} {describe(msg)}", flush=True)
    return handler


def main():
    args = [a for a in sys.argv[1:] if a not in ("--list", "-l")]
    if "--list" in sys.argv or "-l" in sys.argv:
        list_ports()
        return 0

    a_frag = args[0] if len(args) > 0 else "Summit"
    b_frag = args[1] if len(args) > 1 else "DelugEmu"

    a_in, a_out, a_name = resolve_endpoint(a_frag)
    b_in, b_out, b_name = resolve_endpoint(b_frag)

    missing = []
    if a_in is None:
        missing.append(f"input (source) matching '{a_frag}'")
    if a_out is None:
        missing.append(f"output (destination) matching '{a_frag}'")
    if b_in is None:
        missing.append(f"input (source) matching '{b_frag}'")
    if b_out is None:
        missing.append(f"output (destination) matching '{b_frag}'")
    if missing:
        print("Could not find: " + "; ".join(missing) + "\n", file=sys.stderr)
        list_ports()
        return 1

    # A out -> B in, and B out -> A in.
    a_in.set_callback(make_handler(b_out, "A->B", b_name))
    b_in.set_callback(make_handler(a_out, "B->A", a_name))

    print(f"Routing (both ports bound by exact name):\n"
          f"  A->B  '{a_name}' OUT  -->  '{b_name}' IN\n"
          f"  B->A  '{b_name}' OUT  -->  '{a_name}' IN\n"
          f"Play your instrument; Ctrl-C to stop.\n")
    try:
        while True:
            time.sleep(0.2)
    except KeyboardInterrupt:
        print("\nstopped.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
