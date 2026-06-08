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
#define RZA1L_SSIF_BYTES_PER_FRAME (RZA1L_SSIF_NUM_CHANNELS * 4)

/*
 * The firmware's transmit ring is tiny (128 stereo frames, ~2.9 ms) and is
 * overwritten in place by the audio engine on every loop, while the host audio
 * backend pulls in much coarser ~10 ms periods. To avoid reading the ring after
 * the firmware has overwritten it (which manifests as harsh distortion), the
 * SSI copies finished frames into a host-side staging FIFO that the output
 * voice then drains. The copy (rza1l_ssif_pump) is driven primarily from the
 * vCPU thread, on every transmit CRSA register read the firmware performs in
 * its audio loop, so sampling tracks production and is never starved by
 * main-loop stalls such as the periodic display redraw. A virtual-time fallback
 * timer also pumps it so the FIFO keeps draining when CRSA is briefly not
 * polled.
 */
#define RZA1L_SSIF_PLAY_TICK_NS  1000000ull          /* 1 ms fallback tick   */
#define RZA1L_SSIF_FIFO_SIZE     131072u             /* ~372 ms, power of two */

/*
 * Output latency cushion. The host voice consumes at a fixed real-time rate
 * while the firmware renders in bursts (paced by emulated CPU time), so the
 * staging FIFO must hold a buffer of finished audio to absorb that jitter. We
 * withhold output until the FIFO has primed to this depth, then drain at the
 * voice's pull rate, spending and refilling the cushion as bursts ebb and
 * flow. Measured production stalls (emulation briefly pausing while the host
 * keeps consuming, e.g. during the periodic display redraw) drain ~85 ms, so
 * the cushion is sized well above that (~125 ms) to ride them without the FIFO
 * ever reaching empty. A brief shortfall is handled softly (silence for that
 * callback only) rather than by a disruptive full rebuild.
 */
#define RZA1L_SSIF_PRIME_BYTES   44100u             /* ~125 ms @ 44.1k stereo */

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
     * base/size from the DMAC on demand.
     *
     * Because the guest ring (~2.9 ms) is far smaller than the backend's pull
     * period (~10 ms), rza1l_ssif_pump copies finished frames from the ring
     * into a staging FIFO and the output voice drains that FIFO. The read
     * position is bounded by the firmware's render write head
     * (AudioEngine::i2sTXBufferPos): read_off advances up to that head, copying
     * only frames the firmware has finished writing. The pump runs from the
     * vCPU thread on every CRSA register read (plus a fallback timer), so it
     * keeps pace with production even when the main loop stalls. play_anchored
     * is false until the ring is armed and read_off has been seeded.
     */
    AudioBackend *audio_be;
    struct RzA1lDmacState *dmac;
    int      tx_dma_channel;
    int      rx_dma_channel;
    SWVoiceOut *voice_out;
    SWVoiceIn  *voice_in;

    QEMUTimer *play_timer;       /* fine-grained ring sampler             */
    bool      play_anchored;     /* read_off seeded for the live ring     */
    bool      out_primed;        /* output cushion built; draining at rate */
    uint32_t  read_off;          /* byte offset within the TX ring        */
    uint32_t  drain_frac;        /* 16.16 resampler phase for drift trim   */

    /* Host-side staging FIFO between the ring sampler and the output voice. */
    uint8_t   fifo[RZA1L_SSIF_FIFO_SIZE];
    uint32_t  fifo_head;         /* read index (mod RZA1L_SSIF_FIFO_SIZE)  */
    uint32_t  fifo_len;          /* bytes currently staged                */

    uint32_t rec_cursor;    /* byte offset within the RX ring */
    bool     output_open;
    bool     input_open;

    /*
     * Audio capture (line-in) is opt-in. Most host backends used for playback
     * cannot capture (e.g. CoreAudio has no input voice), and attempting to
     * open one emits a spurious "Can not open `ssif.in'" error on the default
     * path. The firmware only sees silence on the receive ring when capture is
     * off, which is the normal case, so we leave it disabled unless a
     * capture-capable backend is explicitly requested via this property.
     */
    bool     capture;
};

/*
 * Bind the SSI to the DMAC and the audio TX/RX channels (board setup). Enables
 * the audio output/input paths when an audio backend is configured.
 */
void rza1l_ssif_set_dma(RzA1lSsifState *s, struct RzA1lDmacState *dmac,
                        int tx_channel, int rx_channel);

/*
 * vCPU-side ring pump invoked by the DMAC on each transmit CRSA register read.
 * Copies finished frames from the guest TX ring into the staging FIFO in step
 * with the firmware's playback polling, so sampling tracks production and is
 * never starved by main-loop stalls. opaque is the RzA1lSsifState.
 */
void rza1l_ssif_tx_crsa_read(void *opaque);

#endif /* HW_SSI_RZA1L_SSIF_H */
