/*
 * Deluge input PIC coprocessor — serial-link model
 *
 * The Deluge offloads pad/button/encoder scanning and RGB LED / 7-segment /
 * OLED driving to a dedicated PIC microcontroller that talks to the main SoC
 * over a UART (SCIF channel 1). This models the PIC end of that link: it is a
 * QEMU character backend wired to the SCIF1 frontend, so every byte the
 * firmware transmits to the PIC arrives at deluge_pic's chr_write, and replies
 * are delivered straight into the firmware's receive DMA ring.
 *
 * This file implements the link + framing layer and the boot handshake
 * (baud-rate switch and firmware-version query). Decoding the full command
 * stream and synthesising input events are layered on top separately.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_DELUGE_PIC_H
#define HW_MISC_DELUGE_PIC_H

#include "chardev/char.h"
#include "qom/object.h"

struct RzA1lDmacState;

#define TYPE_DELUGE_PIC "chardev-deluge-pic"
typedef struct DelugePicState DelugePicState;
DECLARE_INSTANCE_CHECKER(DelugePicState, DELUGE_PIC, TYPE_DELUGE_PIC)

struct DelugePicState {
    /*< private >*/
    Chardev parent;

    /*< public >*/
    /*
     * The firmware receives PIC bytes via a DMA ring (SCFRDR -> picRxBuffer)
     * rather than by reading the SCIF data register, so responses are pushed
     * directly through the receive DMA channel. These are set by the board.
     */
    struct RzA1lDmacState *dmac;
    int rx_dma_channel;

    /* Framing state: payload bytes still expected for the current message. */
    int pending_payload;
    bool awaiting_baud;

    /* Last UART-speed divisor the firmware programmed (PIC message 225). */
    uint8_t baud_div;
};

/*
 * Bind the PIC to the receive DMA channel the firmware uses for PIC input,
 * and to the DMAC that owns it. Called by the board after both exist.
 */
void deluge_pic_set_dma(Chardev *chr, struct RzA1lDmacState *dmac,
                        int rx_dma_channel);

#endif /* HW_MISC_DELUGE_PIC_H */
