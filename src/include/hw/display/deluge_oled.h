/*
 * Deluge OLED display (128x48 monochrome)
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_DELUGE_OLED_H
#define HW_DISPLAY_DELUGE_OLED_H

#include "hw/core/sysbus.h"
#include "ui/console.h"
#include "qom/object.h"

#define TYPE_DELUGE_OLED "deluge-oled"
OBJECT_DECLARE_SIMPLE_TYPE(DelugeOledState, DELUGE_OLED)

#define DELUGE_OLED_WIDTH  128
#define DELUGE_OLED_HEIGHT 48

struct DelugeOledState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    QemuConsole *con;

    /* 1 bpp framebuffer, one byte per pixel for simplicity in the model. */
    uint8_t framebuffer[DELUGE_OLED_WIDTH * DELUGE_OLED_HEIGHT];
    bool dirty;
};

#endif /* HW_DISPLAY_DELUGE_OLED_H */
