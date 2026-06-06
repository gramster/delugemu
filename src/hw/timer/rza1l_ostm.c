/*
 * Renesas RZ/A1 OSTM (OS Timer) — free-running counter model
 *
 * The Deluge firmware's task scheduler programs OSTM0 as a free-running 32-bit
 * up-counter and reads OSTMnCNT as its high-resolution time base:
 *
 *   setTimerValue(0, 0);                  // OSTMnCMP = 0
 *   setOperatingMode(0, FREE_RUNNING, 0); // OSTMnCTL = MD
 *   enableTimer(0);                       // OSTMnTS  = 1
 *   ...
 *   now = getTimerValue(0);               // reads OSTMnCNT
 *
 * This model advances OSTMnCNT off the virtual clock at the OSTM peripheral
 * clock rate so those reads progress. In free-running mode the counter counts
 * up from 0 while started; in interval mode it counts down from OSTMnCMP and
 * reloads. Compare-match interrupts are not modelled — only the counter value,
 * which is what the firmware polls during boot.
 *
 * Register offsets within each channel window (verified via offsetof on the
 * firmware's st_ostm):
 *   OSTMnCMP 0x00  OSTMnCNT 0x04  OSTMnTE 0x10
 *   OSTMnTS  0x14  OSTMnTT  0x18  OSTMnCTL 0x20
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/host-utils.h"
#include "qemu/timer.h"
#include "migration/vmstate.h"
#include "hw/timer/rza1l_ostm.h"

/*
 * OSTM input clock. The RZ/A1 clocks the OSTM from the peripheral clock P0phi
 * (~33.33 MHz, matching the firmware's DELUGE_CLOCKS_PER). The exact rate only
 * affects how fast wall-clock time maps to counts, not boot progress.
 */
#define RZA1L_OSTM_CLK_HZ 33333333u

/* Per-channel register offsets. */
#define OSTM_CMP 0x00 /* 32-bit compare/period            */
#define OSTM_CNT 0x04 /* 32-bit counter (read-only)       */
#define OSTM_TE  0x10 /*  8-bit count-enable status (RO)  */
#define OSTM_TS  0x14 /*  8-bit start trigger (WO)        */
#define OSTM_TT  0x18 /*  8-bit stop trigger (WO)         */
#define OSTM_CTL 0x20 /*  8-bit control                   */

#define OSTM_CTL_MD 0x02 /* operating mode: 0 interval, 1 free-running */

#define RZA1L_OSTM_CH_STRIDE 0x400

static uint32_t rza1l_ostm_cnt_value(RzA1lOstmChannel *c)
{
    int64_t now;
    uint64_t elapsed, ticks;

    if (!c->enabled) {
        return c->frozen;
    }

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    elapsed = now - c->start_ns;
    ticks = muldiv64(elapsed, RZA1L_OSTM_CLK_HZ, NANOSECONDS_PER_SECOND);

    if (c->ctl & OSTM_CTL_MD) {
        /* Free-running: count up from 0. */
        return (uint32_t)ticks;
    }

    /* Interval: count down from CMP and reload. */
    if (c->cmp == 0) {
        return 0;
    }
    return c->cmp - (uint32_t)(ticks % ((uint64_t)c->cmp + 1));
}

static uint64_t rza1l_ostm_read(void *opaque, hwaddr offset, unsigned size)
{
    RzA1lOstmState *s = opaque;
    int ch = (offset / RZA1L_OSTM_CH_STRIDE) & 1;
    hwaddr r = offset % RZA1L_OSTM_CH_STRIDE;
    RzA1lOstmChannel *c = &s->ch[ch];

    switch (r) {
    case OSTM_CMP:
        return c->cmp;
    case OSTM_CNT:
        return rza1l_ostm_cnt_value(c);
    case OSTM_TE:
        return c->enabled ? 1 : 0;
    case OSTM_CTL:
        return c->ctl;
    case OSTM_TS:
    case OSTM_TT:
        /* Write-only trigger registers read as 0. */
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read ch%d off 0x%" HWADDR_PRIx
                      "\n", TYPE_RZA1L_OSTM, ch, r);
        return 0;
    }
}

static void rza1l_ostm_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    RzA1lOstmState *s = opaque;
    int ch = (offset / RZA1L_OSTM_CH_STRIDE) & 1;
    hwaddr r = offset % RZA1L_OSTM_CH_STRIDE;
    RzA1lOstmChannel *c = &s->ch[ch];

    switch (r) {
    case OSTM_CMP:
        c->cmp = value;
        break;
    case OSTM_CTL:
        c->ctl = value & 0xff;
        break;
    case OSTM_TS:
        if (value & 1) {
            /* Start: counter (re)bases to 0 and runs. */
            c->enabled = true;
            c->frozen = 0;
            c->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
        break;
    case OSTM_TT:
        if (value & 1) {
            /* Stop: latch the current count. */
            c->frozen = rza1l_ostm_cnt_value(c);
            c->enabled = false;
        }
        break;
    case OSTM_CNT:
    case OSTM_TE:
        /* Read-only. */
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write ch%d off 0x%" HWADDR_PRIx
                      " val 0x%" PRIx64 "\n", TYPE_RZA1L_OSTM, ch, r, value);
        break;
    }
}

static const MemoryRegionOps rza1l_ostm_ops = {
    .read = rza1l_ostm_read,
    .write = rza1l_ostm_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void rza1l_ostm_reset(DeviceState *dev)
{
    RzA1lOstmState *s = RZA1L_OSTM(dev);

    for (int i = 0; i < RZA1L_OSTM_NUM_CH; i++) {
        s->ch[i].cmp = 0;
        s->ch[i].ctl = 0;
        s->ch[i].enabled = false;
        s->ch[i].start_ns = 0;
        s->ch[i].frozen = 0;
    }
}

static void rza1l_ostm_realize(DeviceState *dev, Error **errp)
{
    RzA1lOstmState *s = RZA1L_OSTM(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rza1l_ostm_ops, s,
                          TYPE_RZA1L_OSTM, RZA1L_OSTM_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_rza1l_ostm_channel = {
    .name = "rza1l-ostm-channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cmp, RzA1lOstmChannel),
        VMSTATE_UINT8(ctl, RzA1lOstmChannel),
        VMSTATE_BOOL(enabled, RzA1lOstmChannel),
        VMSTATE_INT64(start_ns, RzA1lOstmChannel),
        VMSTATE_UINT32(frozen, RzA1lOstmChannel),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_rza1l_ostm = {
    .name = TYPE_RZA1L_OSTM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(ch, RzA1lOstmState, RZA1L_OSTM_NUM_CH, 1,
                             vmstate_rza1l_ostm_channel, RzA1lOstmChannel),
        VMSTATE_END_OF_LIST()
    },
};

static void rza1l_ostm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rza1l_ostm_realize;
    dc->vmsd = &vmstate_rza1l_ostm;
    device_class_set_legacy_reset(dc, rza1l_ostm_reset);
}

static const TypeInfo rza1l_ostm_info = {
    .name          = TYPE_RZA1L_OSTM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RzA1lOstmState),
    .class_init    = rza1l_ostm_class_init,
};

static void rza1l_ostm_register_types(void)
{
    type_register_static(&rza1l_ostm_info);
}

type_init(rza1l_ostm_register_types)
