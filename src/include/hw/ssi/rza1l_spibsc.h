/*
 * Renesas RZ/A1 SPIBSC (SPI multi-I/O bus controller) — minimal model
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SSI_RZA1L_SPIBSC_H
#define HW_SSI_RZA1L_SPIBSC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RZA1L_SPIBSC "rza1l-spibsc"
OBJECT_DECLARE_SIMPLE_TYPE(RzA1lSpibscState, RZA1L_SPIBSC)

#define RZA1L_SPIBSC_MMIO_SIZE 0x100

struct RzA1lSpibscState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;

    /* Shadow of the register file (only the documented offsets are used). */
    uint32_t regs[RZA1L_SPIBSC_MMIO_SIZE / 4];

    /* Whether the SSL (chip-select) line is currently held asserted. */
    bool ssl_asserted;
};

#endif /* HW_SSI_RZA1L_SPIBSC_H */
