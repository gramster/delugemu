/*
 * Deluge 7-segment display array
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_DELUGE_SEGMENT_H
#define HW_DISPLAY_DELUGE_SEGMENT_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_DELUGE_SEGMENT "deluge-segment"
OBJECT_DECLARE_SIMPLE_TYPE(DelugeSegmentState, DELUGE_SEGMENT)

/* Number of 7-segment digits in the array (to be confirmed against hardware). */
#define DELUGE_SEGMENT_DIGITS 4

struct DelugeSegmentState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    /* Raw segment bits per digit (bit0=a .. bit6=g, bit7=decimal point). */
    uint8_t digits[DELUGE_SEGMENT_DIGITS];
};

#endif /* HW_DISPLAY_DELUGE_SEGMENT_H */
