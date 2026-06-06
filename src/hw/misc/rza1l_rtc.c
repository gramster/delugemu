/*
 * Renesas RZ/A1 RTC (real-time clock) — minimal stub
 *
 * The Deluge firmware does not read wall-clock time from the RTC, but the
 * block sits in the high I/O window and may be probed during low-level init.
 * This stub presents a small register file: the BCD calendar counters read
 * back a fixed, plausible date/time (2024-01-01 00:00:00, Monday) and every
 * register accepts writes, so any polling completes without faulting. The
 * counters do not advance — nothing in the firmware depends on them ticking.
 *
 * Register layout (offsetof on the firmware's st_rtc, based at 0xFCFF1000):
 *   R64CNT 0x00  RSECCNT 0x02  RMINCNT 0x04  RHRCNT 0x06  RWKCNT 0x08
 *   RDAYCNT 0x0A  RMONCNT 0x0C  RYRCNT 0x0E(16)  alarms 0x10..0x1A
 *   RCR1 0x1C  RCR2 0x1E  RYRAR 0x20(16)  RCR3 0x24  RCR5 0x26
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/misc/rza1l_rtc.h"

#define RTC_RSECCNT 0x02
#define RTC_RMINCNT 0x04
#define RTC_RHRCNT  0x06
#define RTC_RWKCNT  0x08
#define RTC_RDAYCNT 0x0A
#define RTC_RMONCNT 0x0C
#define RTC_RYRCNT  0x0E

static uint64_t rza1l_rtc_read(void *opaque, hwaddr offset, unsigned size)
{
    RzA1lRtcState *s = opaque;
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size && (offset + i) < RZA1L_RTC_MMIO_SIZE; i++) {
        val |= (uint64_t)s->regs[offset + i] << (8 * i);
    }
    return val;
}

static void rza1l_rtc_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    RzA1lRtcState *s = opaque;
    unsigned i;

    for (i = 0; i < size && (offset + i) < RZA1L_RTC_MMIO_SIZE; i++) {
        s->regs[offset + i] = (value >> (8 * i)) & 0xff;
    }
}

static const MemoryRegionOps rza1l_rtc_ops = {
    .read = rza1l_rtc_read,
    .write = rza1l_rtc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
};

static void rza1l_rtc_reset(DeviceState *dev)
{
    RzA1lRtcState *s = RZA1L_RTC(dev);

    memset(s->regs, 0, sizeof(s->regs));

    /* Plausible fixed wall-clock time in BCD: 2024-01-01 00:00:00, Monday. */
    s->regs[RTC_RSECCNT] = 0x00;
    s->regs[RTC_RMINCNT] = 0x00;
    s->regs[RTC_RHRCNT]  = 0x00;
    s->regs[RTC_RWKCNT]  = 0x01;  /* Monday */
    s->regs[RTC_RDAYCNT] = 0x01;
    s->regs[RTC_RMONCNT] = 0x01;
    s->regs[RTC_RYRCNT]  = 0x24;  /* year 2024 (BCD low byte) */
    s->regs[RTC_RYRCNT + 1] = 0x20;
}

static void rza1l_rtc_realize(DeviceState *dev, Error **errp)
{
    RzA1lRtcState *s = RZA1L_RTC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rza1l_rtc_ops, s,
                          TYPE_RZA1L_RTC, RZA1L_RTC_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_rza1l_rtc = {
    .name = TYPE_RZA1L_RTC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, RzA1lRtcState, RZA1L_RTC_MMIO_SIZE),
        VMSTATE_END_OF_LIST()
    },
};

static void rza1l_rtc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rza1l_rtc_realize;
    dc->vmsd = &vmstate_rza1l_rtc;
    device_class_set_legacy_reset(dc, rza1l_rtc_reset);
}

static const TypeInfo rza1l_rtc_info = {
    .name          = TYPE_RZA1L_RTC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RzA1lRtcState),
    .class_init    = rza1l_rtc_class_init,
};

static void rza1l_rtc_register_types(void)
{
    type_register_static(&rza1l_rtc_info);
}

type_init(rza1l_rtc_register_types)
