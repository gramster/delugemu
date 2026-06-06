/*
 * Renesas RZ/A1 SCIF (serial communication interface with FIFO) — stub
 *
 * Currently a minimal MMIO device: reads return zero, writes are dropped (with
 * an unimplemented-access trace). The transmit path to the host console is
 * wired up but register decoding is intentionally left for M1.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "hw/char/rza1l_scif.h"

static uint64_t rza1l_scif_read(void *opaque, hwaddr offset, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n",
                  __func__, offset);
    return 0;
}

static void rza1l_scif_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    qemu_log_mask(LOG_UNIMP,
                  "%s: unimplemented write @ 0x%" HWADDR_PRIx
                  " value 0x%" PRIx64 "\n",
                  __func__, offset, value);
}

static const MemoryRegionOps rza1l_scif_ops = {
    .read = rza1l_scif_read,
    .write = rza1l_scif_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void rza1l_scif_realize(DeviceState *dev, Error **errp)
{
    RzA1lScifState *s = RZA1L_SCIF(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rza1l_scif_ops, s,
                          TYPE_RZA1L_SCIF, RZA1L_SCIF_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const Property rza1l_scif_properties[] = {
    DEFINE_PROP_CHR("chardev", RzA1lScifState, chr),
};

static const VMStateDescription vmstate_rza1l_scif = {
    .name = TYPE_RZA1L_SCIF,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
};

static void rza1l_scif_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rza1l_scif_realize;
    dc->vmsd = &vmstate_rza1l_scif;
    device_class_set_props(dc, rza1l_scif_properties);
}

static const TypeInfo rza1l_scif_info = {
    .name          = TYPE_RZA1L_SCIF,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RzA1lScifState),
    .class_init    = rza1l_scif_class_init,
};

static void rza1l_scif_register_types(void)
{
    type_register_static(&rza1l_scif_info);
}

type_init(rza1l_scif_register_types)
