/*
 * Renesas RZ/A1 SSIF (serial sound interface with FIFO) — I2S audio
 *
 * Channel 0 of the SSIF at 0xE820B000. The Deluge firmware configures the SSI
 * for 44.1 kHz stereo I2S and then services audio entirely through DMA (channel
 * 6 transmit, channel 7 receive), tracking playback position from the DMA
 * controller rather than the SSI FIFO. This model therefore:
 *
 *   - presents the control/status registers (SSICR/SSISR/SSIFCR/SSIFSR/...) so
 *     the firmware's SSI bring-up completes and any status poll reads "ready";
 *   - reports the transmit FIFO permanently empty (TDE) and the receive FIFO
 *     empty, since the FIFOs are drained/filled by DMA, not the CPU;
 *   - exposes the three SSI interrupt lines to the GIC for completeness.
 *
 * Register offsets within a channel (verified via offsetof on st_ssif):
 *   SSICR 0x00  SSISR 0x04  SSIFCR 0x0C  SSIFSR 0x10  SSIFTDR 0x14
 *   SSIFRDR 0x18  SSITDMR 0x1C  SSIFCCR 0x20  SSIFCMR 0x24  SSIFCSR 0x28
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/audio.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "hw/ssi/rza1l_ssif.h"
#include "hw/dma/rza1l_dmac.h"

/* Channel register offsets. */
#define SSIF_SSICR   0x00 /* control                       */
#define SSIF_SSISR   0x04 /* status                        */
#define SSIF_SSIFCR  0x0C /* FIFO control                  */
#define SSIF_SSIFSR  0x10 /* FIFO status                   */
#define SSIF_SSIFTDR 0x14 /* transmit FIFO data            */
#define SSIF_SSIFRDR 0x18 /* receive FIFO data             */
#define SSIF_SSITDMR 0x1C /* TDM mode                      */
#define SSIF_SSIFCCR 0x20 /* FIFO clock control            */
#define SSIF_SSIFCMR 0x24 /* FIFO clock measure            */
#define SSIF_SSIFCSR 0x28 /* FIFO clock status             */

/* SSICR control bits the firmware uses. */
#define SSICR_TEN  0x00000001 /* transmit enable           */
#define SSICR_REN  0x00000002 /* receive enable            */

/* SSIFCR FIFO-control bits the firmware uses. */
#define SSIFCR_RFRST 0x00000001 /* receive FIFO reset      */
#define SSIFCR_TFRST 0x00000002 /* transmit FIFO reset     */
#define SSIFCR_RIE   0x00000004 /* receive-full int enable */
#define SSIFCR_TIE   0x00000008 /* transmit-empty int enable */

/* SSIFSR FIFO-status bits. */
#define SSIFSR_RDF   0x00000001 /* receive FIFO has data   */
#define SSIFSR_TDE   0x00010000 /* transmit FIFO empty     */

/*
 * Firmware render write head. AudioEngine::i2sTXBufferPos (a uint32_t pointer
 * into the uncached SDRAM mirror) marks the slot just past the last frame the
 * firmware has finished writing into the SSI TX ring. Reading the ring bounded
 * by this head yields finished, tear-free output. The address is fixed by the
 * firmware link map (.bss._ZN11AudioEngine14i2sTXBufferPosE) and the mirror
 * offset by RZA1 cpu_specific.h (UNCACHED_MIRROR_OFFSET).
 */
#define RZA1L_SSIF_I2S_TX_POS_ADDR    0x20038FDC
#define RZA1L_SSIF_RING_MIRROR_OFFSET 0x40000000

