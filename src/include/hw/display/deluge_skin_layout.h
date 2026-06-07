/*
 * Deluge skin layout constants (photo-aligned viewport geometry)
 *
 * Coordinates are in pixels of Deluge_Plain.png.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_DELUGE_SKIN_LAYOUT_H
#define HW_DISPLAY_DELUGE_SKIN_LAYOUT_H

/* Source skin image size used by the calibration script. */
#define DELUGE_SKIN_IMAGE_WIDTH  2256
#define DELUGE_SKIN_IMAGE_HEIGHT 1584

/*
 * OLED viewport for Deluge_Plain.png:
 * - detected aperture is approximately x=1144, y=265, w=310, h=102
 * - render the native 128x48 OLED buffer at 2x => 256x96
 * - centered within the drawn aperture
 */
#define DELUGE_SKIN_OLED_X 1171
#define DELUGE_SKIN_OLED_Y 268
#define DELUGE_SKIN_OLED_W 256
#define DELUGE_SKIN_OLED_H 96

/*
 * Pad-grid overlay geometry for Deluge_Plain.png.
 *
 * Main matrix: 16 columns x 8 rows.
 * Sidebar: 2 columns x 8 rows to the right of the main matrix.
 */
#define DELUGE_SKIN_PAD_MAIN_X0    98
#define DELUGE_SKIN_PAD_MAIN_DX   119
#define DELUGE_SKIN_PAD_ROWS_Y0   632
#define DELUGE_SKIN_PAD_ROWS_DY   119

#define DELUGE_SKIN_PAD_SIDE_X0  2073
#define DELUGE_SKIN_PAD_SIDE_DX   119
#define DELUGE_SKIN_PAD_SIDE_Y0   632
#define DELUGE_SKIN_PAD_SIDE_DY   119

#define DELUGE_SKIN_PAD_SIZE       78
#define DELUGE_SKIN_PAD_ROUND      10

#endif /* HW_DISPLAY_DELUGE_SKIN_LAYOUT_H */
