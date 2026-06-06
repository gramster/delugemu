/*
 * Renesas RZ/A1L SDHI (SD Host Interface) controller model
 *
 * Models the SDHI host port the Synthstrom Deluge uses (SD_PORT = 1, IP1 at
 * 0xE804E800) well enough for the firmware's command/response/data flow. The
 * controller owns an SDBus to which a QEMU sd-card can be attached.
 *
 * The firmware driver (Renesas SD library + Deluge glue in
 * deluge/drivers/sd/sd.c) is configured for hardware-interrupt mode
 * (SDCFG_HWINT) and DMA data transfer (SDCFG_TRNS_DMA). After issuing a command
 * it waits on the SDHI interrupt; its ISR reads SD_INFO1/SD_INFO2. Data is moved
 * by a DMAC channel reading/writing the SD data FIFO. Because the DMAC model
 * accesses its source/destination through the system address space, the same
 * FIFO serves both DMA and PIO accesses.
 *
 * In 64-byte DMA mode (SDCFG_TRANS_DMA_64) the firmware points the DMAC at the
 * register base rather than at SD_BUF0, so the controller also exposes the data
 * FIFO through the low 64-byte window (offsets 0x00..0x3F) while a DMA transfer
 * is in progress; see the DMA_FIFO_WINDOW handling below.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/sd/sd.h"
#include "migration/vmstate.h"
#include "hw/sd/rza1l_sdhi.h"

/* Register offsets (16-bit registers, natural byte offsets). */
#define SD_CMD          0x00
#define SD_ARG0         0x04
#define SD_ARG1         0x06
#define SD_STOP         0x08
#define SD_SECCNT       0x0A
#define SD_RESP0        0x0C
#define SD_RESP7        0x1A
#define SD_INFO1        0x1C
#define SD_INFO2        0x1E
#define SD_INFO1_MASK   0x20
#define SD_INFO2_MASK   0x22
#define SD_CLK_CTRL     0x24
#define SD_SIZE         0x26
#define SD_OPTION       0x28
#define SD_ERR_STS1     0x2C
#define SD_ERR_STS2     0x2E
#define SD_BUF0         0x30
#define SDIO_MODE       0x34
#define SDIO_INFO1      0x36
#define SDIO_INFO1_MASK 0x38
#define CC_EXT_MODE     0xD8
#define SOFT_RST        0xE0
#define SD_VERSION      0xE2
#define EXT_SWAP        0xF0

/* SD_INFO1 bits. */
#define INFO1_RESP       0x0001  /* response/command end */
#define INFO1_DATA_TRNS  0x0004  /* data-transfer/access end */
#define INFO1_REM_CD     0x0008  /* CD-pin removal event */
#define INFO1_INS_CD     0x0010  /* CD-pin insertion event */
#define INFO1_CD_LEVEL   0x0020  /* CD-pin current level (1 = card present) */
#define INFO1_REM_DAT3   0x0100  /* DAT3 removal event */
#define INFO1_INS_DAT3   0x0200  /* DAT3 insertion event */
#define INFO1_DAT3_LEVEL 0x0400  /* DAT3 current level (1 = card present) */
#define INFO1_DET_CD     (INFO1_REM_CD | INFO1_INS_CD)
#define INFO1_DET_DAT3   (INFO1_REM_DAT3 | INFO1_INS_DAT3)
#define INFO1_IRQ_BITS   (INFO1_RESP | INFO1_DATA_TRNS | INFO1_DET_CD | \
                          INFO1_DET_DAT3)

/* SD_INFO2 bits. */
#define INFO2_ERR0       0x0001  /* CMD error */
#define INFO2_ERR6       0x0040  /* response timeout */
#define INFO2_RE         0x0100  /* read buffer ready (BRE) */
#define INFO2_WE         0x0200  /* write buffer ready (BWE) */
#define INFO2_SCLKDIVEN  0x2000  /* SD bus idle/clock ready */
#define INFO2_CBSY       0x4000  /* command busy */
#define INFO2_ILA        0x8000  /* illegal access */
#define INFO2_ERR_MASK   0x807f  /* all error bits + ILA */
#define INFO2_IRQ_BITS   (INFO2_RE | INFO2_WE | INFO2_ERR_MASK)

