/*
 * Renesas RZ/A1L SoC model (as used by the Synthstrom Deluge)
 *
 * Builds the Cortex-A9 CPU, on-chip SRAM and external SDRAM regions, and a
 * logging catch-all over the peripheral space. Individual peripherals (SCIF,
 * SSI, INTC, CPG, BSC, SD, ...) are added incrementally.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/unimp.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "hw/arm/rza1l_soc.h"

static void rza1l_soc_init(Object *obj)
{
    RzA1lSocState *s = RZA1L_SOC(obj);

    object_initialize_child(obj, "cpu", &s->cpu, RZA1L_CPU_TYPE);

    /*
     * The board points this at its system address space before realize. The
     * SoC maps SRAM/SDRAM/peripherals into it.
     */
    object_property_add_link(obj, "memory", TYPE_MEMORY_REGION,
                             (Object **)&s->system_memory,
                             qdev_prop_allow_set_link_before_realize,
                             OBJ_PROP_LINK_STRONG);
}

static void rza1l_soc_realize(DeviceState *dev, Error **errp)
{
    RzA1lSocState *s = RZA1L_SOC(dev);
    MemoryRegion *system_memory = s->system_memory;
    Error *err = NULL;

    if (system_memory == NULL) {
        error_setg(errp, "%s: 'memory' link property not set", __func__);
        return;
    }

    /*
     * Realize the CPU. The Cortex-A9 in the RZ/A1 has no security extensions
     * exposed to firmware in the usual A-profile sense for this use; keep the
     * defaults and let the board/firmware configure the rest.
     */
    if (!qdev_realize(DEVICE(&s->cpu), NULL, errp)) {
        return;
    }

    /* On-chip SRAM (3 MB). */
    memory_region_init_ram(&s->sram, OBJECT(dev), "rza1l.sram",
                           RZA1L_SRAM_SIZE, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    memory_region_add_subregion(system_memory, RZA1L_SRAM_BASE, &s->sram);

    /* External SDRAM (64 MB). */
    memory_region_init_ram(&s->sdram, OBJECT(dev), "rza1l.sdram",
                           RZA1L_SDRAM_SIZE, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    memory_region_add_subregion(system_memory, RZA1L_SDRAM_BASE, &s->sdram);

    /*
     * Uncached mirrors at +0x40000000. The firmware accesses RAM through these
     * aliases for non-cacheable buffers (DMA, etc.); back them with the same
     * RAM so either view sees the same bytes.
     */
    memory_region_init_alias(&s->sram_mirror, OBJECT(dev), "rza1l.sram.mirror",
                             &s->sram, 0, RZA1L_SRAM_SIZE);
    memory_region_add_subregion(system_memory,
                                RZA1L_SRAM_BASE + RZA1L_UNCACHED_MIRROR_OFFSET,
                                &s->sram_mirror);

    memory_region_init_alias(&s->sdram_mirror, OBJECT(dev),
                             "rza1l.sdram.mirror", &s->sdram, 0,
                             RZA1L_SDRAM_SIZE);
    memory_region_add_subregion(system_memory,
                                RZA1L_SDRAM_BASE + RZA1L_UNCACHED_MIRROR_OFFSET,
                                &s->sdram_mirror);

    /*
     * Catch-alls for the three peripheral windows. They log unimplemented
     * accesses (-d unimp) so we can see what the firmware touches and decide
     * which device to model next. Real peripherals are mapped over the top of
     * these with higher priority as they are added.
     */
    create_unimplemented_device("rza1l.io.low",
                                RZA1L_IO_LOW_BASE, RZA1L_IO_LOW_SIZE);
    create_unimplemented_device("rza1l.io.mid",
                                RZA1L_IO_MID_BASE, RZA1L_IO_MID_SIZE);
    create_unimplemented_device("rza1l.io.high",
                                RZA1L_IO_HIGH_BASE, RZA1L_IO_HIGH_SIZE);
}

static void rza1l_soc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rza1l_soc_realize;
    /* This is a container SoC device; it cannot be user-created on the cmdline. */
    dc->user_creatable = false;
}

static const TypeInfo rza1l_soc_info = {
    .name          = TYPE_RZA1L_SOC,
    .parent        = TYPE_DEVICE,
    .instance_size = sizeof(RzA1lSocState),
    .instance_init = rza1l_soc_init,
    .class_init    = rza1l_soc_class_init,
};

static void rza1l_soc_register_types(void)
{
    type_register_static(&rza1l_soc_info);
}

type_init(rza1l_soc_register_types)
