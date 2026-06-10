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
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "hw/gpio/rza1l_gpio.h"

/* Port pin read registers: PPR1 (port 1) at 0x200, stride 4 up to port 11. */
#define GPIO_PPR_BASE 0x200
#define GPIO_PPR_END  0x230 /* PPR1..PPR11 -> 0x200 + 11*4 */
/* Port output data registers: P1 at 0x000, same stride. */
#define GPIO_P_BASE   0x000

/*
 * One rotary encoder's wiring, mirroring the firmware's kEncoderIrqMap
 * (hid/encoders.cpp). All pins are on port 1. The A-side pin is routed to the
 * external IRQn (both edges); the companion B-side pin is read as GPIO inside
 * the ISR to determine direction. invert flips the direction sense.
 */
typedef struct RzA1lEncoderMap {
    uint8_t irq_pin;
    uint8_t comp_pin;
    uint8_t irq_num;
    bool invert;
} RzA1lEncoderMap;

static const RzA1lEncoderMap rza1l_encoder_map[DELUGE_ENC_COUNT] = {
    [DELUGE_ENC_SCROLL_Y] = { .irq_pin = 8,  .comp_pin = 10, .irq_num = 0, .invert = false },
    [DELUGE_ENC_SCROLL_X] = { .irq_pin = 11, .comp_pin = 12, .irq_num = 3, .invert = false },
    [DELUGE_ENC_TEMPO]    = { .irq_pin = 6,  .comp_pin = 7,  .irq_num = 2, .invert = true  },
    [DELUGE_ENC_SELECT]   = { .irq_pin = 3,  .comp_pin = 2,  .irq_num = 7, .invert = false },
    [DELUGE_ENC_MOD_1]    = { .irq_pin = 5,  .comp_pin = 4,  .irq_num = 1, .invert = false },
    [DELUGE_ENC_MOD_0]    = { .irq_pin = 0,  .comp_pin = 15, .irq_num = 4, .invert = false },
};

/* Spacing between successive quadrature edges (virtual time). */
#define RZA1L_GPIO_EDGE_SPACING_NS 50000 /* 50us: ISR runs between edges */

/*
 * Acknowledge a pending edge-mode external IRQ when its handler reads the
 * encoder pins through PPR1. Some firmwares configure the encoder IRQs as
 * edge-detect (ICR1 != 0) and acknowledge only at the GIC, never writing
 * IRQRR; for those the level-held line must be released some other way or the
 * CPU storms on re-pend after every EOI. The ISR reads the encoder's A/B pins
 * via PPR1, and because a pending+enabled IRQ is taken before the main loop's
 * next poll, the first PPR1 read of a pending IRQ's pin byte is that ISR. Clear
 * the pending bit and drop the line there. Low-level IRQs (ICR1 field 0) keep
 * their held semantics and are untouched.
 *
 * port is 1-based; byte_in_word selects pins 0-7 (0) or 8-15 (1) of that port.
 */
static void rza1l_gpio_irq_read_ack(RzA1lGpioState *s, unsigned port,
                                    unsigned byte_in_word)
{
    if (port != 1 || byte_in_word > 1 || !s->irqrr || s->irqrr_ack_seen) {
        return;
    }

    for (unsigned e = 0; e < DELUGE_ENC_COUNT; e++) {
        const RzA1lEncoderMap *m = &rza1l_encoder_map[e];
        unsigned bit = 1u << m->irq_num;
        unsigned field = (s->icr1 >> (2u * m->irq_num)) & 0x3u;

        if ((s->irqrr & bit) && field != 0 &&
            (m->irq_pin / 8u) == byte_in_word) {
            s->irqrr &= (uint16_t)~bit;
            qemu_set_irq(s->irq[m->irq_num], 0);
        }
    }
}

