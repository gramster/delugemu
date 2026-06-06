/*
 * Renesas RZ/A1 WDT (watchdog timer) — minimal model
 *
 * The Deluge firmware periodically "kicks" the watchdog by rewriting the
 * counter (WTCNT) and control (WTCSR) registers, each guarded by an 8-bit
 * write key in the upper byte (0x5A for WTCNT, 0xA5 for WTCSR). Since nothing
 * here ever needs to actually reset the system, the model just records the low
 * byte of each keyed write and lets the counter read back as written — the
 * watchdog never expires, so the firmware's refresh loop runs without faulting.
 *
 * Register layout (offsetof on the firmware's st_wdt, based at 0xFCFE0000):
 *   WTCSR 0x00 (16)  WTCNT 0x02 (16)  WRCSR 0x04 (16)
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/misc/rza1l_wdt.h"

#define WDT_WTCSR 0x00
#define WDT_WTCNT 0x02
#define WDT_WRCSR 0x04

static uint64_t rza1l_wdt_read(void *opaque, hwaddr offset, unsigned size)
{
    RzA1lWdtState *s = opaque;

    switch (offset) {
    case WDT_WTCSR:
        return s->wtcsr;
    case WDT_WTCNT:
        return s->wtcnt;
    case WDT_WRCSR:
        return s->wrcsr;
    default:
        return 0;
    }
}

static void rza1l_wdt_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    RzA1lWdtState *s = opaque;
    uint8_t v = value & 0xff; /* keyed writes carry the data in the low byte */

    switch (offset) {
    case WDT_WTCSR:
        s->wtcsr = v;
        break;
    case WDT_WTCNT:
        s->wtcnt = v;
        break;
    case WDT_WRCSR:
        s->wrcsr = v;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps rza1l_wdt_ops = {
    .read = rza1l_wdt_read,
    .write = rza1l_wdt_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
};

static void rza1l_wdt_reset(DeviceState *dev)
{
    RzA1lWdtState *s = RZA1L_WDT(dev);

    s->wtcsr = 0;
    s->wtcnt = 0;
    s->wrcsr = 0;
}

static void rza1l_wdt_realize(DeviceState *dev, Error **errp)
{
    RzA1lWdtState *s = RZA1L_WDT(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rza1l_wdt_ops, s,
                          TYPE_RZA1L_WDT, RZA1L_WDT_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_rza1l_wdt = {
    .name = TYPE_RZA1L_WDT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(wtcsr, RzA1lWdtState),
        VMSTATE_UINT16(wtcnt, RzA1lWdtState),
        VMSTATE_UINT16(wrcsr, RzA1lWdtState),
        VMSTATE_END_OF_LIST()
    },
};

static void rza1l_wdt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rza1l_wdt_realize;
    dc->vmsd = &vmstate_rza1l_wdt;
    device_class_set_legacy_reset(dc, rza1l_wdt_reset);
}

static const TypeInfo rza1l_wdt_info = {
    .name          = TYPE_RZA1L_WDT,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RzA1lWdtState),
    .class_init    = rza1l_wdt_class_init,
};

static void rza1l_wdt_register_types(void)
{
    type_register_static(&rza1l_wdt_info);
}

type_init(rza1l_wdt_register_types)
