/*
 * Renesas RZ/A1 RSPI (Renesas Serial Peripheral Interface) — minimal model
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SSI_RZA1L_RSPI_H
#define HW_SSI_RZA1L_RSPI_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

struct DelugeOledState;

#define TYPE_RZA1L_RSPI "rza1l-rspi"
OBJECT_DECLARE_SIMPLE_TYPE(RzA1lRspiState, RZA1L_RSPI)

/* One RSPI channel occupies 0x24 bytes; map a padded window. */
#define RZA1L_RSPI_MMIO_SIZE 0x100

struct RzA1lRspiState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;

    /*
     * OLED panel this channel feeds. Data-register writes (from the CPU for
     * init commands, or from DMA for the framebuffer) are forwarded here; the
     * OLED itself ignores them unless its chip select is asserted, so CV/gate
     * traffic on the same channel is harmless.
     */
    struct DelugeOledState *oled;

    /* Shadow of the writable configuration registers for read-back. */
    uint8_t regs[RZA1L_RSPI_MMIO_SIZE];
};

/* Bind the OLED panel this RSPI channel drives (called by the board). */
void rza1l_rspi_set_oled(RzA1lRspiState *s, struct DelugeOledState *oled);

#endif /* HW_SSI_RZA1L_RSPI_H */
