/*
 * Renesas RZ/A1 GPIO (general-purpose I/O ports) — minimal model
 *
 * The Deluge firmware drives output pins through the per-port data registers
 * (P_n), configures pin direction/function through PM/PMC/PFC/PIBC/etc., and
 * reads input pin state through the port pin registers (PPR_n). During boot it
 * busy-polls PPR1 (port 1) for the rotary encoders, buttons and the various
 * detect lines (headphone/mic/line/output), e.g.
 *
 *   uint16_t readInput(uint8_t p, uint8_t q)  // -> GPIO.PPR1[(p-1)*4] bit q
 *
 * This model shadows every register write and loops the output latch back to
 * the corresponding PPR_n on read, so a pin driven high reads high and an
 * undriven input reads 0 (matching the Deluge's "nothing plugged in / no
 * button pressed" idle state). Pin multiplexing, bidirectional buffers and
 * interrupt detection are not modelled — only the data path the firmware polls.
 *
 * Register layout (verified via offsetof on the firmware's st_gpio; the block
 * is based at 0xFCFE3004):
 *   P_n   0x000  PSR_n 0x100  PPR_n 0x1FC  PM_n  0x300  PMC_n 0x400
 *   PFC_n 0x500  PIBC  0x4000 PBDC  0x4100 PIPC  0x4200
 * Each port register is 4 bytes apart with the 16-bit value in the low half.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/gpio/rza1l_gpio.h"

/* Port pin read registers: PPR1 (port 1) at 0x200, stride 4 up to port 11. */
#define GPIO_PPR_BASE 0x200
#define GPIO_PPR_END  0x230 /* PPR1..PPR11 -> 0x200 + 11*4 */
/* Port output data registers: P1 at 0x000, same stride. */
#define GPIO_P_BASE   0x000

static uint64_t rza1l_gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    RzA1lGpioState *s = opaque;
    uint64_t val = 0;

    for (unsigned i = 0; i < size; i++) {
        hwaddr boff = offset + i;
        hwaddr src = boff;

        /*
         * Reading a port pin register reflects the output latch of the same
         * port (loopback). Undriven pins read 0.
         */
        if (boff >= GPIO_PPR_BASE && boff < GPIO_PPR_END) {
            src = GPIO_P_BASE + (boff - GPIO_PPR_BASE);
        }

        if (src < RZA1L_GPIO_MMIO_SIZE) {
            val |= (uint64_t)s->regs[src] << (8 * i);
        }
    }

    return val;
}

static void rza1l_gpio_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    RzA1lGpioState *s = opaque;

    for (unsigned i = 0; i < size; i++) {
        hwaddr boff = offset + i;

        /* Port pin registers are read-only; ignore writes to them. */
        if (boff >= GPIO_PPR_BASE && boff < GPIO_PPR_END) {
            continue;
        }
        if (boff < RZA1L_GPIO_MMIO_SIZE) {
            s->regs[boff] = (value >> (8 * i)) & 0xff;
        }
    }
}

static const MemoryRegionOps rza1l_gpio_ops = {
    .read = rza1l_gpio_read,
    .write = rza1l_gpio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void rza1l_gpio_reset(DeviceState *dev)
{
    RzA1lGpioState *s = RZA1L_GPIO(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void rza1l_gpio_realize(DeviceState *dev, Error **errp)
{
    RzA1lGpioState *s = RZA1L_GPIO(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rza1l_gpio_ops, s,
                          TYPE_RZA1L_GPIO, RZA1L_GPIO_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_rza1l_gpio = {
    .name = TYPE_RZA1L_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, RzA1lGpioState, RZA1L_GPIO_MMIO_SIZE),
        VMSTATE_END_OF_LIST()
    },
};

static void rza1l_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rza1l_gpio_realize;
    dc->vmsd = &vmstate_rza1l_gpio;
    device_class_set_legacy_reset(dc, rza1l_gpio_reset);
}

static const TypeInfo rza1l_gpio_info = {
    .name          = TYPE_RZA1L_GPIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RzA1lGpioState),
    .class_init    = rza1l_gpio_class_init,
};

static void rza1l_gpio_register_types(void)
{
    type_register_static(&rza1l_gpio_info);
}

type_init(rza1l_gpio_register_types)
