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
}

/*
 * Audio output callback. QEMU's audio backend invokes this when its output
 * buffer can accept up to free_bytes more bytes. The firmware streams the I2S
 * transmit buffer through DMA (the SSI transmit channel), so we mirror that
 * ring from guest memory to the host: read samples at play_cursor, wrapping at
 * the ring size, and feed them to the voice. Until the firmware arms the
 * transmit DMA the ring is unknown, so we output silence to keep the backend's
 * clock running.
 */
static void rza1l_ssif_out_cb(void *opaque, int free_bytes)
{
    RzA1lSsifState *s = opaque;
    uint8_t buf[1024];
    uint32_t base = 0, size = 0;
    bool have_ring;

    if (free_bytes <= 0) {
        return;
    }

    have_ring = s->dmac &&
        rza1l_dmac_get_tx_audio_ring(s->dmac, s->tx_dma_channel, &base, &size);

    while (free_bytes > 0) {
        int chunk = MIN(free_bytes, (int)sizeof(buf));
        size_t written;

        if (have_ring) {
            uint32_t avail = size - s->play_cursor;
            if ((uint32_t)chunk > avail) {
                chunk = avail;
            }
            if (dma_memory_read(&address_space_memory, base + s->play_cursor,
                                buf, chunk, MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                memset(buf, 0, chunk);
            }
        } else {
            memset(buf, 0, chunk);
        }

        written = audio_be_write(s->audio_be, s->voice_out, buf, chunk);
        if (have_ring) {
            s->play_cursor += written;
            if (s->play_cursor >= size) {
                s->play_cursor -= size;
            }
        }
        free_bytes -= written;
        if (written < (size_t)chunk) {
            break; /* backend buffer full for now */
        }
    }
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
    s->play_cursor = 0;
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
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ssicr, RzA1lSsifState),
        VMSTATE_UINT32(ssisr, RzA1lSsifState),
        VMSTATE_UINT32(ssifcr, RzA1lSsifState),
        VMSTATE_UINT32(ssitdmr, RzA1lSsifState),
        VMSTATE_UINT32(ssifccr, RzA1lSsifState),
        VMSTATE_UINT32(ssifcmr, RzA1lSsifState),
        VMSTATE_UINT32(ssifcsr, RzA1lSsifState),
        VMSTATE_UINT32(play_cursor, RzA1lSsifState),
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
