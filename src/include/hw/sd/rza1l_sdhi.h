/*
 * Renesas RZ/A1L SDHI (SD Host Interface) controller model
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SD_RZA1L_SDHI_H
#define HW_SD_RZA1L_SDHI_H

#include "hw/core/sysbus.h"
#include "hw/sd/sd.h"
#include "qom/object.h"

#define TYPE_RZA1L_SDHI "rza1l-sdhi"
OBJECT_DECLARE_SIMPLE_TYPE(RzA1lSdhiState, RZA1L_SDHI)

/* MMIO window: registers span SD_CMD (0x00) .. EXT_SWAP (0xF0). */
#define RZA1L_SDHI_MMIO_SIZE 0x100

struct RzA1lSdhiState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    SDBus sdbus;
    qemu_irq irq;

    /* Command/argument registers. */
    uint16_t cmd;
    uint16_t arg0;
    uint16_t arg1;
    uint16_t stop;
    uint16_t seccnt;
    uint16_t size;

    /* Response registers (SD_RESP0..SD_RESP7). */
    uint16_t resp[8];

    /* Status and interrupt-mask registers. */
    uint16_t info1;
    uint16_t info2;
    uint16_t info1_mask;
    uint16_t info2_mask;

    /* Misc shadowed registers. */
    uint16_t clk_ctrl;
    uint16_t option;
    uint16_t err_sts1;
    uint16_t err_sts2;
    uint16_t sdio_mode;
    uint16_t sdio_info1;
    uint16_t sdio_info1_mask;
    uint16_t cc_ext_mode;
    uint16_t soft_rst;
    uint16_t ext_swap;

    /* In-flight data transfer state. */
    uint8_t  data_dir;   /* 0 = none, 1 = read (card->host), 2 = write */
    uint32_t blocklen;   /* bytes per block (from SD_SIZE) */
    uint32_t blockcnt;   /* blocks still to transfer (from SD_SECCNT) */
    uint32_t datacnt;    /* bytes left in the current block */
};

#endif /* HW_SD_RZA1L_SDHI_H */
