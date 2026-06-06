/*
 * Renesas RZ/A1 OSTM (OS Timer) — free-running counter model
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TIMER_RZA1L_OSTM_H
#define HW_TIMER_RZA1L_OSTM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RZA1L_OSTM "rza1l-ostm"
OBJECT_DECLARE_SIMPLE_TYPE(RzA1lOstmState, RZA1L_OSTM)

/* Two channels, each in a 0x400-byte window: OSTM0 @ 0x000, OSTM1 @ 0x400. */
#define RZA1L_OSTM_NUM_CH    2
#define RZA1L_OSTM_MMIO_SIZE 0x800

typedef struct RzA1lOstmChannel {
    uint32_t cmp;      /* OSTMnCMP  — compare/period value           */
    uint8_t  ctl;      /* OSTMnCTL  — operating mode + interrupt en   */
    bool     enabled;  /* OSTMnTE   — counter running                 */
    int64_t  start_ns; /* virtual-clock time the counter was started  */
    uint32_t frozen;   /* counter value latched while stopped         */
} RzA1lOstmChannel;

struct RzA1lOstmState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    RzA1lOstmChannel ch[RZA1L_OSTM_NUM_CH];
};

#endif /* HW_TIMER_RZA1L_OSTM_H */
