/*
 * Renesas RZ/A1 USB 2.0 host/function controller (register stub)
 *
 * The Deluge firmware brings up the Renesas USB stack at start-up (USB host
 * mode, falling back to peripheral/mass-storage mode when nothing is attached).
 * Full host/function emulation is out of scope; instead this models the
 * register window faithfully for the "no cable connected" case:
 *
 *   - Configuration registers (SYSCFG0, INTENB0/1, pipe setup, ...) are
 *     shadowed so the firmware reads back what it wrote. In particular the
 *     SYSCFG0 clock-enable bit reads back immediately, so the module-start
 *     poll loop completes without spinning.
 *   - Status registers (SYSSTS0, INTSTS0/1, the *STS pipe registers, ...) read
 *     back zero: SYSSTS0.LNST = 00 means the SE0/disconnected line state, and a
 *     zero INTSTS0 means there is nothing to service.
 *   - The USBIn interrupt line is wired to the GIC but never asserted, since no
 *     device ever attaches.
 *
 * Register offsets (from st_usb20, base USB200 0xE8010000 / USB201 0xE8207000):
 *   SYSCFG0 0x00  BUSWAIT 0x02  SYSSTS0 0x04  DVSTCTR0 0x08  CFIFO 0x14
 *   CFIFOSEL 0x20 INTENB0 0x30  INTENB1 0x32  INTSTS0 0x40   INTSTS1 0x42
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "hw/usb/rza1l_usb.h"

/* Register offsets that report live hardware status; always read as zero. */
#define USB_SYSSTS0  0x04
#define USB_INTSTS0  0x40
#define USB_INTSTS1  0x42
#define USB_BRDYSTS  0x44
#define USB_NRDYSTS  0x46
#define USB_BEMPSTS  0x48
#define USB_FRMNUM   0x4C
#define USB_UFRMNUM  0x4E

static bool rza1l_usb_is_status_reg(hwaddr offset)
{
    switch (offset) {
    case USB_SYSSTS0:
    case USB_INTSTS0:
    case USB_INTSTS1:
    case USB_BRDYSTS:
    case USB_NRDYSTS:
    case USB_BEMPSTS:
    case USB_FRMNUM:
    case USB_UFRMNUM:
        return true;
    default:
        return false;
    }
}

static uint16_t rza1l_usb_read16(RzA1lUsbState *s, hwaddr offset)
{
    if (rza1l_usb_is_status_reg(offset)) {
        return 0;
    }
    return s->regs[offset >> 1];
}

static uint64_t rza1l_usb_read(void *opaque, hwaddr offset, unsigned size)
{
    RzA1lUsbState *s = opaque;
    uint64_t value;

    switch (size) {
    case 1: {
        uint16_t word = rza1l_usb_read16(s, offset & ~1u);
        value = (offset & 1) ? (word >> 8) : (word & 0xff);
        break;
    }
    case 2:
        value = rza1l_usb_read16(s, offset);
        break;
    case 4:
        value = rza1l_usb_read16(s, offset) |
                ((uint32_t)rza1l_usb_read16(s, offset + 2) << 16);
        break;
    default:
        value = 0;
        break;
    }
    return value;
}

static void rza1l_usb_write16(RzA1lUsbState *s, hwaddr offset, uint16_t value)
{
    /* Status registers are read-only / write-1-to-clear; keep them at zero. */
    if (rza1l_usb_is_status_reg(offset)) {
        return;
    }
    s->regs[offset >> 1] = value;
}

static void rza1l_usb_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    RzA1lUsbState *s = opaque;

    switch (size) {
    case 1: {
        hwaddr word_off = offset & ~1u;
        uint16_t word = s->regs[word_off >> 1];
        if (offset & 1) {
            word = (word & 0x00ff) | ((value & 0xff) << 8);
        } else {
            word = (word & 0xff00) | (value & 0xff);
        }
        rza1l_usb_write16(s, word_off, word);
        break;
    }
    case 2:
        rza1l_usb_write16(s, offset, value);
        break;
    case 4:
        rza1l_usb_write16(s, offset, value & 0xffff);
        rza1l_usb_write16(s, offset + 2, (value >> 16) & 0xffff);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps rza1l_usb_ops = {
    .read = rza1l_usb_read,
    .write = rza1l_usb_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void rza1l_usb_reset(DeviceState *dev)
{
    RzA1lUsbState *s = RZA1L_USB(dev);

    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
}

static void rza1l_usb_realize(DeviceState *dev, Error **errp)
{
    RzA1lUsbState *s = RZA1L_USB(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rza1l_usb_ops, s,
                          TYPE_RZA1L_USB, RZA1L_USB_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription vmstate_rza1l_usb = {
    .name = TYPE_RZA1L_USB,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(regs, RzA1lUsbState, RZA1L_USB_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void rza1l_usb_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rza1l_usb_realize;
    dc->vmsd = &vmstate_rza1l_usb;
    device_class_set_legacy_reset(dc, rza1l_usb_reset);
}

static const TypeInfo rza1l_usb_info = {
    .name = TYPE_RZA1L_USB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RzA1lUsbState),
    .class_init = rza1l_usb_class_init,
};

static void rza1l_usb_register_types(void)
{
    type_register_static(&rza1l_usb_info);
}

type_init(rza1l_usb_register_types)