static uint64_t rza1l_ssif_read(void *opaque, hwaddr offset, unsigned size)
{
    RzA1lSsifState *s = opaque;

    switch (offset) {
    case SSIF_SSICR:
        return s->ssicr;
    case SSIF_SSISR:
        /* No under/overrun or idle conditions are modelled. */
        return s->ssisr;
    case SSIF_SSIFCR:
        return s->ssifcr;
    case SSIF_SSIFSR:
        /*
         * The FIFOs are serviced by DMA, so from the CPU's point of view the
         * transmit FIFO is always empty (ready) and the receive FIFO holds no
         * directly-readable data.
         */
        return SSIFSR_TDE;
    case SSIF_SSIFRDR:
        /* Receive data is delivered via DMA, not CPU reads. */
        return 0;
    case SSIF_SSITDMR:
        return s->ssitdmr;
    case SSIF_SSIFCCR:
        return s->ssifccr;
    case SSIF_SSIFCMR:
        return s->ssifcmr;
    case SSIF_SSIFCSR:
        return s->ssifcsr;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void rza1l_ssif_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    RzA1lSsifState *s = opaque;

    switch (offset) {
    case SSIF_SSICR:
        s->ssicr = value;
        break;
    case SSIF_SSISR:
        /* Status bits are cleared by writing; nothing latched to clear. */
        break;
    case SSIF_SSIFCR:
        s->ssifcr = value;
        break;
    case SSIF_SSIFSR:
        /* Write-to-clear receive flags; TDE is recomputed on read. */
        break;
    case SSIF_SSIFTDR:
        /* Transmit data reaches the codec via DMA; CPU writes are absorbed. */
        break;
    case SSIF_SSITDMR:
        s->ssitdmr = value;
        break;
    case SSIF_SSIFCCR:
        s->ssifccr = value;
        break;
    case SSIF_SSIFCMR:
        s->ssifcmr = value;
        break;
    case SSIF_SSIFCSR:
        s->ssifcsr = value;
        break;
    case SSIF_SSIFRDR:
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented write @ 0x%" HWADDR_PRIx
                      " value 0x%" PRIx64 "\n",
                      __func__, offset, value);
        break;
    }
}

