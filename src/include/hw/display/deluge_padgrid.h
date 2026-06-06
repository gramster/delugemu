/*
 * Deluge RGB pad-grid display (16x8 main grid + 2x8 sidebar, 144 RGB LEDs)
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_DELUGE_PADGRID_H
#define HW_DISPLAY_DELUGE_PADGRID_H

#include "hw/core/sysbus.h"
#include "ui/console.h"
#include "qom/object.h"

#define TYPE_DELUGE_PADGRID "deluge-padgrid"
OBJECT_DECLARE_SIMPLE_TYPE(DelugePadGridState, DELUGE_PADGRID)

/*
 * Grid geometry, mirrored from the firmware (definitions_cxx.hpp): kDisplayWidth
 * 16 main columns plus kSideBarWidth 2 sidebar columns, kDisplayHeight 8 rows.
 */
#define DELUGE_PADGRID_COLS (16 + 2)
#define DELUGE_PADGRID_ROWS 8

/* Each pad is drawn as a filled square of this many pixels, plus a 1px gap. */
#define DELUGE_PADGRID_CELL 12
#define DELUGE_PADGRID_GAP  1

#define DELUGE_PADGRID_WIDTH \
    (DELUGE_PADGRID_COLS * (DELUGE_PADGRID_CELL + DELUGE_PADGRID_GAP) + \
     DELUGE_PADGRID_GAP)
#define DELUGE_PADGRID_HEIGHT \
    (DELUGE_PADGRID_ROWS * (DELUGE_PADGRID_CELL + DELUGE_PADGRID_GAP) + \
     DELUGE_PADGRID_GAP)

struct DelugePadGridState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    QemuConsole *con;

    /* RGB colour per pad, [column][row]; columns include the sidebar. */
    uint8_t rgb[DELUGE_PADGRID_COLS][DELUGE_PADGRID_ROWS][3];

    bool dirty;
};

/*
 * Replace the whole grid with a fresh set of colours (the PIC command decoder
 * calls this after updating its pad state). The source layout matches rgb.
 */
void deluge_padgrid_update(DelugePadGridState *s,
                           const uint8_t rgb[DELUGE_PADGRID_COLS]
                                            [DELUGE_PADGRID_ROWS][3]);

#endif /* HW_DISPLAY_DELUGE_PADGRID_H */
