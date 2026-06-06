/*
 * Renesas RZ/A1 MTU2 (Multi-Function Timer Pulse Unit 2) — free-running model
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TIMER_RZA1L_MTU2_H
#define HW_TIMER_RZA1L_MTU2_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RZA1L_MTU2 "rza1l-mtu2"
OBJECT_DECLARE_SIMPLE_TYPE(RzA1lMtu2State, RZA1L_MTU2)

#define RZA1L_MTU2_MMIO_SIZE 0x400

/* Five timer channels plus the synchronous counter (TCNTS). */
#define RZA1L_MTU2_NUM_TCNT 6

typedef struct RzA1lMtu2Counter {
    int64_t reset_ns;   /* virtual-clock time the counter was last set */
    uint16_t reset_val; /* value written at reset_ns */
} RzA1lMtu2Counter;

struct RzA1lMtu2State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;

    RzA1lMtu2Counter tcnt[RZA1L_MTU2_NUM_TCNT];
    uint8_t regs[RZA1L_MTU2_MMIO_SIZE];
};

#endif /* HW_TIMER_RZA1L_MTU2_H */
