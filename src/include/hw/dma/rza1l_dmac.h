/*
 * Renesas RZ/A1 DMAC (Direct Memory Access Controller) — minimal model
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DMA_RZA1L_DMAC_H
#define HW_DMA_RZA1L_DMAC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RZA1L_DMAC "rza1l-dmac"
OBJECT_DECLARE_SIMPLE_TYPE(RzA1lDmacState, RZA1L_DMAC)

/* 16 channels split into two groups of eight. */
#define RZA1L_DMAC_NUM_CH 16

/* MMIO window covers both channel groups and their common registers. */
#define RZA1L_DMAC_MMIO_SIZE 0x800

typedef struct RzA1lDmacChannel {
    uint32_t n0sa;   /* next-0 source address */
    uint32_t n0da;   /* next-0 destination address */
    uint32_t n0tb;   /* next-0 transfer byte count */
    uint32_t n1sa;
    uint32_t n1da;
    uint32_t n1tb;
    uint32_t crsa;   /* current source address (read-only) */
    uint32_t crda;   /* current destination address (read-only) */
    uint32_t crtb;   /* current transfer byte count (read-only) */
    uint32_t chstat; /* channel status (read-only) */
    uint32_t chcfg;  /* channel configuration */
    uint32_t chitvl;
    uint32_t chext;
    uint32_t nxla;
    uint32_t crla;
} RzA1lDmacChannel;

struct RzA1lDmacState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;

    RzA1lDmacChannel ch[RZA1L_DMAC_NUM_CH];

    /* Group common control registers (DCTRL_0_7 / DCTRL_8_15), shadowed. */
    uint32_t dctrl[2];
};

#endif /* HW_DMA_RZA1L_DMAC_H */
