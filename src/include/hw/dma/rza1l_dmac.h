/*
 * Renesas RZ/A1 DMAC (Direct Memory Access Controller) — minimal model
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DMA_RZA1L_DMAC_H
#define HW_DMA_RZA1L_DMAC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RZA1L_DMAC "rza1l-dmac"
OBJECT_DECLARE_SIMPLE_TYPE(RzA1lDmacState, RZA1L_DMAC)

/* 16 channels split into two groups of eight. */
#define RZA1L_DMAC_NUM_CH 16

/* MMIO window covers both channel groups and their common registers. */
#define RZA1L_DMAC_MMIO_SIZE 0x800

typedef struct RzA1lDmacChannel {
    uint32_t n0sa;   /* next-0 source address */
    uint32_t n0da;   /* next-0 destination address */
    uint32_t n0tb;   /* next-0 transfer byte count */
    uint32_t n1sa;
    uint32_t n1da;
    uint32_t n1tb;
    uint32_t crsa;   /* current source address (read-only) */
    uint32_t crda;   /* current destination address (read-only) */
    uint32_t crtb;   /* current transfer byte count (read-only) */
    uint32_t chstat; /* channel status (read-only) */
    uint32_t chcfg;  /* channel configuration */
    uint32_t chitvl;
    uint32_t chext;
    uint32_t nxla;
    uint32_t crla;

    /*
     * Link/register-mode peripheral receive ring (e.g. a SCIF RX channel).
     * When such a channel is enabled the descriptor's destination buffer is
     * latched here so a peripheral can deliver bytes one at a time, advancing
     * CRDA around the ring (see rza1l_dmac_peripheral_rx_push). Only channels
     * registered via rza1l_dmac_register_rx_ring are treated this way.
     */
    bool     rx_ring_peripheral;
    uint32_t rx_ring_base;
    uint32_t rx_ring_size;
    bool     rx_ring_active;

    /*
     * Audio playback ring (the SSI transmit channel). The SSI continuously
     * streams the firmware's TX buffer to the audio codec, and the firmware
     * uses the channel's current source address (CRSA) as its master audio
     * sample clock. Rather than transfer instantly, this channel's CRSA is
     * advanced from virtual time at the audio sample rate so the firmware's
     * audioSampleTimer (and every UI timer derived from it) keeps ticking.
     * Only channels registered via rza1l_dmac_register_tx_audio_ring behave
     * this way.
     */
    bool     tx_audio_ring;
    bool     tx_audio_active;
    uint32_t tx_audio_base;
    uint32_t tx_audio_size;
    int64_t  tx_audio_start_ns;

    /*
     * Audio capture ring (the SSI receive channel). Symmetric to the transmit
     * ring above: the SSI continuously writes incoming codec samples into the
     * firmware's RX buffer, and the firmware reads the channel's current
     * destination address (CRDA) to track the live input write position and
     * resynchronise its read latency. CRDA is advanced from virtual time at the
     * audio sample rate. Only channels registered via
     * rza1l_dmac_register_rx_audio_ring behave this way.
     */
    bool     rx_audio_ring;
    bool     rx_audio_active;
    uint32_t rx_audio_base;
    uint32_t rx_audio_size;
    int64_t  rx_audio_start_ns;
} RzA1lDmacChannel;

struct RzA1lDmacState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;

    RzA1lDmacChannel ch[RZA1L_DMAC_NUM_CH];

    /*
     * Per-channel transfer-end interrupt lines (DMAINT0..DMAINT15). Pulsed
     * when a transfer completes with the channel's end-interrupt unmasked
     * (CHCFG.DEM clear). These are wired to the GIC by the SoC.
     */
    qemu_irq irq[RZA1L_DMAC_NUM_CH];

    /* Group common control registers (DCTRL_0_7 / DCTRL_8_15), shadowed. */
    uint32_t dctrl[2];
};

/*
 * Mark a channel as a peripheral receive ring. When the firmware enables such
 * a channel with a self-linking link descriptor, its destination buffer is
 * latched so a peripheral can deliver bytes via rza1l_dmac_peripheral_rx_push.
 */
void rza1l_dmac_register_rx_ring(RzA1lDmacState *s, int ch);

/*
 * Mark a channel as the SSI audio transmit ring. When the firmware enables
 * such a channel with a self-linking link descriptor, its source buffer is
 * latched and its current source address (CRSA) is thereafter advanced from
 * virtual time at the audio sample rate, modelling continuous playback.
 */
void rza1l_dmac_register_tx_audio_ring(RzA1lDmacState *s, int ch);

/*
 * Report the live geometry of a TX audio ring channel. When the channel has
 * been armed by the firmware (a self-linking audio descriptor), stores the
 * ring's guest base address and byte size and returns true; otherwise returns
 * false. Used by the SSI model to mirror the transmit buffer to host audio.
 */
bool rza1l_dmac_get_tx_audio_ring(RzA1lDmacState *s, int ch,
                                  uint32_t *base, uint32_t *size);

/*
 * Mark a channel as the SSI audio receive ring. When the firmware enables such
 * a channel with a self-linking descriptor, its destination buffer is latched
 * and its current destination address (CRDA) is thereafter advanced from
 * virtual time at the audio sample rate, modelling continuous capture.
 */
void rza1l_dmac_register_rx_audio_ring(RzA1lDmacState *s, int ch);

/*
 * Report the live geometry of an RX audio ring channel. When the channel has
 * been armed by the firmware, stores the ring's guest base address and byte
 * size and returns true; otherwise returns false.
 */
bool rza1l_dmac_get_rx_audio_ring(RzA1lDmacState *s, int ch,
                                  uint32_t *base, uint32_t *size);

/*
 * Return the live current destination address (CRDA) of an RX audio ring,
 * i.e. the position at which the SSI is currently writing captured samples.
 * Returns false if the channel is not an armed RX audio ring.
 */
bool rza1l_dmac_get_rx_audio_crda(RzA1lDmacState *s, int ch, uint32_t *crda);

/*
 * Deliver one byte from a peripheral into a link-mode receive-ring channel.
 * Writes the byte at the channel's current destination address (CRDA) in
 * guest memory and advances CRDA by one, wrapping within the descriptor's
 * destination buffer. Returns true if the channel was an armed receive ring.
 */
bool rza1l_dmac_peripheral_rx_push(RzA1lDmacState *s, int ch, uint8_t byte);

#endif /* HW_DMA_RZA1L_DMAC_H */