#define SD_CMD_INDEX_MASK 0x003f

/* CC_EXT_MODE bits. */
#define CC_EXT_MODE_DMASDRW 0x0002  /* SD buffer accesses are driven by DMA */

/*
 * Size of the 64-byte DMA data window. In DMA mode (SDCFG_TRANS_DMA_64) the
 * firmware points the DMAC source/destination at the register base rather than
 * at SD_BUF0, so the SDHI exposes the data FIFO through the low 64-byte window
 * (offsets 0x00..0x3F) instead of only at SD_BUF0. While a DMA-driven data
 * transfer is in progress, accesses in that window stream the FIFO.
 */
#define DMA_FIFO_WINDOW 0x40

/*
 * Standard SD/MMC commands that complete without any response. For these a
 * zero-length result from the card is normal and must not be reported as a
 * response timeout. CMD0 GO_IDLE_STATE, CMD4 SET_DSR, CMD15 GO_INACTIVE_STATE.
 */
static bool rza1l_sdhi_cmd_has_no_response(uint8_t cmd)
{
    return cmd == 0 || cmd == 4 || cmd == 15;
}

static void rza1l_sdhi_update_irq(RzA1lSdhiState *s)
{
    /*
     * The hardware mask registers use inverted logic: a register bit of 0 means
     * the corresponding interrupt is enabled. Raise the line whenever an enabled
     * event bit is set (SCLKDIVEN/CBSY are status, not interrupt sources).
     */
    uint16_t i1 = s->info1 & (uint16_t)~s->info1_mask & INFO1_IRQ_BITS;
    uint16_t i2 = s->info2 & (uint16_t)~s->info2_mask & INFO2_IRQ_BITS;

    qemu_set_irq(s->irq, (i1 || i2) ? 1 : 0);
}

static void rza1l_sdhi_command(RzA1lSdhiState *s, uint16_t cmdreg)
{
    SDRequest req;
    uint8_t rsp[16];
    size_t rlen;

    req.cmd = cmdreg & SD_CMD_INDEX_MASK;
    req.arg = ((uint32_t)s->arg1 << 16) | s->arg0;
    req.crc = 0;

    /* Clear status from any previous command. */
    s->info1 &= ~(INFO1_RESP | INFO1_DATA_TRNS);
    s->info2 &= ~(INFO2_ERR_MASK | INFO2_RE | INFO2_WE);
    memset(s->resp, 0, sizeof(s->resp));
    s->data_dir = 0;
    s->datacnt = 0;
    s->blockcnt = 0;

    rlen = sdbus_do_command(&s->sdbus, &req, rsp, sizeof(rsp));

    if (rlen == 0) {
        /*
         * No response. For commands that genuinely return none this is the
         * expected outcome; otherwise it means the card did not answer, which
         * the controller reports as a response timeout.
         */
        if (!rza1l_sdhi_cmd_has_no_response(req.cmd)) {
            s->info2 |= INFO2_ERR6;
        }
    } else if (rlen == 4) {
        /* Short response (R1/R1b/R3/R6/R7): RESP1:RESP0 = 32-bit content. */
        uint32_t v = ldl_be_p(&rsp[0]);
        s->resp[0] = v & 0xffff;
        s->resp[1] = (v >> 16) & 0xffff;
    } else if (rlen == 16) {
        /*
         * Long response (R2, CID/CSD). The SDHI exposes the 136-bit response
         * frame bits [135:8] across SD_RESP7..SD_RESP0; the firmware reads
         * cid/csd[k] = SD_RESP(7-k) and expects, e.g., CSD_STRUCTURE at
         * csd[0][7:6] (= frame[127:126] = CSD[127:126]). QEMU returns the raw
         * 16-byte CID/CSD (rsp[0] = bits [127:120], rsp[15] = CRC). Prepend the
         * 8-bit frame header, drop the trailing CRC byte, then slice into the
         * eight response halfwords MSB first.
         */
        uint8_t frame[16];
        int k;

        frame[0] = 0x3f;                 /* R2 frame header (all-ones field) */
        memcpy(&frame[1], rsp, 15);      /* CSD/CID[127:8]; drop CRC (rsp[15]) */
        for (k = 0; k < 8; k++) {
            s->resp[7 - k] = ((uint16_t)frame[2 * k] << 8) | frame[2 * k + 1];
        }
    }

    /* Response phase complete. */
    s->info1 |= INFO1_RESP;

    /*
     * Determine whether a data phase follows from the card's resulting state,
     * which avoids decoding the SDHI command flags (and the ACMD13/CMD13 index
     * ambiguity). The transfer length comes from SD_SIZE/SD_SECCNT.
     */
    if (sdbus_data_ready(&s->sdbus)) {
        s->data_dir = 1;
        s->blocklen = s->size ? s->size : 512;
        s->blockcnt = s->seccnt ? s->seccnt : 1;
        s->datacnt = s->blocklen;
        s->info2 |= INFO2_RE;
    } else if (sdbus_receive_ready(&s->sdbus)) {
        s->data_dir = 2;
        s->blocklen = s->size ? s->size : 512;
        s->blockcnt = s->seccnt ? s->seccnt : 1;
        s->datacnt = s->blocklen;
        s->info2 |= INFO2_WE;
    }

    rza1l_sdhi_update_irq(s);
}

