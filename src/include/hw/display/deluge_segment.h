/*
 * Deluge 7-segment display array
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_DELUGE_SEGMENT_H
#define HW_DISPLAY_DELUGE_SEGMENT_H

#include "hw/core/sysbus.h"
#include "ui/console.h"
#include "qom/object.h"

#define TYPE_DELUGE_SEGMENT "deluge-segment"
OBJECT_DECLARE_SIMPLE_TYPE(DelugeSegmentState, DELUGE_SEGMENT)

/* Number of 7-segment digits in the array. */
#define DELUGE_SEGMENT_DIGITS 4

/* Per-digit drawing geometry (pixels). */
#define DELUGE_SEGMENT_THICK   4   /* segment thickness                       */
#define DELUGE_SEGMENT_DW     24   /* digit body width                        */
#define DELUGE_SEGMENT_VLEN   22   /* vertical segment length                 */
#define DELUGE_SEGMENT_DH     (2 * DELUGE_SEGMENT_VLEN + 3 * DELUGE_SEGMENT_THICK)
#define DELUGE_SEGMENT_PITCH  (DELUGE_SEGMENT_DW + 12) /* digit-to-digit step  */
#define DELUGE_SEGMENT_MARGIN 6

#define DELUGE_SEGMENT_WIDTH \
    (DELUGE_SEGMENT_DIGITS * DELUGE_SEGMENT_PITCH + 2 * DELUGE_SEGMENT_MARGIN)
#define DELUGE_SEGMENT_HEIGHT (DELUGE_SEGMENT_DH + 2 * DELUGE_SEGMENT_MARGIN)

struct DelugeSegmentState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    QemuConsole *con;

    /*
     * Raw segment bitmask per digit, as sent by the firmware (PIC command 224).
     * bit0=g(middle) bit1=f bit2=e bit3=d bit4=c bit5=b bit6=a bit7=decimal pt.
     */
    uint8_t digits[DELUGE_SEGMENT_DIGITS];

    bool dirty;
};

/*
 * Replace all four digit bitmasks (the PIC command decoder calls this when it
 * receives a seven-segment update). The source layout matches digits.
 */
void deluge_segment_update(DelugeSegmentState *s,
                           const uint8_t digits[DELUGE_SEGMENT_DIGITS]);

#endif /* HW_DISPLAY_DELUGE_SEGMENT_H */
