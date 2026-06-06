/*
 * Renesas RZ/A1 CPG (clock pulse generator) + module standby — minimal model
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RZA1L_CPG_H
#define HW_MISC_RZA1L_CPG_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RZA1L_CPG "rza1l-cpg"
OBJECT_DECLARE_SIMPLE_TYPE(RzA1lCpgState, RZA1L_CPG)

/*
 * The st_cpg block (clock-frequency control plus the STBCR module-standby
 * registers) lives within the first ~0x100 bytes; 0x1000 covers it with room
 * to spare for the software-reset registers the firmware may poke.
 */
#define RZA1L_CPG_MMIO_SIZE 0x1000

struct RzA1lCpgState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint8_t regs[RZA1L_CPG_MMIO_SIZE];
};

#endif /* HW_MISC_RZA1L_CPG_H */
