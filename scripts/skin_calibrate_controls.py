#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Detect the precise bounds of the Deluge front-panel controls (buttons, rotary
encoders and the gold-knob level LEDs) in the skin image, seeded by the rough
centre coordinates measured by hand.

The circular RGB pushbuttons and the rotary encoders all have a light (white or
grey) interior bounded by a solid black border, so each control is found by
flood-filling the connected "light" region that contains its seed centre and
taking the bounding box of that region.

Usage:
  python3 scripts/skin_calibrate_controls.py \
      --image Deluge_Plain.png --write-preview /tmp/controls_preview.png

Prints a C table (centre + radius per control) ready to paste into
src/include/hw/display/deluge_skin_controls.h, and optionally writes a preview
PNG with each detected bound drawn.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np


@dataclass
class Seed:
    name: str
    col: int
    row: int
    cx: int
    cy: int
    kind: str  # "button", "encoder"


# (col, row) are the 9x4 button-matrix coordinates from definitions_cxx.hpp.
# Seed centres are the hand-measured pixel coordinates of each control.
SEEDS: list[Seed] = [
    # 8 MOD (gold-knob assignment) buttons, top row. LEDs MOD_0..MOD_7.
    Seed("MOD0", 1, 0, 327, 332, "button"),
    Seed("MOD1", 1, 1, 417, 332, "button"),
    Seed("MOD2", 1, 2, 507, 332, "button"),
    Seed("MOD3", 1, 3, 597, 332, "button"),
    Seed("MOD4", 2, 0, 687, 332, "button"),
    Seed("MOD5", 2, 1, 777, 332, "button"),
    Seed("MOD6", 2, 2, 867, 332, "button"),
    Seed("MOD7", 2, 3, 957, 332, "button"),
    # Named function buttons (each has an indicator LED at the same col,row).
    Seed("AFFECT_ENTIRE", 3, 0, 685, 468, "button"),
    Seed("SESSION_VIEW", 3, 1, 867, 420, "button"),
    Seed("CLIP_VIEW", 3, 2, 868, 516, "button"),
    Seed("SYNTH", 5, 0, 1188, 424, "button"),
    Seed("KIT", 5, 1, 1270, 424, "button"),
    Seed("MIDI", 5, 2, 1355, 424, "button"),
    Seed("CV", 5, 3, 1435, 424, "button"),
    Seed("SCALE_MODE", 6, 0, 1208, 514, "button"),
    Seed("CROSS_SCREEN", 6, 2, 1351, 514, "button"),
    Seed("BACK", 7, 1, 1534, 242, "button"),
    Seed("LOAD", 6, 1, 1534, 330, "button"),
    Seed("SAVE", 6, 3, 1534, 422, "button"),
    Seed("LEARN", 7, 0, 1534, 514, "button"),
    Seed("TAP_TEMPO", 7, 3, 1816, 330, "button"),
    Seed("SYNC_SCALING", 7, 2, 1816, 422, "button"),
    Seed("TRIPLETS", 8, 1, 1816, 510, "button"),
    Seed("PLAY", 8, 3, 2076, 330, "button"),
    Seed("RECORD", 8, 2, 2076, 422, "button"),
    Seed("SHIFT", 8, 0, 2076, 510, "button"),
    # Encoder push-clicks (route through the button matrix, no indicator LED).
    Seed("Y_ENC", 0, 0, 94, 464, "encoder"),
    Seed("X_ENC", 0, 1, 324, 184, "encoder"),
    Seed("MOD_ENCODER_0", 0, 2, 547, 465, "encoder"),
    Seed("MOD_ENCODER_1", 0, 3, 778, 197, "encoder"),
    Seed("SELECT_ENC", 4, 3, 1066, 327, "encoder"),
    Seed("TEMPO_ENC", 4, 1, 1819, 192, "encoder"),
]


@dataclass
class Bound:
    name: str
    col: int
    row: int
    cx: int
    cy: int
    radius: int


def detect_bound(gray: np.ndarray, seed: Seed) -> Bound:
    h, w = gray.shape
    # Local window large enough to contain even the big gold encoders (~136px).
    win = 120 if seed.kind == "button" else 110
    x0 = max(0, seed.cx - win)
    y0 = max(0, seed.cy - win)
    x1 = min(w, seed.cx + win)
    y1 = min(h, seed.cy + win)
    roi = gray[y0:y1, x0:x1]

    # The interior is light, the border is near-black. Threshold the light area;
    # the border ring separates this control from its neighbours.
    seed_val = int(gray[seed.cy, seed.cx])
    thresh = max(70, seed_val // 2)
    _, light = cv2.threshold(roi, thresh, 255, cv2.THRESH_BINARY)

    # Connected component containing the seed (in ROI coordinates).
    num, labels = cv2.connectedComponents(light)
    sx, sy = seed.cx - x0, seed.cy - y0
    seed_label = labels[sy, sx]
    if seed_label == 0:
        # Seed landed on a dark pixel; pick the nearest light label.
        ys, xs = np.where(light > 0)
        if len(xs) == 0:
            return Bound(seed.name, seed.col, seed.row, seed.cx, seed.cy, 25)
        d = (xs - sx) ** 2 + (ys - sy) ** 2
        i = int(np.argmin(d))
        seed_label = labels[ys[i], xs[i]]

    mask = (labels == seed_label).astype(np.uint8)
    bx, by, bw_, bh_ = cv2.boundingRect(mask)
    cx = x0 + bx + bw_ // 2
    cy = y0 + by + bh_ // 2
    radius = min(bw_, bh_) // 2
    return Bound(seed.name, seed.col, seed.row, cx, cy, radius)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", default="Deluge_Plain.png")
    ap.add_argument("--write-preview", default=None)
    args = ap.parse_args()

    img = cv2.imread(args.image, cv2.IMREAD_COLOR)
    if img is None:
        raise SystemExit(f"could not read image: {args.image}")
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)

    bounds = [detect_bound(gray, s) for s in SEEDS]

    print("/* name, col, row, cx, cy, radius  (auto-detected) */")
    for b in bounds:
        print(f"  {{ \"{b.name}\", {b.col}, {b.row}, {b.cx}, {b.cy}, {b.radius} }},")

    if args.write_preview:
        prev = img.copy()
        for b, s in zip(bounds, SEEDS):
            colour = (0, 255, 0) if s.kind == "button" else (0, 128, 255)
            cv2.circle(prev, (b.cx, b.cy), b.radius, colour, 3)
            cv2.drawMarker(prev, (s.cx, s.cy), (0, 0, 255),
                           cv2.MARKER_CROSS, 16, 2)
            cv2.putText(prev, b.name, (b.cx - b.radius, b.cy - b.radius - 6),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.5, colour, 1, cv2.LINE_AA)
        cv2.imwrite(args.write_preview, prev)
        print(f"\nwrote preview: {args.write_preview}")


if __name__ == "__main__":
    main()