static uint64_t rza1l_gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    RzA1lGpioState *s = opaque;
    uint64_t val = 0;

    for (unsigned i = 0; i < size; i++) {
        hwaddr boff = offset + i;
        hwaddr src = boff;
        uint8_t byte;

        /*
         * Reading a port pin register reflects the output latch of the same
         * port (loopback). Undriven pins read 0.
         */
        if (boff >= GPIO_PPR_BASE && boff < GPIO_PPR_END) {
            src = GPIO_P_BASE + (boff - GPIO_PPR_BASE);
        }

        byte = (src < RZA1L_GPIO_MMIO_SIZE) ? s->regs[src] : 0;

        /*
         * For port pin registers, overlay any host-injected input bits (rotary
         * encoders) on top of the latch loopback.
         */
        if (boff >= GPIO_PPR_BASE && boff < GPIO_PPR_END) {
            hwaddr rel = boff - GPIO_PPR_BASE;
            unsigned port = rel / 4 + 1;
            unsigned byte_in_word = rel % 4;

            if (port < RZA1L_GPIO_NUM_PORTS && byte_in_word < 2) {
                uint8_t mask = (s->in_mask[port] >> (8 * byte_in_word)) & 0xff;
                uint8_t drive = (s->in_drive[port] >> (8 * byte_in_word)) & 0xff;
                byte = (byte & ~mask) | (drive & mask);
            }
            /* An ISR reading the encoder pin acknowledges its edge-mode IRQ. */
            rza1l_gpio_irq_read_ack(s, port, byte_in_word);
        }

        val |= (uint64_t)byte << (8 * i);
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

/* Drive a host-controlled input pin level on a given port (1-based). */
static void rza1l_gpio_set_input_pin(RzA1lGpioState *s, unsigned port,
                                     unsigned pin, bool level)
{
    if (port >= RZA1L_GPIO_NUM_PORTS || pin >= 16) {
        return;
    }
    s->in_mask[port] |= (uint16_t)(1u << pin);
    if (level) {
        s->in_drive[port] |= (uint16_t)(1u << pin);
    } else {
        s->in_drive[port] &= (uint16_t)~(1u << pin);
    }
}

/*
 * Apply one quadrature phase step for an encoder. The encoder's (A,B) lines
 * follow a 4-state Gray code:
 *
 *   phase:  0     1     2     3
 *   A:      0     1     1     0
 *   B:      0     0     1     1
 *
 * so A = ((phase+1) >> 1) & 1 and B = (phase >> 1) & 1. Stepping the phase by
 * +/-1 changes exactly one of A/B — a real quadrature transition. Only the
 * steps that move the A (IRQ) pin raise the external IRQn; the firmware samples
 * B at that A-edge to decode direction:
 *   cw = invert ? (a != b) : (a == b);  inc = cw ? +1 : -1.
 * Stepping the phase FORWARD makes both A-edges read a != b; stepping BACKWARD
 * makes both read a == b. So to have the firmware register direction `dir` we
 * step the phase by (invert ? dir : -dir).
 *
 * The IRQ line is held (not pulsed): the firmware configures these SPIs as
 * level-triggered in the GIC, so a momentary pulse would be lost. It is
 * deasserted either when the firmware clears IRQRR (rza1l_intc_write) or, for
 * edge-detect firmwares that never touch IRQRR, when the ISR reads the encoder
 * pin via PPR1 (rza1l_gpio_irq_read_ack).
 */
static void rza1l_gpio_apply_edge(RzA1lGpioState *s, unsigned enc, int dir)
{
    const RzA1lEncoderMap *m;
    unsigned old_phase;
    unsigned new_phase;
    int pstep;
    bool old_a;
    bool new_a;
    bool new_b;

    if (enc >= DELUGE_ENC_COUNT) {
        return;
    }
    m = &rza1l_encoder_map[enc];

    pstep = (m->invert ? dir : -dir) > 0 ? 1 : 3; /* +1 or -1 (mod 4) */

    old_phase = (s->enc_phase >> (2u * enc)) & 0x3u;
    new_phase = (old_phase + (unsigned)pstep) & 0x3u;

    old_a = ((old_phase + 1) >> 1) & 1;
    new_a = ((new_phase + 1) >> 1) & 1;
    new_b = (new_phase >> 1) & 1;

    rza1l_gpio_set_input_pin(s, 1, m->irq_pin, new_a);
    rza1l_gpio_set_input_pin(s, 1, m->comp_pin, new_b);

    s->enc_phase &= (uint16_t)~(0x3u << (2u * enc));
    s->enc_phase |= (uint16_t)(new_phase << (2u * enc));

    /* Only an A-pin transition is an IRQ edge; B-only steps are silent. */
    if (new_a != old_a && m->irq_num < RZA1L_GPIO_NUM_IRQ) {
        s->irqrr |= (uint16_t)(1u << m->irq_num);
        qemu_set_irq(s->irq[m->irq_num], 1);
    }
}

static void rza1l_gpio_edge_cb(void *opaque)
{
    RzA1lGpioState *s = opaque;

    if (s->edge_head == s->edge_tail) {
        return; /* queue empty */
    }

    rza1l_gpio_apply_edge(s, s->edge_q[s->edge_head].enc,
                          s->edge_q[s->edge_head].dir);
    s->edge_head = (s->edge_head + 1) % RZA1L_GPIO_EDGE_QUEUE;

    /* If more edges remain, schedule the next one after the ISR has run. */
    if (s->edge_head != s->edge_tail) {
        timer_mod(s->edge_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      RZA1L_GPIO_EDGE_SPACING_NS);
    }
}

static void rza1l_gpio_enqueue_edge(RzA1lGpioState *s, unsigned enc, int dir)
{
    int next = (s->edge_tail + 1) % RZA1L_GPIO_EDGE_QUEUE;

    if (next == s->edge_head) {
        /* Queue full: drop the edge rather than overrun. */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rza1l-gpio: encoder edge queue full, dropping step\n");
        return;
    }

    s->edge_q[s->edge_tail].enc = enc;
    s->edge_q[s->edge_tail].dir = dir;
    s->edge_tail = next;

    if (!timer_pending(s->edge_timer)) {
        timer_mod(s->edge_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      RZA1L_GPIO_EDGE_SPACING_NS);
    }
}

void rza1l_gpio_encoder_step(DeviceState *dev, int enc, int dir)
{
    RzA1lGpioState *s = RZA1L_GPIO(dev);

    if (enc < 0 || enc >= DELUGE_ENC_COUNT || dir == 0) {
        return;
    }

    dir = (dir > 0) ? 1 : -1;

    /*
     * One detent = one full quadrature cycle = four Gray-code phase steps,
     * which produce two A-pin edges (IRQs) with a B transition between them.
     */
    rza1l_gpio_enqueue_edge(s, (unsigned)enc, dir);
    rza1l_gpio_enqueue_edge(s, (unsigned)enc, dir);
    rza1l_gpio_enqueue_edge(s, (unsigned)enc, dir);
    rza1l_gpio_enqueue_edge(s, (unsigned)enc, dir);
}

bool rza1l_gpio_get_output_pin(DeviceState *dev, unsigned port, unsigned pin)
{
    RzA1lGpioState *s = RZA1L_GPIO(dev);
    hwaddr off;

    if (port < 1 || port >= RZA1L_GPIO_NUM_PORTS || pin > 15) {
        return false;
    }

    /* Output latch: port p data register at (p-1)*4, low byte = pins 0..7. */
    off = GPIO_P_BASE + (port - 1) * 4 + (pin / 8);
    if (off >= RZA1L_GPIO_MMIO_SIZE) {
        return false;
    }

    return (s->regs[off] >> (pin % 8)) & 1;
}

static const MemoryRegionOps rza1l_gpio_ops = {
    .read = rza1l_gpio_read,
    .write = rza1l_gpio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/*
 * INTC external-IRQ pin registers, based at 0xFCFEF800:
 *   0x00 ICR0   IRQ sense/NMI control (shadowed; unused by us)
 *   0x02 ICR1   per-IRQn edge mode (shadowed; firmware sets both-edge)
 *   0x04 IRQRR  IRQ request register: bit n latches IRQn pending
 *
 * The firmware's clearIRQInterrupt(n) reads IRQRR, and if bit n is set,
 * writes it back with bit n cleared. We follow the hardware: clearing a
 * pending bit deasserts the corresponding (level-held) GIC line.
 */
static uint64_t rza1l_intc_read(void *opaque, hwaddr offset, unsigned size)
{
    RzA1lGpioState *s = opaque;

    switch (offset) {
    case 0x00:
        return s->icr0;
    case 0x02:
        return s->icr1;
    case 0x04:
        return s->irqrr;
    default:
        return 0;
    }
}

static void rza1l_intc_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    RzA1lGpioState *s = opaque;

    switch (offset) {
    case 0x00:
        s->icr0 = (uint16_t)value;
        break;
    case 0x02:
        s->icr1 = (uint16_t)value;
        break;
    case 0x04: {
        uint16_t newv = (uint16_t)value;
        uint16_t cleared = s->irqrr & ~newv; /* bits going 1 -> 0 */

        if (cleared) {
            /* This firmware acks via IRQRR; disable the read-ack fallback. */
            s->irqrr_ack_seen = true;
        }
        s->irqrr = newv;
        /* Deassert the level-held line for each acknowledged IRQn. */
        for (int n = 0; n < RZA1L_GPIO_NUM_IRQ; n++) {
            if (cleared & (1u << n)) {
                qemu_set_irq(s->irq[n], 0);
            }
        }
        break;
    }
    default:
        break;
    }
}

static const MemoryRegionOps rza1l_intc_ops = {
    .read = rza1l_intc_read,
    .write = rza1l_intc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void rza1l_gpio_reset(DeviceState *dev)
{
    RzA1lGpioState *s = RZA1L_GPIO(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->in_drive, 0, sizeof(s->in_drive));
    memset(s->in_mask, 0, sizeof(s->in_mask));
    s->enc_phase = 0;
    s->icr0 = 0;
    s->icr1 = 0;
    s->irqrr = 0;
    s->irqrr_ack_seen = false;
    s->edge_head = 0;
    s->edge_tail = 0;
    if (s->edge_timer) {
        timer_del(s->edge_timer);
    }
    for (int i = 0; i < RZA1L_GPIO_NUM_IRQ; i++) {
        if (s->irq[i]) {
            qemu_set_irq(s->irq[i], 0);
        }
    }
}

static void rza1l_gpio_realize(DeviceState *dev, Error **errp)
{
    RzA1lGpioState *s = RZA1L_GPIO(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rza1l_gpio_ops, s,
                          TYPE_RZA1L_GPIO, RZA1L_GPIO_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    /* Second MMIO window: the INTC external-IRQ pin registers (ICR0/1, IRQRR). */
    memory_region_init_io(&s->intc_iomem, OBJECT(dev), &rza1l_intc_ops, s,
                          TYPE_RZA1L_GPIO ".intc", RZA1L_INTC_IRQ_SIZE);
    sysbus_init_mmio(sbd, &s->intc_iomem);

    for (int i = 0; i < RZA1L_GPIO_NUM_IRQ; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }

    s->edge_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rza1l_gpio_edge_cb, s);
}

static void rza1l_gpio_unrealize(DeviceState *dev)
{
    RzA1lGpioState *s = RZA1L_GPIO(dev);

    if (s->edge_timer) {
        timer_del(s->edge_timer);
        timer_free(s->edge_timer);
        s->edge_timer = NULL;
    }
}

static const VMStateDescription vmstate_rza1l_gpio = {
    .name = TYPE_RZA1L_GPIO,
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, RzA1lGpioState, RZA1L_GPIO_MMIO_SIZE),
        VMSTATE_UINT16_ARRAY(in_drive, RzA1lGpioState, RZA1L_GPIO_NUM_PORTS),
        VMSTATE_UINT16_ARRAY(in_mask, RzA1lGpioState, RZA1L_GPIO_NUM_PORTS),
        VMSTATE_UINT16(enc_phase, RzA1lGpioState),
        VMSTATE_UINT16(icr0, RzA1lGpioState),
        VMSTATE_UINT16(icr1, RzA1lGpioState),
        VMSTATE_UINT16(irqrr, RzA1lGpioState),
        VMSTATE_END_OF_LIST()
    },
};

static void rza1l_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rza1l_gpio_realize;
    dc->unrealize = rza1l_gpio_unrealize;
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
