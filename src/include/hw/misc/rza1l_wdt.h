/*
 * Renesas RZ/A1 WDT (watchdog timer) — minimal model
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RZA1L_WDT_H
#define HW_MISC_RZA1L_WDT_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RZA1L_WDT "rza1l-wdt"
OBJECT_DECLARE_SIMPLE_TYPE(RzA1lWdtState, RZA1L_WDT)

#define RZA1L_WDT_MMIO_SIZE 0x10

struct RzA1lWdtState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint16_t wtcsr;
    uint16_t wtcnt;
    uint16_t wrcsr;
};

#endif /* HW_MISC_RZA1L_WDT_H */
