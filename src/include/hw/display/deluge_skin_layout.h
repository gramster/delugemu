/*
 * Deluge skin layout constants (photo-aligned viewport geometry)
 *
 * Coordinates are in pixels of Synthstrom_Deluge_Skin.png.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_DELUGE_SKIN_LAYOUT_H
#define HW_DISPLAY_DELUGE_SKIN_LAYOUT_H

/* Source skin image size used by the calibration script. */
#define DELUGE_SKIN_IMAGE_WIDTH  1855
#define DELUGE_SKIN_IMAGE_HEIGHT 1307

/*
 * OLED viewport chosen from calibration:
 * - 2x scale of the native 128x48 OLED buffer => 256x96
 * - centered horizontally on detected aperture
 * - top-biased vertical overhang split (75% top, 25% bottom)
 *   relative to detected aperture (x=921, y=220, w=249, h=78), resulting in
 *   overhangs L=3, R=4, T=13, B=5.
 */
#define DELUGE_SKIN_OLED_X 918
#define DELUGE_SKIN_OLED_Y 207
#define DELUGE_SKIN_OLED_W 256
#define DELUGE_SKIN_OLED_H 96

/*
 * Pad-grid overlay geometry (first-pass calibration from skin photo).
 *
 * Main matrix: 16 columns x 8 rows.
 * Sidebar: 2 columns x 8 rows to the right of the main matrix.
 */
#define DELUGE_SKIN_PAD_MAIN_X0    64
#define DELUGE_SKIN_PAD_MAIN_DX    98
#define DELUGE_SKIN_PAD_ROWS_Y0   636
#define DELUGE_SKIN_PAD_ROWS_DY    96

#define DELUGE_SKIN_PAD_SIDE_X0  1693
#define DELUGE_SKIN_PAD_SIDE_DX    98
#define DELUGE_SKIN_PAD_SIDE_Y0   531
#define DELUGE_SKIN_PAD_SIDE_DY    99

#define DELUGE_SKIN_PAD_SIZE       64
#define DELUGE_SKIN_PAD_ROUND      10

#endif /* HW_DISPLAY_DELUGE_SKIN_LAYOUT_H */