static void rza1l_sdhi_data_block_done(RzA1lSdhiState *s, uint16_t ready_bit)
{
    if (s->blockcnt) {
        s->blockcnt--;
    }
    if (s->blockcnt > 0) {
        /* More blocks: keep the buffer-ready flag set for the next block. */
        s->datacnt = s->blocklen;
    } else {
        s->info2 &= ~ready_bit;
        s->info1 |= INFO1_DATA_TRNS;
        s->data_dir = 0;
    }
    rza1l_sdhi_update_irq(s);
}

static uint32_t rza1l_sdhi_read_buf(RzA1lSdhiState *s, unsigned size)
{
    uint32_t val = 0;
    unsigned i;

    if (s->data_dir != 1) {
        return 0;
    }
    for (i = 0; i < size; i++) {
        uint8_t b = sdbus_read_byte(&s->sdbus);
        val |= (uint32_t)b << (8 * i);
        if (s->datacnt) {
            s->datacnt--;
        }
    }
    if (s->datacnt == 0) {
        rza1l_sdhi_data_block_done(s, INFO2_RE);
    }
    return val;
}

static void rza1l_sdhi_write_buf(RzA1lSdhiState *s, uint32_t val, unsigned size)
{
    unsigned i;

    if (s->data_dir != 2) {
        return;
    }
    for (i = 0; i < size; i++) {
        sdbus_write_byte(&s->sdbus, (val >> (8 * i)) & 0xff);
        if (s->datacnt) {
            s->datacnt--;
        }
    }
    if (s->datacnt == 0) {
        rza1l_sdhi_data_block_done(s, INFO2_WE);
    }
}

static uint64_t rza1l_sdhi_read(void *opaque, hwaddr offset, unsigned size)
{
    RzA1lSdhiState *s = opaque;

    /*
     * During a DMA-driven read the DMAC pulls bytes from the register base
     * (the 64-byte DMA window), not from SD_BUF0; route those accesses to the
     * data FIFO so the block is properly drained.
     */
    if (s->data_dir != 0 && (s->cc_ext_mode & CC_EXT_MODE_DMASDRW) &&
        offset < DMA_FIFO_WINDOW) {
        return rza1l_sdhi_read_buf(s, size);
    }

    if (offset >= SD_RESP0 && offset <= SD_RESP7) {
        return s->resp[(offset - SD_RESP0) / 2];
    }

    switch (offset) {
    case SD_CMD:
        return s->cmd;
    case SD_ARG0:
        return s->arg0;
    case SD_ARG1:
        return s->arg1;
    case SD_STOP:
        return s->stop;
    case SD_SECCNT:
        return s->seccnt;
    case SD_INFO1:
        /*
         * The CD/DAT3 level bits reflect the live card-detect state and are not
         * latching, so report them from the bus rather than from stored status
         * (which a clear-write could otherwise drop).
         */
        if (sdbus_get_inserted(&s->sdbus)) {
            return s->info1 | INFO1_CD_LEVEL | INFO1_DAT3_LEVEL;
        }
        return s->info1 & ~(INFO1_CD_LEVEL | INFO1_DAT3_LEVEL);
    case SD_INFO2:
        /* SCLKDIVEN is always reported set: the SD bus is never busy. */
        return s->info2 | INFO2_SCLKDIVEN;
    case SD_INFO1_MASK:
        return s->info1_mask;
    case SD_INFO2_MASK:
        return s->info2_mask;
    case SD_CLK_CTRL:
        return s->clk_ctrl;
    case SD_SIZE:
        return s->size;
    case SD_OPTION:
        return s->option;
    case SD_ERR_STS1:
        return s->err_sts1;
    case SD_ERR_STS2:
        return s->err_sts2;
    case SD_BUF0:
        return rza1l_sdhi_read_buf(s, size);
    case SDIO_MODE:
        return s->sdio_mode;
    case SDIO_INFO1:
        return s->sdio_info1;
    case SDIO_INFO1_MASK:
        return s->sdio_info1_mask;
    case CC_EXT_MODE:
        return s->cc_ext_mode;
    case SOFT_RST:
        return s->soft_rst;
    case SD_VERSION:
        return 0;
    case EXT_SWAP:
        return s->ext_swap;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @0x%02x\n",
                      __func__, (unsigned)offset);
        return 0;
    }
}

