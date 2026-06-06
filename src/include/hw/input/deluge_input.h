/*
 * Deluge host-input mapping — keyboard to PIC pad/button events
 *
 * Bridges QEMU's host input to the modelled Deluge so the firmware can be
 * driven headlessly, before the graphical hardware skin exists. Host key
 * presses are translated into the same pad/button matrix events the PIC
 * coprocessor would report for the physical surface, and injected through the
 * PIC's input-synthesis API (deluge_pic_pad_event / deluge_pic_button_event).
 *
 * The device carries no MMIO; it exists purely to own a QEMU keyboard input
 * handler and a reference to the PIC it feeds.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_INPUT_DELUGE_INPUT_H
#define HW_INPUT_DELUGE_INPUT_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

struct Chardev;
struct QemuInputHandlerState;

#define TYPE_DELUGE_INPUT "deluge-input"
typedef struct DelugeInputState DelugeInputState;
DECLARE_INSTANCE_CHECKER(DelugeInputState, DELUGE_INPUT, TYPE_DELUGE_INPUT)

struct DelugeInputState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    /* The PIC the synthesised pad/button events are fed into (set by board). */
    struct Chardev *pic;

    /* The registered QEMU keyboard handler. */
    struct QemuInputHandlerState *handler;
};

/* Bind the PIC whose input stream this device drives (board setup). */
void deluge_input_set_pic(DeviceState *dev, struct Chardev *pic);

#endif /* HW_INPUT_DELUGE_INPUT_H */
