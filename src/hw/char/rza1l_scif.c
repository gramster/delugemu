/*
 * Renesas RZ/A1 SCIF (serial communication interface with FIFO)
 *
 * Models one SCIF channel as a UART. Transmit is synchronous: a byte written
 * to the transmit FIFO data register (SCFTDR) is pushed straight to the
 * character backend and the FIFO is reported permanently ready (TDFE|TEND).
 * Receive is interrupt-driven: an incoming byte is latched, the receive-data
 * flags (RDF|DR) are set, and the RXI interrupt is asserted when receive
 * interrupts are enabled (SCSCR.RIE). Reading SCFRDR returns the byte and
 * clears the receive state.
 *
 * Register offsets within a channel (verified via offsetof on st_scif):
 *   SCSMR 0x00  SCBRR 0x04  SCSCR 0x08  SCFTDR 0x0C  SCFSR 0x10
 *   SCFRDR 0x14 SCFCR 0x18  SCFDR 0x1C  SCSPTR 0x20  SCLSR 0x24  SCEMR 0x28
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "hw/char/rza1l_scif.h"
#include "hw/dma/rza1l_dmac.h"

/* Channel register offsets. */
#define SCIF_SCSMR  0x00 /* serial mode            (16) */
#define SCIF_SCBRR  0x04 /* bit rate               (8)  */
#define SCIF_SCSCR  0x08 /* serial control         (16) */
#define SCIF_SCFTDR 0x0C /* transmit FIFO data     (8)  */
#define SCIF_SCFSR  0x10 /* serial status          (16) */
#define SCIF_SCFRDR 0x14 /* receive FIFO data      (8)  */
#define SCIF_SCFCR  0x18 /* FIFO control           (16) */
#define SCIF_SCFDR  0x1C /* FIFO data count        (16) */
#define SCIF_SCSPTR 0x20 /* serial port            (16) */
#define SCIF_SCLSR  0x24 /* line status            (16) */
#define SCIF_SCEMR  0x28 /* serial extended mode   (16) */

/* SCSCR control bits. */
#define SCSCR_RE  0x0010 /* receive enable             */
#define SCSCR_TE  0x0020 /* transmit enable            */
#define SCSCR_RIE 0x0040 /* receive interrupt enable   */
#define SCSCR_TIE 0x0080 /* transmit interrupt enable  */

/* SCFSR status bits. */
#define SCFSR_DR   0x0001 /* receive data ready         */
#define SCFSR_RDF  0x0002 /* receive FIFO data full     */
#define SCFSR_TDFE 0x0020 /* transmit FIFO data empty   */
#define SCFSR_TEND 0x0040 /* transmit end               */

static void rza1l_scif_update_irq(RzA1lScifState *s)
{
    bool rx = (s->scscr & SCSCR_RIE) && s->rx_full;

    qemu_set_irq(s->irq, rx);
}

static uint64_t rza1l_scif_read(void *opaque, hwaddr offset, unsigned size)
{
    RzA1lScifState *s = opaque;

    switch (offset) {
    case SCIF_SCSMR:
        return s->scsmr;
    case SCIF_SCBRR:
        return s->scbrr;
    case SCIF_SCSCR:
        return s->scscr;
    case SCIF_SCFSR:
        /* Transmit is always ready; receive flags reflect the holding byte. */
        return s->scfsr | SCFSR_TDFE | SCFSR_TEND |
               (s->rx_full ? (SCFSR_RDF | SCFSR_DR) : 0);
    case SCIF_SCFRDR: {
        uint8_t b = s->rx_fifo;
        s->rx_full = false;
        s->scfsr &= ~(SCFSR_RDF | SCFSR_DR);
        rza1l_scif_update_irq(s);
        qemu_chr_fe_accept_input(&s->chr);
        return b;
    }
    case SCIF_SCFCR:
        return s->scfcr;
    case SCIF_SCFDR:
        /* Receive-data count in [4:0]; transmit FIFO always empty. */
        return s->rx_full ? 1 : 0;
    case SCIF_SCSPTR:
    case SCIF_SCLSR:
    case SCIF_SCEMR:
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void rza1l_scif_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    RzA1lScifState *s = opaque;

    switch (offset) {
    case SCIF_SCSMR:
        s->scsmr = value;
        break;
    case SCIF_SCBRR:
        s->scbrr = value;
        break;
    case SCIF_SCSCR:
        s->scscr = value;
        rza1l_scif_update_irq(s);
        break;
    case SCIF_SCFTDR: {
        uint8_t b = value;
        /* Transmit immediately to the backend. */
        qemu_chr_fe_write_all(&s->chr, &b, 1);
        break;
    }
    case SCIF_SCFSR:
        /*
         * Status bits are write-0-to-clear. Keep only the bits the firmware
         * leaves set; TDFE/TEND/RDF/DR are recomputed on read.
         */
        s->scfsr = value & ~(SCFSR_TDFE | SCFSR_TEND | SCFSR_RDF | SCFSR_DR);
        break;
    case SCIF_SCFCR:
        s->scfcr = value;
        break;
    case SCIF_SCFDR:
    case SCIF_SCSPTR:
    case SCIF_SCLSR:
    case SCIF_SCEMR:
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented write @ 0x%" HWADDR_PRIx
                      " value 0x%" PRIx64 "\n",
                      __func__, offset, value);
        break;
    }
}

