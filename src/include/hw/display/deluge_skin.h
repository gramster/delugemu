/*
 * Deluge composited front-panel skin view
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_DELUGE_SKIN_H
#define HW_DISPLAY_DELUGE_SKIN_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "ui/console.h"

#define TYPE_DELUGE_SKIN "deluge-skin"
OBJECT_DECLARE_SIMPLE_TYPE(DelugeSkinState, DELUGE_SKIN)

struct DelugeOledState;
struct DelugePadGridState;

struct DelugeSkinState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    QemuConsole *con;

    struct DelugeOledState *oled;
    struct DelugePadGridState *padgrid;

    /* PIC backend, read for indicator-LED and gold-knob state (may be NULL). */
    struct Chardev *pic;

    /* GPIO device, read for firmware-driven panel LEDs (Synced). May be NULL. */
    DeviceState *gpio;

    /* RGBA skin background, converted to 32-bit ARGB pixels. */
    uint32_t *bg_argb;
    bool bg_loaded;

    /* Optional image path; defaults to Deluge_Plain.png in cwd. */
    char *image_path;

    bool dirty;
    QEMUTimer *refresh_timer;
};

void deluge_skin_set_oled(DeviceState *dev, struct DelugeOledState *oled);
void deluge_skin_set_padgrid(DeviceState *dev,
                             struct DelugePadGridState *padgrid);
void deluge_skin_set_pic(DeviceState *dev, struct Chardev *pic);
void deluge_skin_set_gpio(DeviceState *dev, DeviceState *gpio);

#endif /* HW_DISPLAY_DELUGE_SKIN_H */
