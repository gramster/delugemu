#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Detect the OLED aperture in the Deluge skin image and compute scale options so
an OLED source buffer (128x48) maps cleanly.

Typical use:
  python3 scripts/skin_calibrate_oled.py \
      --image Synthstrom_Deluge_Skin.png --target-mult 2 --write-preview /tmp/skin_preview.png

This prints:
  - detected OLED rectangle in image pixels,
  - non-uniform global image scale to make OLED exactly (128*m)x(48*m),
  - uniform global scale options (fit-width / fit-height / mean) and resulting
    OLED pixel error.

If --write-preview is set, it writes a copy with the detected rectangle drawn.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np

OLED_W = 128
OLED_H = 48


@dataclass
class OledRect:
    x: int
    y: int
    w: int
    h: int
    score: float


def detect_oled_rect(img: np.ndarray) -> OledRect:
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    h, w = gray.shape

    # Search the top-center where the Deluge OLED physically lives.
    x0 = int(w * 0.35)
    x1 = int(w * 0.68)
    y0 = int(h * 0.10)
    y1 = int(h * 0.33)
    roi = gray[y0:y1, x0:x1]

    best: OledRect | None = None

    for t in [25, 30, 35, 40, 45, 50, 55, 60, 65]:
        _, bw = cv2.threshold(roi, t, 255, cv2.THRESH_BINARY_INV)
        bw = cv2.morphologyEx(bw, cv2.MORPH_OPEN, np.ones((3, 3), np.uint8), iterations=1)
        bw = cv2.morphologyEx(bw, cv2.MORPH_CLOSE, np.ones((5, 5), np.uint8), iterations=1)

        contours, _ = cv2.findContours(bw, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)

        for c in contours:
            area = cv2.contourArea(c)
            if area < 1200:
                continue

            x, y, ww, hh = cv2.boundingRect(c)
            if ww < 120 or hh < 40:
                continue

            aspect = ww / float(hh)
            # Real panel is 128/48 = 2.667, but image perspective/bezel may stretch.
            if not (2.2 <= aspect <= 3.6):
                continue

            fill = area / max(ww * hh, 1)
            if fill < 0.80:
                continue

            gx, gy = x0 + x, y0 + y
            cx, cy = gx + ww / 2.0, gy + hh / 2.0
            center_pen = abs(cx - w / 2.0) * 0.02 + abs(cy - h * 0.19) * 0.03
            aspect_pen = abs(aspect - (OLED_W / OLED_H)) * 30.0
            score = fill * 100.0 - center_pen - aspect_pen

            cand = OledRect(gx, gy, ww, hh, score)
            if best is None or cand.score > best.score:
                best = cand

    if best is None:
        raise RuntimeError("Could not detect an OLED rectangle candidate")

    return best


def print_scale_report(rect: OledRect, target_mult: float) -> None:
    target_w = OLED_W * target_mult
    target_h = OLED_H * target_mult

    nonuniform_sx = target_w / rect.w
    nonuniform_sy = target_h / rect.h

    uniform_fit_width = target_w / rect.w
    uniform_fit_height = target_h / rect.h
    uniform_mean = (uniform_fit_width + uniform_fit_height) / 2.0

    print(f"Detected OLED rectangle: x={rect.x}, y={rect.y}, w={rect.w}, h={rect.h}")
    print(f"Source aspect={rect.w/rect.h:.6f}, OLED aspect={OLED_W/OLED_H:.6f}")
    print(f"Target active size for mult={target_mult:g}: {target_w:.3f} x {target_h:.3f}")
    print()
    print("Non-uniform global scale (exact OLED match):")
    print(f"  sx={nonuniform_sx:.6f}, sy={nonuniform_sy:.6f}")
    print("  Note: this warps the full skin image.")
    print()
    print("Uniform global scale choices (preserve image proportions):")
    for name, s in [
        ("fit-width", uniform_fit_width),
        ("fit-height", uniform_fit_height),
        ("mean", uniform_mean),
    ]:
        ow = rect.w * s
        oh = rect.h * s
        err_w = ow - target_w
        err_h = oh - target_h
        print(
            f"  {name:10s} s={s:.6f} -> OLED {ow:.3f} x {oh:.3f} "
            f"(err {err_w:+.3f}, {err_h:+.3f})"
        )


