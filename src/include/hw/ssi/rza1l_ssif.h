/*
 * Renesas RZ/A1 SSIF (serial sound interface with FIFO) — I2S audio
 *
 * Models SSIF channel 0 (0xE820B000), the I2S link the Deluge firmware uses for
 * its audio codec. The firmware does not service the SSI FIFOs from the CPU;
 * instead DMA channel 6 streams the transmit buffer to SSIFTDR and channel 7
 * drains SSIFRDR, while the firmware tracks playback position from the DMA
 * channel's current source address (modelled in the DMAC). This device
 * therefore presents a register-accurate control/status surface so the
 * firmware's SSI setup completes, and (when an audio backend is attached) it
 * mirrors the transmit ring to QEMU's audio subsystem and feeds the receive
 * ring from capture/silence.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SSI_RZA1L_SSIF_H
#define HW_SSI_RZA1L_SSIF_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/timer.h"
#include "qemu/audio.h"

#define TYPE_RZA1L_SSIF "rza1l-ssif"
OBJECT_DECLARE_SIMPLE_TYPE(RzA1lSsifState, RZA1L_SSIF)

/* MMIO window for one SSIF channel (channels are 0x800 apart). */
#define RZA1L_SSIF_MMIO_SIZE 0x800

/* Audio format the firmware drives: 44.1 kHz, stereo, 32-bit system word. */
#define RZA1L_SSIF_SAMPLE_RATE   44100
#define RZA1L_SSIF_NUM_CHANNELS  2

struct RzA1lDmacState;

struct RzA1lSsifState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;

    /*
     * FIFO interrupt lines to the GIC: idle/error (SSII0), receive-full
     * (SSIRXI0) and transmit-empty (SSITXI0). The firmware enables these but
     * services audio through DMA, so they are wired for completeness and left
     * quiescent (asserting transmit-empty continuously would storm the GIC).
     */
    qemu_irq irq_ssii;
    qemu_irq irq_rxi;
    qemu_irq irq_txi;

    /* Control/status register shadow. */
    uint32_t ssicr;
    uint32_t ssisr;
    uint32_t ssifcr;
    uint32_t ssitdmr;
    uint32_t ssifccr;
    uint32_t ssifcmr;
    uint32_t ssifcsr;

    /*
     * Audio output. The transmit DMA channel's ring buffer (in guest memory)
     * is mirrored to a QEMU output voice. The DMAC owns the ring geometry; the
     * SSI is told which channel carries it (tx_dma_channel) and reads the live
     * base/size from the DMAC on demand. play_cursor walks the ring as the
     * audio backend consumes samples.
     */
    AudioBackend *audio_be;
    struct RzA1lDmacState *dmac;
    int      tx_dma_channel;
    int      rx_dma_channel;
    SWVoiceOut *voice_out;
    SWVoiceIn  *voice_in;
    uint32_t play_cursor;   /* byte offset within the TX ring */
    uint32_t rec_cursor;    /* byte offset within the RX ring */
    bool     output_open;
    bool     input_open;
};

/*
 * Bind the SSI to the DMAC and the audio TX/RX channels (board setup). Enables
 * the audio output/input paths when an audio backend is configured.
 */
void rza1l_ssif_set_dma(RzA1lSsifState *s, struct RzA1lDmacState *dmac,
                        int tx_channel, int rx_channel);

#endif /* HW_SSI_RZA1L_SSIF_H */