static void rza1l_sdhi_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    RzA1lSdhiState *s = opaque;
    uint16_t val = (uint16_t)value;

    /* DMA-driven writes target the 64-byte DMA window; route to the FIFO. */
    if (s->data_dir != 0 && (s->cc_ext_mode & CC_EXT_MODE_DMASDRW) &&
        offset < DMA_FIFO_WINDOW) {
        rza1l_sdhi_write_buf(s, (uint32_t)value, size);
        return;
    }

    if (offset >= SD_RESP0 && offset <= SD_RESP7) {
        /* Response registers are read-only. */
        return;
    }

    switch (offset) {
    case SD_CMD:
        s->cmd = val;
        rza1l_sdhi_command(s, val);
        break;
    case SD_ARG0:
        s->arg0 = val;
        break;
    case SD_ARG1:
        s->arg1 = val;
        break;
    case SD_STOP:
        s->stop = val;
        break;
    case SD_SECCNT:
        s->seccnt = val;
        break;
    case SD_INFO1:
        /* Status register: written with 0s to clear the matching bits. */
        s->info1 &= val;
        rza1l_sdhi_update_irq(s);
        break;
    case SD_INFO2:
        s->info2 &= val;
        rza1l_sdhi_update_irq(s);
        break;
    case SD_INFO1_MASK:
        s->info1_mask = val;
        rza1l_sdhi_update_irq(s);
        break;
    case SD_INFO2_MASK:
        s->info2_mask = val;
        rza1l_sdhi_update_irq(s);
        break;
    case SD_CLK_CTRL:
        s->clk_ctrl = val;
        break;
    case SD_SIZE:
        s->size = val;
        break;
    case SD_OPTION:
        s->option = val;
        break;
    case SD_ERR_STS1:
        s->err_sts1 = val;
        break;
    case SD_ERR_STS2:
        s->err_sts2 = val;
        break;
    case SD_BUF0:
        rza1l_sdhi_write_buf(s, (uint32_t)value, size);
        break;
    case SDIO_MODE:
        s->sdio_mode = val;
        break;
    case SDIO_INFO1:
        s->sdio_info1 &= val;
        break;
    case SDIO_INFO1_MASK:
        s->sdio_info1_mask = val;
        break;
    case CC_EXT_MODE:
        s->cc_ext_mode = val;
        break;
    case SOFT_RST:
        s->soft_rst = val;
        /* Asserting reset (bit 0 clear) aborts any in-flight transfer. */
        if (!(val & 0x0001)) {
            s->data_dir = 0;
            s->datacnt = 0;
            s->blockcnt = 0;
            s->info2 &= ~(INFO2_RE | INFO2_WE);
            rza1l_sdhi_update_irq(s);
        }
        break;
    case EXT_SWAP:
        s->ext_swap = val;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @0x%02x = 0x%04x\n",
                      __func__, (unsigned)offset, val);
        break;
    }
}

