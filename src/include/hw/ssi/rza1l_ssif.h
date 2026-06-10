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
 * Minimum virtual-time spacing between actual ring copies. The firmware polls
 * the DMA play head (CRSA) from a tight loop in its audio routine, and every
 * such poll would otherwise drive a full ring scan plus a render-head DMA read
 * on the vCPU thread — millions of times a second — stealing cycles from the
 * very loop that renders the audio. The copy only needs to run often enough
 * that the ~2.9 ms guest ring never laps unread, so we coalesce bursts of polls
 * into one copy per this interval. Far finer than both the ring wrap and the
 * host pull period, so output stays smooth while the per-poll overhead is gone.
 */
#define RZA1L_SSIF_PUMP_MIN_NS   250000ll            /* 250 us copy throttle */

/*
 * Output latency cushion. The host voice consumes at a fixed real-time rate
 * while the firmware renders in bursts (paced by emulated CPU time), so the
 * staging FIFO must hold a buffer of finished audio to absorb that jitter. We
 * withhold output until the FIFO has primed to this depth, then drain at the
 * voice's pull rate, spending and refilling the cushion as bursts ebb and
 * flow. Historically the periodic display redraw stalled emulation for ~85 ms
 * while the host kept consuming, but the skin view now skips unchanged frames
 * so that stall is gone in practice; a small cushion (~15 ms default) is enough
 * to absorb ordinary burst jitter. A brief shortfall is handled softly (silence
 * for that callback only) rather than by a disruptive full rebuild.
 *
 * The depth is tunable at runtime via the "prime-ms" property (run.sh's
 * --audio-buffer): raising it rides larger stalls at the cost of more latency,
 * lowering it cuts perceived latency (helpful when playing the emulated Deluge
 * live from external MIDI) at the risk of soft dropouts. Clamped to FIFO size.
 */
#define RZA1L_SSIF_DEFAULT_PRIME_MS  15u            /* ~15 ms @ 44.1k stereo  */

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
     * into a staging FIFO and the output voice drains that FIFO. The copy is
     * bounded by the firmware's render head, not the wall-clock DMA play head.
     *
     * The firmware's audio loop reads the DMA play head P (CRSA) and renders
     * the backlog [W, P), advancing its render head W (AudioEngine::
     * i2sTXBufferPos) forward toward P; so slots [W, P) hold the *previous*
     * loop's audio until the firmware catches up. CRSA is synthesised from
     * elapsed virtual time, so under TCG load P outruns W and the backlog grows
     * toward a full ring — copying up to P then reads up to ~2.9 ms of
     * last-loop audio, the harsh ring-lap distortion. Copying only up to W
     * never reads an unrendered slot; when the firmware falls behind, the
     * staging FIFO simply underruns into a brief, cushion-absorbed silence.
     *
     * W lives in guest memory at a firmware build-specific address, supplied
     * out-of-band by the "tx-render-head" property (render_head_addr). When it
     * is 0 (unset, or firmware whose symbol address is unknown) the copy falls
     * back to the bare play head P: firmware-independent and never silent, but
     * subject to the ring-lap distortion under heavy load. The pump runs from
     * the vCPU thread on every CRSA register read (plus a fallback timer), so
     * it keeps pace with production even when the main loop stalls.
     * play_anchored is false until the ring is armed and read_off is seeded.
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
    uint32_t  render_head_addr;  /* guest addr of i2sTXBufferPos; 0=play-head */
    uint32_t  drain_frac;        /* 16.16 resampler phase for drift trim   */
    int64_t   last_pump_ns;      /* virtual time of last ring copy (throttle) */

    /* Output latency cushion: "prime-ms" property -> derived byte depth. */
    uint32_t  prime_ms;          /* configured cushion in milliseconds    */
    uint32_t  prime_bytes;       /* cushion depth in bytes (clamped)      */

    /* Host-side staging FIFO between the ring sampler and the output voice. */
    uint8_t   fifo[RZA1L_SSIF_FIFO_SIZE];
    uint32_t  fifo_head;         /* read index (mod RZA1L_SSIF_FIFO_SIZE)  */
    uint32_t  fifo_len;          /* bytes currently staged                */

    /*
     * Optional production-rate probe (DELUGEMU_SSIF_STATS). The real-world
     * success signal for the firmware-performance effort: how many freshly
     * rendered guest audio bytes per second reach the staging FIFO, against the
     * 352,800 B/s real-time target, plus how often the output voice underran a
     * primed FIFO. Counters accumulate over a one-second virtual-time window
     * and are reported (and cleared) from the fallback play timer. Inert unless
     * the environment variable is set, so default runs are unaffected.
     */
    uint64_t  stats_prod_bytes;  /* bytes pushed to FIFO this window       */
    uint64_t  stats_underruns;   /* primed-FIFO shortfalls this window     */
    int64_t   stats_window_ns;   /* virtual time the current window opened */

    /*
     * Optional raw render capture (DELUGEMU_SSIF_DUMP=<path>). When set, every
     * freshly-rendered S32LE stereo frame copied from the guest TX ring is
     * written to this file *before* the drift resampler, so the capture is the
     * firmware's exact rendered output. Under --icount the guest renders
     * deterministically, so two runs with identical input produce byte-equal
     * files: the bit-exact gate for validating a firmware DSP change. NULL when
     * the variable is unset.
     */
    FILE     *dump_fp;

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

    /*
     * Host-side monitor output level (an emulator-only attenuator, not a guest
     * register). The Deluge's master OUTPUT LEVEL is an analog pot the firmware
     * never reads, so in emulation it does nothing; we repurpose it as a
     * software volume so a hot firmware (e.g. third-party builds that render
     * ~10 dB louder) cannot overdrive the host audio interface. out_level is a
     * step index 0..RZA1L_SSIF_OUT_LEVEL_MAX (0 = mute, MAX = unity/0 dB);
     * out_gain_q16 is the matching 16.16 linear gain applied in the drain.
     */
    uint32_t out_level;
    uint32_t out_gain_q16;
};

/* Output-level step ladder: index MAX = unity (0 dB), 0 = mute, -2 dB/step. */
#define RZA1L_SSIF_OUT_LEVEL_MAX 16

/*
 * Bind the SSI to the DMAC and the audio TX/RX channels (board setup). Enables
 * the audio output/input paths when an audio backend is configured.
 */
void rza1l_ssif_set_dma(RzA1lSsifState *s, struct RzA1lDmacState *dmac,
                        int tx_channel, int rx_channel);

/*
 * Step the host monitor output level (master OUTPUT LEVEL knob). dir > 0 raises
 * it (toward unity), dir < 0 lowers it (toward mute), clamped to the ladder.
 * Returns the new step index. Safe to call from the input/UI thread.
 */
int rza1l_ssif_output_level_step(DeviceState *dev, int dir);

/* Current output-level step index (0..RZA1L_SSIF_OUT_LEVEL_MAX). */
int rza1l_ssif_output_level(DeviceState *dev);

/*
 * vCPU-side ring pump invoked by the DMAC on each transmit CRSA register read.
 * Copies finished frames from the guest TX ring into the staging FIFO in step
 * with the firmware's playback polling, so sampling tracks production and is
 * never starved by main-loop stalls. opaque is the RzA1lSsifState.
 */
void rza1l_ssif_tx_crsa_read(void *opaque);

#endif /* HW_SSI_RZA1L_SSIF_H */
