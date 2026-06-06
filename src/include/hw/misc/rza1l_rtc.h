/*
 * Renesas RZ/A1 RTC (real-time clock) — minimal stub
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RZA1L_RTC_H
#define HW_MISC_RZA1L_RTC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RZA1L_RTC "rza1l-rtc"
OBJECT_DECLARE_SIMPLE_TYPE(RzA1lRtcState, RZA1L_RTC)

#define RZA1L_RTC_MMIO_SIZE 0x40

struct RzA1lRtcState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint8_t regs[RZA1L_RTC_MMIO_SIZE];
};

#endif /* HW_MISC_RZA1L_RTC_H */
