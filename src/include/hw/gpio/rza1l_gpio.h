/*
 * Renesas RZ/A1 GPIO (general-purpose I/O ports) — minimal model
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_GPIO_RZA1L_GPIO_H
#define HW_GPIO_RZA1L_GPIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RZA1L_GPIO "rza1l-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(RzA1lGpioState, RZA1L_GPIO)

/*
 * The st_gpio register block spans P1 (offset 0) through the PIPC/port-buffer
 * registers near 0x4200. 0x4F00 covers the whole structure.
 */
#define RZA1L_GPIO_MMIO_SIZE 0x4F00

struct RzA1lGpioState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint8_t regs[RZA1L_GPIO_MMIO_SIZE];
};

#endif /* HW_GPIO_RZA1L_GPIO_H */
