/*
 * Renesas RZ/A1 GPIO (general-purpose I/O ports) — minimal model
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_GPIO_RZA1L_GPIO_H
#define HW_GPIO_RZA1L_GPIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RZA1L_GPIO "rza1l-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(RzA1lGpioState, RZA1L_GPIO)

/*
 * The st_gpio register block spans P1 (offset 0) through the PIPC/port-buffer
 * registers near 0x4200. 0x4F00 covers the whole structure.
 */
#define RZA1L_GPIO_MMIO_SIZE 0x4F00

/* Number of GPIO ports (P1..P11); index 0 is unused, slots 1..11 are valid. */
#define RZA1L_GPIO_NUM_PORTS 12

/*
 * The six rotary encoders the firmware reads directly off the RZ/A1L (not via
 * the PIC). Each has an A-side pin routed via PFC alt-2 to an external IRQn and
 * a B-side companion pin read as plain GPIO inside the ISR. The enum order
 * mirrors the firmware's kEncoderIrqMap (hid/encoders.cpp).
 */
typedef enum DelugeEncoder {
    DELUGE_ENC_SCROLL_Y = 0,
    DELUGE_ENC_SCROLL_X,
    DELUGE_ENC_TEMPO,
    DELUGE_ENC_SELECT,
    DELUGE_ENC_MOD_1,
    DELUGE_ENC_MOD_0,
    DELUGE_ENC_COUNT
} DelugeEncoder;

/* Eight external IRQ lines (IRQ0..IRQ7) exposed as sysbus IRQ outputs. */
#define RZA1L_GPIO_NUM_IRQ 8

/* Pending quadrature-edge FIFO depth (2 edges per detent; queue several). */
#define RZA1L_GPIO_EDGE_QUEUE 64

/*
 * RZ/A1 INTC external-IRQ pin registers (ICR0/ICR1/IRQRR), at 0xFCFEF800 —
 * a separate block from the GIC distributor. The firmware configures edge
 * sensing in ICR1 and acknowledges each external IRQ by clearing the matching
 * bit in IRQRR. We model just enough of these so a host-driven encoder edge
 * raises the (level-triggered) IRQn line and the firmware's ISR can deassert
 * it, exactly as the real INTC pin latch does.
 */
#define RZA1L_INTC_IRQ_BASE 0xFCFEF800
#define RZA1L_INTC_IRQ_SIZE 0x10

struct RzA1lGpioState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion intc_iomem;
    uint8_t regs[RZA1L_GPIO_MMIO_SIZE];

    /* INTC external-IRQ pin registers (16-bit shadows). */
    uint16_t icr0;
    uint16_t icr1;
    uint16_t irqrr;

    /*
     * True once the firmware has acknowledged an external IRQ by clearing a
     * bit in IRQRR. Such firmwares hold the level-triggered line until that
     * write, so the read-ack fallback (which releases the line when the ISR
     * reads the encoder pin) must stay disabled for them. Firmwares that only
     * acknowledge at the GIC never set this and rely on the read-ack.
     */
    bool irqrr_ack_seen;

    /*
     * Host-injected input pin levels, overlaid on the output-latch loopback
     * when a port pin register (PPR_n) is read. in_mask marks which bits the
     * host drives; in_drive holds their levels. Indexed by port number (1..11).
     */
    uint16_t in_drive[RZA1L_GPIO_NUM_PORTS];
    uint16_t in_mask[RZA1L_GPIO_NUM_PORTS];

    /* External IRQ0..IRQ7 outputs to the INTC/GIC. */
    qemu_irq irq[RZA1L_GPIO_NUM_IRQ];

    /*
     * Current quadrature phase per encoder: a 2-bit Gray-code index (0..3)
     * packed two bits per encoder (encoder e at bits [2e+1:2e]). Stepping the
     * phase by one changes exactly one of the A/B pins, so the firmware sees a
     * real quadrature waveform (only one line transitions per edge) rather than
     * a simultaneous A+B toggle. This matters for firmwares that decode with a
     * stateful quadrature state machine (which rejects diagonal transitions).
     */
    uint16_t enc_phase;

    /*
     * Deferred quadrature-edge queue. Each detent enqueues four phase steps; a
     * timer applies them one at a time so the firmware's IRQ handler runs (and
     * the GIC's edge latch re-arms) between the two A-pin edges of the cycle.
     */
    struct {
        uint8_t enc;
        int8_t dir;
    } edge_q[RZA1L_GPIO_EDGE_QUEUE];
    int edge_head;
    int edge_tail;
    QEMUTimer *edge_timer;
};

/*
 * Host API: advance encoder `enc` (a DelugeEncoder) by one detent in direction
 * `dir` (+1 / -1). Generates a full Gray-code A/B quadrature cycle (two A-pin
 * edges, one B transition between) so the firmware's ISR reads the direction.
 */
void rza1l_gpio_encoder_step(DeviceState *dev, int enc, int dir);

/*
 * Read the current output-latch level of a GPIO pin (port 1..11, pin 0..15).
 * Used by the skin renderer to reflect firmware-driven panel LEDs such as the
 * Synced LED on P6_7.
 */
bool rza1l_gpio_get_output_pin(DeviceState *dev, unsigned port, unsigned pin);

#endif /* HW_GPIO_RZA1L_GPIO_H */
