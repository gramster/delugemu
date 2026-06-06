/*
 * Renesas RZ/A1 MTU2 (Multi-Function Timer Pulse Unit 2) — free-running model
 *
 * The Deluge firmware uses the MTU2 16-bit timer counters (TCNT_n) as free-
 * running time bases for busy-wait delays and timeouts, e.g.
 *
 *   start = MTU2.TCNT_4;
 *   while ((uint16_t)(MTU2.TCNT_n - start) < ticks);
 *
 * This model makes every TCNT register advance off the virtual clock at the
 * MTU2 peripheral clock rate so those loops terminate. Channel control
 * (prescaler select in TCR, compare matches, interrupts) is not modelled: the
 * counters always run at the base peripheral clock, which is accurate enough
 * for the firmware's coarse millisecond/microsecond delays during boot.
 *
 * Register offsets (from the RZ/A1 hardware, verified via offsetof on the
 * firmware's st_mtu2):
 *   TCNT_2 0x006  TCNT_3 0x210  TCNT_4 0x212  TCNTS 0x220
 *   TCNT_0 0x306  TCNT_1 0x386
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/host-utils.h"
#include "qemu/timer.h"
#include "migration/vmstate.h"
#include "hw/timer/rza1l_mtu2.h"

/*
 * MTU2 input clock. The RZ/A1 clocks the MTU2 from the peripheral clock P0phi
 * (~33.33 MHz). The exact rate only affects how long delay loops spin in wall
 * time, not whether the firmware progresses.
 */
#define RZA1L_MTU2_CLK_HZ 33333333u

static const struct {
    hwaddr offset;
} rza1l_mtu2_tcnt[RZA1L_MTU2_NUM_TCNT] = {
    { 0x306 }, /* TCNT_0 */
    { 0x386 }, /* TCNT_1 */
    { 0x006 }, /* TCNT_2 */
    { 0x210 }, /* TCNT_3 */
    { 0x212 }, /* TCNT_4 */
    { 0x220 }, /* TCNTS  */
};

/* Return the TCNT channel index whose 16-bit register contains byte boff. */
static int rza1l_mtu2_tcnt_for_byte(hwaddr boff)
{
    for (int i = 0; i < RZA1L_MTU2_NUM_TCNT; i++) {
        hwaddr base = rza1l_mtu2_tcnt[i].offset;
        if (boff == base || boff == base + 1) {
            return i;
        }
    }
    return -1;
}

static uint16_t rza1l_mtu2_tcnt_value(RzA1lMtu2State *s, int idx)
{
    RzA1lMtu2Counter *c = &s->tcnt[idx];
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t elapsed = now - c->reset_ns;
    uint64_t ticks = muldiv64(elapsed, RZA1L_MTU2_CLK_HZ,
                              NANOSECONDS_PER_SECOND);

    return (uint16_t)(c->reset_val + ticks);
}

static uint64_t rza1l_mtu2_read(void *opaque, hwaddr offset, unsigned size)
{
    RzA1lMtu2State *s = opaque;
    uint64_t val = 0;

    for (unsigned i = 0; i < size; i++) {
        hwaddr boff = offset + i;
        int idx = rza1l_mtu2_tcnt_for_byte(boff);
        uint8_t b;

        if (idx >= 0) {
            uint16_t cnt = rza1l_mtu2_tcnt_value(s, idx);
            unsigned shift = (boff - rza1l_mtu2_tcnt[idx].offset) * 8;
            b = (cnt >> shift) & 0xff;
        } else {
            b = s->regs[boff];
        }
        val |= (uint64_t)b << (8 * i);
    }

    return val;
}

static void rza1l_mtu2_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    RzA1lMtu2State *s = opaque;

    for (unsigned i = 0; i < size; i++) {
        s->regs[offset + i] = (value >> (8 * i)) & 0xff;
    }

    /* A write that fully covers a TCNT register re-bases that counter. */
    for (int idx = 0; idx < RZA1L_MTU2_NUM_TCNT; idx++) {
        hwaddr base = rza1l_mtu2_tcnt[idx].offset;

        if (base >= offset && base + 2 <= offset + size) {
            s->tcnt[idx].reset_val = lduw_le_p(&s->regs[base]);
            s->tcnt[idx].reset_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
    }
}

static const MemoryRegionOps rza1l_mtu2_ops = {
    .read = rza1l_mtu2_read,
    .write = rza1l_mtu2_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void rza1l_mtu2_reset(DeviceState *dev)
{
    RzA1lMtu2State *s = RZA1L_MTU2(dev);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    memset(s->regs, 0, sizeof(s->regs));
    for (int i = 0; i < RZA1L_MTU2_NUM_TCNT; i++) {
        s->tcnt[i].reset_ns = now;
        s->tcnt[i].reset_val = 0;
    }
}

static void rza1l_mtu2_realize(DeviceState *dev, Error **errp)
{
    RzA1lMtu2State *s = RZA1L_MTU2(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rza1l_mtu2_ops, s,
                          TYPE_RZA1L_MTU2, RZA1L_MTU2_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_rza1l_mtu2_counter = {
    .name = "rza1l-mtu2-counter",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(reset_ns, RzA1lMtu2Counter),
        VMSTATE_UINT16(reset_val, RzA1lMtu2Counter),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_rza1l_mtu2 = {
    .name = TYPE_RZA1L_MTU2,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(tcnt, RzA1lMtu2State, RZA1L_MTU2_NUM_TCNT, 1,
                             vmstate_rza1l_mtu2_counter, RzA1lMtu2Counter),
        VMSTATE_UINT8_ARRAY(regs, RzA1lMtu2State, RZA1L_MTU2_MMIO_SIZE),
        VMSTATE_END_OF_LIST()
    },
};

static void rza1l_mtu2_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rza1l_mtu2_realize;
    dc->vmsd = &vmstate_rza1l_mtu2;
    device_class_set_legacy_reset(dc, rza1l_mtu2_reset);
}

static const TypeInfo rza1l_mtu2_info = {
    .name          = TYPE_RZA1L_MTU2,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RzA1lMtu2State),
    .class_init    = rza1l_mtu2_class_init,
};

static void rza1l_mtu2_register_types(void)
{
    type_register_static(&rza1l_mtu2_info);
}

type_init(rza1l_mtu2_register_types)