def centered_target_rect(rect: OledRect, target_mult: float) -> OledRect:
    target_w = int(round(OLED_W * target_mult))
    target_h = int(round(OLED_H * target_mult))

    cx = rect.x + rect.w / 2.0
    cy = rect.y + rect.h / 2.0

    x = int(round(cx - target_w / 2.0))
    y = int(round(cy - target_h / 2.0))

    return OledRect(x=x, y=y, w=target_w, h=target_h, score=0.0)


def print_centered_target_report(rect: OledRect, target_mult: float) -> None:
    target = centered_target_rect(rect, target_mult)

    left_overhang = rect.x - target.x
    right_overhang = (target.x + target.w) - (rect.x + rect.w)
    top_overhang = rect.y - target.y
    bottom_overhang = (target.y + target.h) - (rect.y + rect.h)

    print()
    print("Centered fixed viewport (no global image distortion):")
    print(
        f"  target x={target.x}, y={target.y}, w={target.w}, h={target.h} "
        f"(centered on detected OLED)"
    )
    print(
        "  overhang vs detected aperture "
        f"L={left_overhang:+d}px R={right_overhang:+d}px "
        f"T={top_overhang:+d}px B={bottom_overhang:+d}px"
    )


def write_preview(img: np.ndarray, rect: OledRect, out_path: Path) -> None:
    preview = img.copy()
    cv2.rectangle(preview, (rect.x, rect.y), (rect.x + rect.w, rect.y + rect.h), (0, 255, 255), 2)
    cv2.putText(
        preview,
        f"OLED {rect.x},{rect.y} {rect.w}x{rect.h}",
        (rect.x, max(20, rect.y - 8)),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.6,
        (0, 255, 255),
        2,
        cv2.LINE_AA,
    )
    out_path.parent.mkdir(parents=True, exist_ok=True)
    cv2.imwrite(str(out_path), preview)


def write_preview_with_target(
    img: np.ndarray,
    rect: OledRect,
    target_mult: float,
    out_path: Path,
) -> None:
    preview = img.copy()
    target = centered_target_rect(rect, target_mult)

    # Detected bezel/aperture from photo.
    cv2.rectangle(preview, (rect.x, rect.y), (rect.x + rect.w, rect.y + rect.h), (0, 255, 255), 2)
    cv2.putText(
        preview,
        f"detected {rect.x},{rect.y} {rect.w}x{rect.h}",
        (rect.x, max(20, rect.y - 10)),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.55,
        (0, 255, 255),
        2,
        cv2.LINE_AA,
    )

    # Proposed centered fixed OLED viewport.
    cv2.rectangle(
        preview,
        (target.x, target.y),
        (target.x + target.w, target.y + target.h),
        (0, 180, 255),
        2,
    )
    cv2.putText(
        preview,
        f"target {target.x},{target.y} {target.w}x{target.h}",
        (target.x, target.y + target.h + 22),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.55,
        (0, 180, 255),
        2,
        cv2.LINE_AA,
    )

    out_path.parent.mkdir(parents=True, exist_ok=True)
    cv2.imwrite(str(out_path), preview)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--target-mult", type=float, default=2.0)
    parser.add_argument("--write-preview", type=Path)
    args = parser.parse_args()

    img = cv2.imread(str(args.image))
    if img is None:
        raise SystemExit(f"Could not read image: {args.image}")

    rect = detect_oled_rect(img)
    print_scale_report(rect, args.target_mult)
    print_centered_target_report(rect, args.target_mult)

    if args.write_preview:
        write_preview_with_target(img, rect, args.target_mult, args.write_preview)
        print(f"Preview written: {args.write_preview}")


if __name__ == "__main__":
    main()