static const MemoryRegionOps rza1l_ssif_ops = {
    .read = rza1l_ssif_read,
    .write = rza1l_ssif_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

void rza1l_ssif_set_dma(RzA1lSsifState *s, struct RzA1lDmacState *dmac,
                        int tx_channel, int rx_channel)
{
    s->dmac = dmac;
    s->tx_dma_channel = tx_channel;
    s->rx_dma_channel = rx_channel;
    /* Sample the TX ring on the vCPU thread, driven by the firmware's CRSA
     * polling, so it is never starved by main-loop (display) stalls. */
    rza1l_dmac_set_tx_audio_pump(dmac, rza1l_ssif_tx_crsa_read, s);
}

/*
 * Drain up to max_bytes of staged audio into the output voice, returning the
 * number of bytes actually accepted. Draining is bounded so the caller can
 * pace it to the voice's consumption rate and keep the rest of the FIFO as a
 * jitter cushion. audio_be_write accepts only what currently fits in the mix
 * buffer, so we stop early if the voice is momentarily full.
 */
/*
 * Drift-correcting drain. The host audio device and the emulated guest each
 * run their own ~44.1 kHz clock; they are not synchronised, so over time one
 * outruns the other (measured ~0.5%). Left alone the staging FIFO would slowly
 * empty (host faster) or overflow (host slower), and a momentary production
 * stall would leave the cushion unable to recover. To lock the FIFO depth at a
 * target we resample on the fly by a tiny, depth-dependent factor: when the
 * FIFO is below target we advance through it slightly slower than we emit
 * (occasionally repeating a stereo frame, stretching time), and when it is
 * above target we advance slightly faster (consuming frames a touch quicker).
 * The correction is proportional and clamped to +/-2%, so at steady state it is
 * effectively zero and well under perceptible pitch change; its only job is to
 * absorb clock drift and rebuild the cushion after a stall. We resample with
 * linear interpolation between adjacent stereo frames (the FIFO holds S32
 * stereo), so the fractional, sub-percent rate change is smooth and inaudible
 * rather than the clicks that nearest-frame dup/drop would produce.
 *
 * out is filled with up to max_bytes of emitted audio; the function returns the
 * number of bytes written to the voice (the device decides how much it takes).
 */
static void rza1l_ssif_drain(RzA1lSsifState *s, uint32_t max_bytes)
{
    const uint32_t F = RZA1L_SSIF_BYTES_PER_FRAME;
    int32_t target = RZA1L_SSIF_PRIME_BYTES;
    int32_t err = (int32_t)s->fifo_len - target;
    uint32_t step;                   /* 16.16 FIFO frames per emitted frame */
    uint8_t out[4096];

    /* Proportional, clamped to +/-2% (65536 == 1.0x). */
    if (err > target) {
        err = target;
    } else if (err < -target) {
        err = -target;
    }
    step = (uint32_t)(65536 + (int64_t)err * (65536 / 50) / target);

    while (max_bytes >= F) {
        uint32_t cap = MIN(max_bytes, (uint32_t)sizeof(out));
        uint32_t built = 0;
        size_t written;

        while (built + F <= cap) {
            uint32_t nxt;
            int32_t l0, r0, l1, r1, lo, ro;
            int64_t t;

            if (s->fifo_len < 2 * F) {
                break;          /* need a frame pair to interpolate */
            }
            nxt = (s->fifo_head + F) % RZA1L_SSIF_FIFO_SIZE;
            memcpy(&l0, s->fifo + s->fifo_head, 4);
            memcpy(&r0, s->fifo + s->fifo_head + 4, 4);
            memcpy(&l1, s->fifo + nxt, 4);
            memcpy(&r1, s->fifo + nxt + 4, 4);

            /* Linear interpolation in S32 space (t in 16.16 fixed point). */
            t = s->drain_frac;
            lo = (int32_t)(l0 + ((((int64_t)l1 - l0) * t) >> 16));
            ro = (int32_t)(r0 + ((((int64_t)r1 - r0) * t) >> 16));
            memcpy(out + built, &lo, 4);
            memcpy(out + built + 4, &ro, 4);
            built += F;

            s->drain_frac += step;
            while (s->drain_frac >= 65536) {
                s->drain_frac -= 65536;
                if (s->fifo_len >= F) {
                    s->fifo_head = (s->fifo_head + F) % RZA1L_SSIF_FIFO_SIZE;
                    s->fifo_len -= F;
                }
            }
        }

        if (built == 0) {
            break;
        }
        written = audio_be_write(s->audio_be, s->voice_out, out, built);
        max_bytes -= (uint32_t)written;
        if (written < built) {
            break;              /* mix buffer full for now */
        }
    }
}

/*
 * Append PCM bytes to the staging FIFO. If the FIFO would overflow (the host
 * backend is draining slower than real time) the oldest staged bytes are
 * dropped to keep latency bounded; this is inaudible jitter, not the gross
 * distortion that reading a stale ring would cause.
 */
static void rza1l_ssif_fifo_push(RzA1lSsifState *s, const uint8_t *buf,
                                 uint32_t len)
{
    uint32_t tail;

    if (len >= RZA1L_SSIF_FIFO_SIZE) {
        /* Keep only the most recent FIFO-worth. */
        buf += len - (RZA1L_SSIF_FIFO_SIZE - RZA1L_SSIF_BYTES_PER_FRAME);
        len = RZA1L_SSIF_FIFO_SIZE - RZA1L_SSIF_BYTES_PER_FRAME;
    }
    if (s->fifo_len + len > RZA1L_SSIF_FIFO_SIZE) {
        uint32_t drop = s->fifo_len + len - RZA1L_SSIF_FIFO_SIZE;
        s->fifo_head = (s->fifo_head + drop) % RZA1L_SSIF_FIFO_SIZE;
        s->fifo_len -= drop;
    }

    tail = (s->fifo_head + s->fifo_len) % RZA1L_SSIF_FIFO_SIZE;
    while (len > 0) {
        uint32_t to_end = RZA1L_SSIF_FIFO_SIZE - tail;
        uint32_t chunk = MIN(len, to_end);
        memcpy(s->fifo + tail, buf, chunk);
        tail = (tail + chunk) % RZA1L_SSIF_FIFO_SIZE;
        buf += chunk;
        len -= chunk;
        s->fifo_len += chunk;
    }
}

/*
 * Copy newly-rendered frames from the guest TX ring into the staging FIFO.
 *
 * The read position is bounded by the firmware's render write head, not by the
 * DMA play head. In AudioEngine::doSomeOutputting the firmware copies freshly
 * mixed frames into the ring starting at i2sTXBufferPos and advancing up to —
 * but never past — the DMA source address (CRSA), then publishes the updated
 * i2sTXBufferPos in one store once the whole batch is written. Therefore every
 * slot strictly *behind* i2sTXBufferPos holds finished, tear-free output that
 * will not change until the head laps the ring a whole wrap later. We copy
 * exactly that span: from read_off forward up to i2sTXBufferPos, one slot at a
 * time, in order, delivering the firmware's true output stream frame-for-frame.
 *
 * Crucially this runs on the vCPU thread: the DMAC calls it from every CRSA
 * register read, which the firmware performs inside its audio loop (see
 * ssi.c getTxBufferCurrentPlace). That keeps the copy locked to production and
 * immune to main-loop stalls — the guest ring is only 128 frames (~2.9 ms), so
 * the periodic display redraw, which blocks the iothread timers for several
 * milliseconds, would otherwise let the write head lap an unread region and
 * tear the audio. A virtual-clock fallback timer also pumps it so the FIFO
 * keeps draining while the firmware is briefly not polling CRSA.
 */
static void rza1l_ssif_pump(RzA1lSsifState *s)
{
    uint32_t base = 0, size = 0;
    uint32_t wpos_ptr = 0, wpos, avail;

    if (!s->dmac ||
        !rza1l_dmac_get_tx_audio_ring(s->dmac, s->tx_dma_channel,
                                      &base, &size) ||
        size < RZA1L_SSIF_BYTES_PER_FRAME) {
        /* Ring not armed yet: re-seed read_off when it appears. */
        s->play_anchored = false;
        return;
    }

    /*
     * Fetch AudioEngine::i2sTXBufferPos, the firmware's render write head. It
     * is a pointer into the uncached SDRAM mirror; strip the mirror offset and
     * ring base to land on a ring byte offset, then frame-align it (the
     * firmware only ever advances it whole frames, but guard against a torn
     * read shearing a stereo frame).
     */
    if (dma_memory_read(&address_space_memory, RZA1L_SSIF_I2S_TX_POS_ADDR,
                        &wpos_ptr, sizeof(wpos_ptr),
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return;
    }
    wpos_ptr = le32_to_cpu(wpos_ptr);
    wpos = (wpos_ptr - RZA1L_SSIF_RING_MIRROR_OFFSET - base) % size;
    wpos &= ~(RZA1L_SSIF_BYTES_PER_FRAME - 1);

    if (!s->play_anchored) {
        /* Seed the read head at the live write head: no backlog. */
        s->play_anchored = true;
        s->read_off = wpos;
    }

    /* Frames to copy now: distance from read_off forward to the write head. */
    avail = (wpos + size - s->read_off) % size;

    /*
     * If we have somehow fallen more than a ring behind (a stall longer than
     * the ring's wrap period), the head has lapped us and the data in between
     * is already overwritten; resync rather than read stale frames.
     */
    if (avail >= size - RZA1L_SSIF_BYTES_PER_FRAME) {
        s->read_off = wpos;
        avail = 0;
    }

    while (avail > 0) {
        uint8_t buf[1024];
        uint32_t to_end = size - s->read_off;
        uint32_t chunk = MIN(avail, MIN(to_end, (uint32_t)sizeof(buf)));

        if (dma_memory_read(&address_space_memory, base + s->read_off, buf,
                            chunk, MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            memset(buf, 0, chunk);
        }
        rza1l_ssif_fifo_push(s, buf, chunk);
        s->read_off = (s->read_off + chunk) % size;
        avail -= chunk;
    }
}

/*
 * DMAC hook: the transmit audio channel invokes this on every CRSA register
 * read so the ring is sampled on the vCPU thread, in step with the firmware's
 * own playback polling (see rza1l_ssif_pump).
 */
void rza1l_ssif_tx_crsa_read(void *opaque)
{
    rza1l_ssif_pump((RzA1lSsifState *)opaque);
}

/*
 * Fallback sampling/drain timer on the virtual clock. The vCPU-side pump above
 * does the bulk of the work; this keeps the FIFO draining (and catches up the
 * ring) during stretches where the firmware is not polling CRSA.
 */
static void rza1l_ssif_play_tick(void *opaque)
{
    RzA1lSsifState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    rza1l_ssif_pump(s);

    timer_mod_ns(s->play_timer, now + RZA1L_SSIF_PLAY_TICK_NS);
}

/*
 * Audio output pull callback. The backend invokes this when its mix buffer can
 * accept more samples, at the real-time consumption rate. We feed it from the
 * staging FIFO, but only after a latency cushion has primed, and only up to
 * the bytes it just asked for — the remainder of the FIFO stays as a reserve
 * that absorbs the firmware's bursty, emulated-time-paced production. If the
 * reserve is momentarily exhausted we simply deliver what we have and let the
 * voice play silence for the shortfall (a sub-callback gap), then resume as
 * soon as production catches up; the large cushion makes this rare and brief,
 * and it is far less disruptive than withholding output to rebuild the buffer.
 */
static void rza1l_ssif_out_cb(void *opaque, int free_bytes)
{
    RzA1lSsifState *s = opaque;

    if (free_bytes <= 0) {
        return;
    }

    if (!s->out_primed) {
        if (s->fifo_len < RZA1L_SSIF_PRIME_BYTES) {
            return; /* still building the cushion; voice plays silence */
        }
        s->out_primed = true;
    }

    rza1l_ssif_drain(s, (uint32_t)free_bytes);
}

/*
 * Audio input callback. The backend invokes this when captured samples are
 * available. The firmware receives audio through the SSI receive DMA channel,
 * so we write captured frames into the RX ring in guest memory at the channel's
 * live write position (CRDA). With no capture source the read yields silence,
 * and the firmware's input path simply sees zeroed samples.
 */
static void rza1l_ssif_in_cb(void *opaque, int avail)
{
    RzA1lSsifState *s = opaque;
    uint8_t buf[1024];
    uint32_t base, size, crda, off;

    if (avail <= 0) {
        return;
    }

    if (!s->dmac ||
        !rza1l_dmac_get_rx_audio_ring(s->dmac, s->rx_dma_channel,
                                      &base, &size) ||
        !rza1l_dmac_get_rx_audio_crda(s->dmac, s->rx_dma_channel, &crda)) {
        /* Ring not armed yet: drain and discard the captured samples. */
        while (avail > 0) {
            int chunk = MIN(avail, (int)sizeof(buf));
            size_t got = audio_be_read(s->audio_be, s->voice_in, buf, chunk);
            if (got == 0) {
                break;
            }
            avail -= got;
        }
        return;
    }

    off = (crda - base) % size;
    while (avail > 0) {
        int chunk = MIN(avail, (int)sizeof(buf));
        uint32_t to_end = size - off;
        size_t got;

        if ((uint32_t)chunk > to_end) {
            chunk = to_end;
        }
        got = audio_be_read(s->audio_be, s->voice_in, buf, chunk);
        if (got == 0) {
            break;
        }
        dma_memory_write(&address_space_memory, base + off, buf, got,
                         MEMTXATTRS_UNSPECIFIED);
        off = (off + got) % size;
        s->rec_cursor = off;
        avail -= got;
    }
}


static void rza1l_ssif_reset(DeviceState *dev)
{
    RzA1lSsifState *s = RZA1L_SSIF(dev);

    s->ssicr = 0;
    s->ssisr = 0;
    s->ssifcr = SSIFCR_TFRST | SSIFCR_RFRST;
    s->ssitdmr = 0;
    s->ssifccr = 0;
    s->ssifcmr = 0;
    s->ssifcsr = 0;
    s->play_anchored = false;
    s->out_primed = false;
    s->read_off = 0;
    s->drain_frac = 0;
    s->fifo_head = 0;
    s->fifo_len = 0;
    s->rec_cursor = 0;
}

static void rza1l_ssif_realize(DeviceState *dev, Error **errp)
{
    RzA1lSsifState *s = RZA1L_SSIF(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rza1l_ssif_ops, s,
                          TYPE_RZA1L_SSIF, RZA1L_SSIF_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq_ssii);
    sysbus_init_irq(sbd, &s->irq_rxi);
    sysbus_init_irq(sbd, &s->irq_txi);

    /*
     * The Deluge is fundamentally an audio device, so we always open a host
     * voice. With no -audiodev on the command line, audio_be_check() resolves
     * the OS default backend (coreaudio/pa/dsound/...); pass -audiodev only to
     * select a non-default backend. The voice runs continuously and pulls
     * samples from the transmit DMA ring (or silence until armed).
     */
    if (!audio_be_check(&s->audio_be, errp)) {
        return;
    }

    {
        struct audsettings as = {
            .freq = RZA1L_SSIF_SAMPLE_RATE,
            .nchannels = RZA1L_SSIF_NUM_CHANNELS,
            .fmt = AUDIO_FORMAT_S32,
            .big_endian = false,
        };

        s->voice_out = audio_be_open_out(s->audio_be, s->voice_out, "ssif.out",
                                         s, rza1l_ssif_out_cb, &as);
        if (!s->voice_out) {
            error_setg(errp, "rza1l-ssif: failed to open audio output");
            return;
        }
        audio_be_set_active_out(s->audio_be, s->voice_out, true);
        s->output_open = true;

        /*
         * Start the fallback ring sampler/drain timer. The bulk of the ring
         * copying is driven from the vCPU thread on each CRSA read (see
         * rza1l_ssif_pump); this timer keeps the FIFO draining and catches the
         * ring up whenever the firmware is briefly not polling CRSA.
         */
        s->play_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rza1l_ssif_play_tick,
                                     s);
        timer_mod_ns(s->play_timer,
                     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                         RZA1L_SSIF_PLAY_TICK_NS);

        s->voice_in = audio_be_open_in(s->audio_be, s->voice_in, "ssif.in",
                                       s, rza1l_ssif_in_cb, &as);
        if (s->voice_in) {
            audio_be_set_active_in(s->audio_be, s->voice_in, true);
            s->input_open = true;
        }
    }
}

static const VMStateDescription vmstate_rza1l_ssif = {
    .name = TYPE_RZA1L_SSIF,
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ssicr, RzA1lSsifState),
        VMSTATE_UINT32(ssisr, RzA1lSsifState),
        VMSTATE_UINT32(ssifcr, RzA1lSsifState),
        VMSTATE_UINT32(ssitdmr, RzA1lSsifState),
        VMSTATE_UINT32(ssifccr, RzA1lSsifState),
        VMSTATE_UINT32(ssifcmr, RzA1lSsifState),
        VMSTATE_UINT32(ssifcsr, RzA1lSsifState),
        VMSTATE_UINT32(rec_cursor, RzA1lSsifState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property rza1l_ssif_properties[] = {
    DEFINE_AUDIO_PROPERTIES(RzA1lSsifState, audio_be),
};

static void rza1l_ssif_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rza1l_ssif_realize;
    dc->vmsd = &vmstate_rza1l_ssif;
    device_class_set_props(dc, rza1l_ssif_properties);
    device_class_set_legacy_reset(dc, rza1l_ssif_reset);
}

static const TypeInfo rza1l_ssif_info = {
    .name          = TYPE_RZA1L_SSIF,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RzA1lSsifState),
    .class_init    = rza1l_ssif_class_init,
};

static void rza1l_ssif_register_types(void)
{
    type_register_static(&rza1l_ssif_info);
}

type_init(rza1l_ssif_register_types)