static const MemoryRegionOps rza1l_sdhi_ops = {
    .read = rza1l_sdhi_read,
    .write = rza1l_sdhi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void rza1l_sdhi_init(Object *obj)
{
    RzA1lSdhiState *s = RZA1L_SDHI(obj);

    qbus_init(&s->sdbus, sizeof(s->sdbus), TYPE_SD_BUS, DEVICE(s), "sd-bus");

    memory_region_init_io(&s->iomem, obj, &rza1l_sdhi_ops, s,
                          TYPE_RZA1L_SDHI, RZA1L_SDHI_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
}

static void rza1l_sdhi_reset(DeviceState *dev)
{
    RzA1lSdhiState *s = RZA1L_SDHI(dev);

    s->cmd = 0;
    s->arg0 = 0;
    s->arg1 = 0;
    s->stop = 0;
    s->seccnt = 0;
    s->size = 0;
    memset(s->resp, 0, sizeof(s->resp));
    s->info1 = 0;
    s->info2 = 0;
    /* Interrupts start fully masked (register bits set = disabled). */
    s->info1_mask = 0xffff;
    s->info2_mask = 0xffff;
    s->clk_ctrl = 0;
    s->option = 0;
    s->err_sts1 = 0;
    s->err_sts2 = 0;
    s->sdio_mode = 0;
    s->sdio_info1 = 0;
    s->sdio_info1_mask = 0xffff;
    s->cc_ext_mode = 0;
    s->soft_rst = 0;
    s->ext_swap = 0;
    s->data_dir = 0;
    s->blocklen = 0;
    s->blockcnt = 0;
    s->datacnt = 0;

    qemu_set_irq(s->irq, 0);
}

static const VMStateDescription vmstate_rza1l_sdhi = {
    .name = TYPE_RZA1L_SDHI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(cmd, RzA1lSdhiState),
        VMSTATE_UINT16(arg0, RzA1lSdhiState),
        VMSTATE_UINT16(arg1, RzA1lSdhiState),
        VMSTATE_UINT16(stop, RzA1lSdhiState),
        VMSTATE_UINT16(seccnt, RzA1lSdhiState),
        VMSTATE_UINT16(size, RzA1lSdhiState),
        VMSTATE_UINT16_ARRAY(resp, RzA1lSdhiState, 8),
        VMSTATE_UINT16(info1, RzA1lSdhiState),
        VMSTATE_UINT16(info2, RzA1lSdhiState),
        VMSTATE_UINT16(info1_mask, RzA1lSdhiState),
        VMSTATE_UINT16(info2_mask, RzA1lSdhiState),
        VMSTATE_UINT16(clk_ctrl, RzA1lSdhiState),
        VMSTATE_UINT16(option, RzA1lSdhiState),
        VMSTATE_UINT16(err_sts1, RzA1lSdhiState),
        VMSTATE_UINT16(err_sts2, RzA1lSdhiState),
        VMSTATE_UINT16(sdio_mode, RzA1lSdhiState),
        VMSTATE_UINT16(sdio_info1, RzA1lSdhiState),
        VMSTATE_UINT16(sdio_info1_mask, RzA1lSdhiState),
        VMSTATE_UINT16(cc_ext_mode, RzA1lSdhiState),
        VMSTATE_UINT16(soft_rst, RzA1lSdhiState),
        VMSTATE_UINT16(ext_swap, RzA1lSdhiState),
        VMSTATE_UINT8(data_dir, RzA1lSdhiState),
        VMSTATE_UINT32(blocklen, RzA1lSdhiState),
        VMSTATE_UINT32(blockcnt, RzA1lSdhiState),
        VMSTATE_UINT32(datacnt, RzA1lSdhiState),
        VMSTATE_END_OF_LIST()
    }
};

static void rza1l_sdhi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, rza1l_sdhi_reset);
    dc->vmsd = &vmstate_rza1l_sdhi;
    dc->user_creatable = false;
}

static const TypeInfo rza1l_sdhi_info = {
    .name          = TYPE_RZA1L_SDHI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RzA1lSdhiState),
    .instance_init = rza1l_sdhi_init,
    .class_init    = rza1l_sdhi_class_init,
};

static void rza1l_sdhi_register_types(void)
{
    type_register_static(&rza1l_sdhi_info);
}

type_init(rza1l_sdhi_register_types)
