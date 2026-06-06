/*
 * Renesas RZ/A1 BSC (bus state controller) — minimal model
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RZA1L_BSC_H
#define HW_MISC_RZA1L_BSC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RZA1L_BSC "rza1l-bsc"
OBJECT_DECLARE_SIMPLE_TYPE(RzA1lBscState, RZA1L_BSC)

/*
 * The register file (CMNCR..TOENR) sits in the first ~0x100 bytes, but the
 * firmware also writes the SDRAM mode register through the controller's
 * mode-set windows at offsets 0x1040 (CS2) and 0x2040 (CS3); 0x3000 covers
 * the register file plus both windows.
 */
#define RZA1L_BSC_MMIO_SIZE 0x3000

struct RzA1lBscState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint8_t regs[RZA1L_BSC_MMIO_SIZE];
};

#endif /* HW_MISC_RZA1L_BSC_H */
