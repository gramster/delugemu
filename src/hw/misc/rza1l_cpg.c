/*
 * Renesas RZ/A1 CPG (clock pulse generator) + module standby — minimal model
 *
 * The Deluge firmware programs the PLL/divider tree through FRQCR/FRQCR2 to
 * derive the I:G:B:P1:P0 = 400:266:133:66:33 MHz clocks from the 13.33 MHz
 * crystal, then ungates the peripherals it uses by clearing bits in the
 * module-standby control registers (STBCR1..STBCR10, which live in the same
 * register block as the clock control). Both are pure configuration writes —
 * there is no PLL to lock and no clock to actually gate in emulation — so this
 * model simply shadows every register and reads back what was written. The
 * CPU-status register (CPUSTS) reads back 0, i.e. "running, not in standby",
 * which is the state the firmware expects after clock setup.
 *
 * Register layout (verified via offsetof on the firmware's st_cpg; the block
 * is based at 0xFCFE0010):
 *   FRQCR 0x00  FRQCR2 0x04  CPUSTS 0x08  STBCR1 0x10  STBCR2 0x14 ...
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/misc/rza1l_cpg.h"

static uint64_t rza1l_cpg_read(void *opaque, hwaddr offset, unsigned size)
{
    RzA1lCpgState *s = opaque;
    uint64_t val = 0;

    for (unsigned i = 0; i < size; i++) {
        hwaddr boff = offset + i;
        if (boff < RZA1L_CPG_MMIO_SIZE) {
            val |= (uint64_t)s->regs[boff] << (8 * i);
        }
    }

    return val;
}

static void rza1l_cpg_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    RzA1lCpgState *s = opaque;

    for (unsigned i = 0; i < size; i++) {
        hwaddr boff = offset + i;
        if (boff < RZA1L_CPG_MMIO_SIZE) {
            s->regs[boff] = (value >> (8 * i)) & 0xff;
        }
    }
}

static const MemoryRegionOps rza1l_cpg_ops = {
    .read = rza1l_cpg_read,
    .write = rza1l_cpg_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void rza1l_cpg_reset(DeviceState *dev)
{
    RzA1lCpgState *s = RZA1L_CPG(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void rza1l_cpg_realize(DeviceState *dev, Error **errp)
{
    RzA1lCpgState *s = RZA1L_CPG(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rza1l_cpg_ops, s,
                          TYPE_RZA1L_CPG, RZA1L_CPG_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_rza1l_cpg = {
    .name = TYPE_RZA1L_CPG,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, RzA1lCpgState, RZA1L_CPG_MMIO_SIZE),
        VMSTATE_END_OF_LIST()
    },
};

static void rza1l_cpg_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rza1l_cpg_realize;
    dc->vmsd = &vmstate_rza1l_cpg;
    device_class_set_legacy_reset(dc, rza1l_cpg_reset);
}

static const TypeInfo rza1l_cpg_info = {
    .name          = TYPE_RZA1L_CPG,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RzA1lCpgState),
    .class_init    = rza1l_cpg_class_init,
};

static void rza1l_cpg_register_types(void)
{
    type_register_static(&rza1l_cpg_info);
}

type_init(rza1l_cpg_register_types)
