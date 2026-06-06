/*
 * Renesas RZ/A1 SCIF (serial communication interface with FIFO)
 *
 * Minimal model: enough surface for firmware to push characters out to the
 * host console. Register-accurate behaviour is filled in as needed.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHAR_RZA1L_SCIF_H
#define HW_CHAR_RZA1L_SCIF_H

#include "hw/core/sysbus.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

#define TYPE_RZA1L_SCIF "rza1l-scif"
OBJECT_DECLARE_SIMPLE_TYPE(RzA1lScifState, RZA1L_SCIF)

/* Size of the MMIO register window for one SCIF channel (placeholder). */
#define RZA1L_SCIF_MMIO_SIZE 0x100

struct RzA1lScifState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    CharFrontend chr;
    qemu_irq irq;
};

#endif /* HW_CHAR_RZA1L_SCIF_H */
