/*
 * Renesas RZ/A1 BSC (bus state controller) — minimal model
 *
 * The Deluge has a 64 MB SDRAM device on chip-select 3 (mapped at 0x0C000000).
 * During boot the firmware programs the BSC to bring it up: the chip-select
 * bus/wait control registers (CS2BCR/CS3BCR, CS3WCR), the SDRAM control and
 * refresh registers (SDCR, RTCSR, RTCOR), and finally the SDRAM mode register
 * via the controller's mode-set address windows (offsets 0x1040 and 0x2040).
 *
 * The SDRAM itself is already backed by a plain RAM region created by the SoC,
 * so there is no real controller state to maintain — this model just shadows
 * every register write (and reads it back) so the init sequence completes
 * without faulting and the firmware proceeds to use SDRAM.
 *
 * Register layout (offsetof on the firmware's st_bsc, based at 0x3FFFC000):
 *   CMNCR 0x00  CS0BCR 0x04 ... CS3BCR 0x10 ...  CS3WCR 0x34 ...
 *   SDCR 0x4C  RTCSR 0x50  RTCNT 0x54  RTCOR 0x58
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/misc/rza1l_bsc.h"

static uint64_t rza1l_bsc_read(void *opaque, hwaddr offset, unsigned size)
{
    RzA1lBscState *s = opaque;
    uint64_t val = 0;

    for (unsigned i = 0; i < size; i++) {
        hwaddr boff = offset + i;
        if (boff < RZA1L_BSC_MMIO_SIZE) {
            val |= (uint64_t)s->regs[boff] << (8 * i);
        }
    }

    return val;
}

static void rza1l_bsc_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    RzA1lBscState *s = opaque;

    for (unsigned i = 0; i < size; i++) {
        hwaddr boff = offset + i;
        if (boff < RZA1L_BSC_MMIO_SIZE) {
            s->regs[boff] = (value >> (8 * i)) & 0xff;
        }
    }
}

static const MemoryRegionOps rza1l_bsc_ops = {
    .read = rza1l_bsc_read,
    .write = rza1l_bsc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void rza1l_bsc_reset(DeviceState *dev)
{
    RzA1lBscState *s = RZA1L_BSC(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void rza1l_bsc_realize(DeviceState *dev, Error **errp)
{
    RzA1lBscState *s = RZA1L_BSC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rza1l_bsc_ops, s,
                          TYPE_RZA1L_BSC, RZA1L_BSC_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_rza1l_bsc = {
    .name = TYPE_RZA1L_BSC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, RzA1lBscState, RZA1L_BSC_MMIO_SIZE),
        VMSTATE_END_OF_LIST()
    },
};

static void rza1l_bsc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rza1l_bsc_realize;
    dc->vmsd = &vmstate_rza1l_bsc;
    device_class_set_legacy_reset(dc, rza1l_bsc_reset);
}

static const TypeInfo rza1l_bsc_info = {
    .name          = TYPE_RZA1L_BSC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RzA1lBscState),
    .class_init    = rza1l_bsc_class_init,
};

static void rza1l_bsc_register_types(void)
{
    type_register_static(&rza1l_bsc_info);
}

type_init(rza1l_bsc_register_types)
