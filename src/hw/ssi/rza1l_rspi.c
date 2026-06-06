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
 * The low byte of each data-register write is forwarded to the OLED panel
 * model (deluge_oled), which decodes the SSD130x command/pixel stream; CV/gate
 * words are ignored by the panel while its chip select is de-asserted.
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
#include "hw/display/deluge_oled.h"

/* Register offsets within an RSPI channel block. */
#define RSPI_SPCR     0x00  /* control register */
#define RSPI_SPSR     0x03  /* status register (read-mostly) */
#define RSPI_SPDR     0x04  /* data register (32-bit) */
#define RSPI_SPDR_END 0x08

/* SPCR control bits. */
#define RSPI_SPCR_SPRIE 0x80  /* receive-buffer-full interrupt enable */

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
    bool spdr_written = false;
    bool spcr_written = false;

    for (unsigned i = 0; i < size; i++) {
        hwaddr boff = offset + i;
        uint8_t b = (value >> (8 * i)) & 0xff;

        if (boff >= RSPI_SPDR && boff < RSPI_SPDR_END) {
            /*
             * Transmitted data. The OLED panel is fed from the low byte of
             * the data register (the firmware streams 8-bit pixel/command
             * bytes through SPDR.BYTE.LL); the panel ignores them while its
             * chip select is de-asserted, so CV/gate words are unaffected.
             */
            if (boff == RSPI_SPDR && s->oled) {
                deluge_oled_spi_byte(s->oled, b);
            }
            spdr_written = true;
            continue;
        }
        if (boff == RSPI_SPSR) {
            /* Status is driven by hardware state, not by writes. */
            continue;
        }
        if (boff == RSPI_SPCR) {
            spcr_written = true;
        }
        s->regs[boff] = b;
    }

    /*
     * Receive-buffer-full interrupt (SPRI). The CV/gate path arms it via
     * SPCR.SPRIE and then drives a 32-bit word into SPDR, relying on the
     * resulting interrupt (cvSPITransferComplete) to advance the shared SPI
     * transfer queue — which is what eventually kicks off OLED frame
     * transfers. This model completes every transfer instantly, so the
     * receive buffer is "full" as soon as the word is written.
     *
     * SPRF/SPRI is level-sensitive on real hardware: the line stays asserted
     * until the ISR clears the condition, which it does by clearing SPRIE
     * (RSPI.SPCR &= ~(1 << 7)) on entry. Model it the same way so the GIC
     * latches it reliably (a brief pulse would be dropped). OLED pixel/command
     * bytes are streamed with SPRIE clear (they use DMA + the DMA-complete
     * IRQ), so they never assert this line.
     */
    if (spdr_written && (s->regs[RSPI_SPCR] & RSPI_SPCR_SPRIE)) {
        qemu_set_irq(s->irq, 1);
    } else if (spcr_written && !(s->regs[RSPI_SPCR] & RSPI_SPCR_SPRIE)) {
        qemu_set_irq(s->irq, 0);
    }
}

void rza1l_rspi_set_oled(RzA1lRspiState *s, struct DelugeOledState *oled)
{
    s->oled = oled;
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
