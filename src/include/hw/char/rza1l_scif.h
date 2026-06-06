/*
 * Renesas RZ/A1 SCIF (serial communication interface with FIFO)
 *
 * Models one SCIF channel as a UART: synchronous transmit to a character
 * backend and interrupt-driven receive. Register-accurate enough for the
 * firmware's serial setup and byte-level I/O.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHAR_RZA1L_SCIF_H
#define HW_CHAR_RZA1L_SCIF_H

#include "hw/core/sysbus.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

#define TYPE_RZA1L_SCIF "rza1l-scif"
OBJECT_DECLARE_SIMPLE_TYPE(RzA1lScifState, RZA1L_SCIF)

/* MMIO register window for one SCIF channel. */
#define RZA1L_SCIF_MMIO_SIZE 0x100

struct RzA1lDmacState;

struct RzA1lScifState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    CharFrontend chr;
    qemu_irq irq;       /* receive (RXI) interrupt to the GIC */

    uint16_t scsmr;     /* serial mode                    */
    uint8_t  scbrr;     /* bit rate                       */
    uint16_t scscr;     /* serial control (TIE/RIE/TE/RE) */
    uint16_t scfsr;     /* serial status (shadow)         */
    uint16_t scfcr;     /* FIFO control                   */

    uint8_t  rx_fifo;   /* single receive holding byte    */
    bool     rx_full;   /* receive byte pending           */

    /*
     * Optional receive-DMA binding. When set, incoming bytes are pushed into
     * the DMAC's receive ring (the MIDI UART is read via DMA, with the
     * firmware polling CRDA) instead of latched into the single holding byte.
     */
    struct RzA1lDmacState *dmac;
    int rx_dma_channel;
};

/* Route received bytes into a DMAC receive ring (MIDI input via DMA). */
void rza1l_scif_set_rx_dma(RzA1lScifState *s, struct RzA1lDmacState *dmac,
                           int rx_dma_channel);

#endif /* HW_CHAR_RZA1L_SCIF_H */