static const MemoryRegionOps rza1l_scif_ops = {
    .read = rza1l_scif_read,
    .write = rza1l_scif_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static int rza1l_scif_can_receive(void *opaque)
{
    RzA1lScifState *s = opaque;

    /* Accept a byte only when receive is enabled and the holding reg is free. */
    return (s->scscr & SCSCR_RE) && !s->rx_full ? 1 : 0;
}

static void rza1l_scif_receive(void *opaque, const uint8_t *buf, int size)
{
    RzA1lScifState *s = opaque;

    if (size < 1) {
        return;
    }

    if (s->dmac) {
        /*
         * MIDI input is consumed by the firmware via the receive DMA ring
         * (it polls the channel's CRDA), so hand the byte straight to the
         * DMAC rather than latching it into SCFRDR and raising RXI.
         */
        rza1l_dmac_peripheral_rx_push(s->dmac, s->rx_dma_channel, buf[0]);
        return;
    }

    s->rx_fifo = buf[0];
    s->rx_full = true;
    s->scfsr |= SCFSR_RDF | SCFSR_DR;
    rza1l_scif_update_irq(s);
}

void rza1l_scif_set_rx_dma(RzA1lScifState *s, struct RzA1lDmacState *dmac,
                           int rx_dma_channel)
{
    s->dmac = dmac;
    s->rx_dma_channel = rx_dma_channel;
    rza1l_dmac_register_rx_ring(dmac, rx_dma_channel);
}

static void rza1l_scif_reset(DeviceState *dev)
{
    RzA1lScifState *s = RZA1L_SCIF(dev);

    s->scsmr = 0;
    s->scbrr = 0xff;
    s->scscr = 0;
    s->scfsr = 0;
    s->scfcr = 0;
    s->rx_fifo = 0;
    s->rx_full = false;
    rza1l_scif_update_irq(s);
}

static void rza1l_scif_realize(DeviceState *dev, Error **errp)
{
    RzA1lScifState *s = RZA1L_SCIF(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rza1l_scif_ops, s,
                          TYPE_RZA1L_SCIF, RZA1L_SCIF_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    qemu_chr_fe_set_handlers(&s->chr, rza1l_scif_can_receive,
                             rza1l_scif_receive, NULL, NULL, s, NULL, true);
}

static const Property rza1l_scif_properties[] = {
    DEFINE_PROP_CHR("chardev", RzA1lScifState, chr),
};

static const VMStateDescription vmstate_rza1l_scif = {
    .name = TYPE_RZA1L_SCIF,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(scsmr, RzA1lScifState),
        VMSTATE_UINT8(scbrr, RzA1lScifState),
        VMSTATE_UINT16(scscr, RzA1lScifState),
        VMSTATE_UINT16(scfsr, RzA1lScifState),
        VMSTATE_UINT16(scfcr, RzA1lScifState),
        VMSTATE_UINT8(rx_fifo, RzA1lScifState),
        VMSTATE_BOOL(rx_full, RzA1lScifState),
        VMSTATE_END_OF_LIST()
    },
};

static void rza1l_scif_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rza1l_scif_realize;
    dc->vmsd = &vmstate_rza1l_scif;
    device_class_set_legacy_reset(dc, rza1l_scif_reset);
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

