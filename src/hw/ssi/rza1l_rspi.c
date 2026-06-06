/*
 * Renesas RZ/A1 RSPI (Renesas Serial Peripheral Interface) — minimal model
 *
 * The Deluge firmware drives the OLED display and the CV/gate DAC over RSPI0
 * (channel 0, 0xE800C800). Its driver (R_RSPI_SendBasic8/32, R_RSPI_WaitEnd)
 * busy-waits on the status register before writing the data register:
 *
 *   while (SPSR.SPTEF == 0);   // wait for TX buffer empty
 *   SPDR = data;              // push a byte/word
 *   while (SPSR.TEND == 0);    // wait for transfer end
 *
 * This model accepts all writes, discards transmitted data, and always reports
 * the transmitter as ready (SPTEF | TEND set) so the firmware never stalls.
 * Actual pixel/CV output is handled elsewhere (OLED model, M4) once the SPI/DMA
 * path is wired up; for boot bring-up an always-ready transmitter is enough.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "hw/ssi/rza1l_rspi.h"

/* Register offsets within an RSPI channel block. */
#define RSPI_SPSR     0x03  /* status register (read-mostly) */
#define RSPI_SPDR     0x04  /* data register (32-bit) */
#define RSPI_SPDR_END 0x08

/* SPSR status bits. */
#define RSPI_SPSR_SPTEF 0x20  /* transmit buffer empty */
#define RSPI_SPSR_TEND  0x40  /* transfer end */

/* The transmitter is always idle and ready in this model. */
#define RSPI_SPSR_READY (RSPI_SPSR_SPTEF | RSPI_SPSR_TEND)

static uint64_t rza1l_rspi_read(void *opaque, hwaddr offset, unsigned size)
{
    RzA1lRspiState *s = opaque;
    uint64_t val = 0;

    for (unsigned i = 0; i < size; i++) {
        hwaddr boff = offset + i;
        uint8_t b;

        if (boff == RSPI_SPSR) {
            b = RSPI_SPSR_READY;
        } else if (boff >= RSPI_SPDR && boff < RSPI_SPDR_END) {
            b = 0; /* nothing received */
        } else {
            b = s->regs[boff];
        }
        val |= (uint64_t)b << (8 * i);
    }

    return val;
}

static void rza1l_rspi_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    RzA1lRspiState *s = opaque;

    for (unsigned i = 0; i < size; i++) {
        hwaddr boff = offset + i;
        uint8_t b = (value >> (8 * i)) & 0xff;

        if (boff >= RSPI_SPDR && boff < RSPI_SPDR_END) {
            /* Transmitted data: accepted and discarded. */
            continue;
        }
        if (boff == RSPI_SPSR) {
            /* Status is driven by hardware state, not by writes. */
            continue;
        }
        s->regs[boff] = b;
    }
}

static const MemoryRegionOps rza1l_rspi_ops = {
    .read = rza1l_rspi_read,
    .write = rza1l_rspi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void rza1l_rspi_reset(DeviceState *dev)
{
    RzA1lRspiState *s = RZA1L_RSPI(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void rza1l_rspi_realize(DeviceState *dev, Error **errp)
{
    RzA1lRspiState *s = RZA1L_RSPI(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rza1l_rspi_ops, s,
                          TYPE_RZA1L_RSPI, RZA1L_RSPI_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription vmstate_rza1l_rspi = {
    .name = TYPE_RZA1L_RSPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, RzA1lRspiState, RZA1L_RSPI_MMIO_SIZE),
        VMSTATE_END_OF_LIST()
    },
};

static void rza1l_rspi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rza1l_rspi_realize;
    dc->vmsd = &vmstate_rza1l_rspi;
    device_class_set_legacy_reset(dc, rza1l_rspi_reset);
}

static const TypeInfo rza1l_rspi_info = {
    .name          = TYPE_RZA1L_RSPI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RzA1lRspiState),
    .class_init    = rza1l_rspi_class_init,
};

static void rza1l_rspi_register_types(void)
{
    type_register_static(&rza1l_rspi_info);
}

type_init(rza1l_rspi_register_types)
