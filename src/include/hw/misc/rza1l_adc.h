/*
 * Renesas RZ/A1 ADC (S12AD) — minimal battery-sense stub
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RZA1L_ADC_H
#define HW_MISC_RZA1L_ADC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RZA1L_ADC "rza1l-adc"
OBJECT_DECLARE_SIMPLE_TYPE(RzA1lAdcState, RZA1L_ADC)

#define RZA1L_ADC_MMIO_SIZE 0x100

struct RzA1lAdcState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint16_t adcsr;
};

#endif /* HW_MISC_RZA1L_ADC_H */
