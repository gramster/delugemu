/*
 * Deluge input PIC bridge
 *
 * The Deluge offloads pad/button/encoder scanning and RGB LED/PWM driving to a
 * dedicated PIC microcontroller that talks to the main SoC over a serial link.
 * This device models that bridge: it surfaces firmware-bound input events and
 * accepts LED updates, rather than emulating the PIC's own instruction set.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_DELUGE_PIC_H
#define HW_MISC_DELUGE_PIC_H

#include "hw/sysbus.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

#define TYPE_DELUGE_PIC "deluge-pic"
OBJECT_DECLARE_SIMPLE_TYPE(DelugePicState, DELUGE_PIC)

struct DelugePicState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    /* Serial link to the SoC (firmware side). */
    CharBackend chr;

    /* IRQ raised when input data is available for the firmware to read. */
    qemu_irq irq;
};

#endif /* HW_MISC_DELUGE_PIC_H */
