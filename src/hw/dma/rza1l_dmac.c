/*
 * Renesas RZ/A1 DMAC (Direct Memory Access Controller) — minimal model
 *
 * The RZ/A1 DMAC has 16 channels in two groups of eight. The Deluge firmware
 * uses it to move data between memory and the SPI (OLED/CV), SSI (audio) and
 * SD-host peripherals. Its drivers program a channel (N0SA source, N0DA dest,
 * N0TB byte count, CHCFG attributes), issue a software reset and enable
 * (CHCTRL SWRST then SETEN), and busy-wait on the channel status (CHSTAT) for
 * the transfer to finish (TACT/EN clear, END/TC set).
 *
 * This model performs each transfer synchronously the moment a channel is
 * enabled: it copies N0TB bytes from N0SA to N0DA through the system address
 * space — honouring the source/destination address-increment flags (SAD/DAD)
 * and unit sizes (SDS/DDS) — then reports the channel as completed. Hardware
 * DMA request lines, descriptor-chained (link-mode) transfers, repeat mode and
 * interrupts are not modelled; synchronous completion is sufficient for the
 * firmware's polled transfers and keeps boot moving.
 *
 * Register offsets verified via offsetof on the firmware st_dmac:
 *   Per channel (0x40 stride): N0SA 0x00 N0DA 0x04 N0TB 0x08 N1SA 0x0c
 *   N1DA 0x10 N1TB 0x14 CRSA 0x18 CRDA 0x1c CRTB 0x20 CHSTAT 0x24 CHCTRL 0x28
 *   CHCFG 0x2c CHITVL 0x30 CHEXT 0x34 NXLA 0x38 CRLA 0x3c.
 *   Channels 0-7 at 0x000+, channels 8-15 at 0x400+.
 *   Group common: DCTRL_0_7 0x300, DSTAT_EN 0x310, _ER 0x314, _END 0x318,
 *   _TC 0x31c, _SUS 0x320; group 1 mirrors these at 0x700+.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "hw/dma/rza1l_dmac.h"

/* Per-channel register byte offsets within a 0x40 channel block. */
#define DMAC_N0SA   0x00
#define DMAC_N0DA   0x04
#define DMAC_N0TB   0x08
#define DMAC_N1SA   0x0c
#define DMAC_N1DA   0x10
#define DMAC_N1TB   0x14
#define DMAC_CRSA   0x18
#define DMAC_CRDA   0x1c
#define DMAC_CRTB   0x20
#define DMAC_CHSTAT 0x24
#define DMAC_CHCTRL 0x28
#define DMAC_CHCFG  0x2c
#define DMAC_CHITVL 0x30
#define DMAC_CHEXT  0x34
#define DMAC_NXLA   0x38
#define DMAC_CRLA   0x3c
#define DMAC_CH_STRIDE 0x40

/* Common register offsets, per group. */
#define DMAC_DCTRL_G0    0x300
#define DMAC_DSTAT_EN_G0  0x310
#define DMAC_DSTAT_ER_G0  0x314
#define DMAC_DSTAT_END_G0 0x318
#define DMAC_DSTAT_TC_G0  0x31c
#define DMAC_DSTAT_SUS_G0 0x320
#define DMAC_DCTRL_G1    0x700
#define DMAC_DSTAT_EN_G1  0x710
#define DMAC_DSTAT_ER_G1  0x714
#define DMAC_DSTAT_END_G1 0x718
#define DMAC_DSTAT_TC_G1  0x71c
#define DMAC_DSTAT_SUS_G1 0x720

/* CHSTAT status bits. */
#define CHSTAT_EN   0x00000001
#define CHSTAT_TACT 0x00000004
#define CHSTAT_ER   0x00000010
#define CHSTAT_END  0x00000020
#define CHSTAT_TC   0x00000040

/* CHCTRL control bits (write-to-act, self-clearing). */
#define CHCTRL_SETEN  0x00000001
#define CHCTRL_CLREN  0x00000002
#define CHCTRL_SWRST  0x00000008
#define CHCTRL_CLREND 0x00000020
#define CHCTRL_CLRTC  0x00000040

/* CHCFG fields. */
#define CHCFG_SDS_SHIFT 12
#define CHCFG_SDS_MASK  0x0000f000
#define CHCFG_DDS_SHIFT 16
#define CHCFG_DDS_MASK  0x000f0000
#define CHCFG_SAD       0x00100000  /* set: source address fixed */
#define CHCFG_DAD       0x00200000  /* set: destination address fixed */

/*
 * Decode an MMIO offset into a channel index and the register offset within
 * that channel's block. Returns true on a hit.
 */
static bool rza1l_dmac_decode_ch(hwaddr offset, int *ch_out, hwaddr *reg_out)
{
    int ch;
    hwaddr rel;

    if (offset < 0x200) {
        ch = offset / DMAC_CH_STRIDE;       /* 0-7 */
        rel = offset % DMAC_CH_STRIDE;
    } else if (offset >= 0x400 && offset < 0x600) {
        ch = 8 + (offset - 0x400) / DMAC_CH_STRIDE; /* 8-15 */
        rel = (offset - 0x400) % DMAC_CH_STRIDE;
    } else {
        return false;
    }

    *ch_out = ch;
    *reg_out = rel;
    return true;
}

static uint32_t *rza1l_dmac_ch_reg_ptr(RzA1lDmacChannel *c, hwaddr reg)
{
    switch (reg) {
    case DMAC_N0SA:   return &c->n0sa;
    case DMAC_N0DA:   return &c->n0da;
    case DMAC_N0TB:   return &c->n0tb;
    case DMAC_N1SA:   return &c->n1sa;
    case DMAC_N1DA:   return &c->n1da;
    case DMAC_N1TB:   return &c->n1tb;
    case DMAC_CRSA:   return &c->crsa;
    case DMAC_CRDA:   return &c->crda;
    case DMAC_CRTB:   return &c->crtb;
    case DMAC_CHSTAT: return &c->chstat;
    case DMAC_CHCFG:  return &c->chcfg;
    case DMAC_CHITVL: return &c->chitvl;
    case DMAC_CHEXT:  return &c->chext;
    case DMAC_NXLA:   return &c->nxla;
    case DMAC_CRLA:   return &c->crla;
    default:          return NULL;
    }
}

/* Aggregate a CHSTAT bit across a group's eight channels into a DSTAT value. */
static uint32_t rza1l_dmac_dstat(RzA1lDmacState *s, int group, uint32_t bit)
{
    uint32_t val = 0;

    for (int i = 0; i < 8; i++) {
        if (s->ch[group * 8 + i].chstat & bit) {
            val |= 1u << i;
        }
    }
    return val;
}

/* Copy N0TB bytes from N0SA to N0DA, honouring address direction and unit. */
static void rza1l_dmac_do_transfer(RzA1lDmacState *s, int ch)
{
    RzA1lDmacChannel *c = &s->ch[ch];
    uint64_t sa = c->n0sa;
    uint64_t da = c->n0da;
    uint32_t remaining = c->n0tb;
    bool sad_fixed = c->chcfg & CHCFG_SAD;
    bool dad_fixed = c->chcfg & CHCFG_DAD;
    unsigned sds = 1u << ((c->chcfg & CHCFG_SDS_MASK) >> CHCFG_SDS_SHIFT);
    unsigned dds = 1u << ((c->chcfg & CHCFG_DDS_MASK) >> CHCFG_DDS_SHIFT);
    unsigned unit = MIN(sds, dds);
    uint8_t buf[32];

    if (unit == 0 || unit > sizeof(buf)) {
        unit = 1;
    }

    while (remaining) {
        unsigned n = MIN(unit, remaining);

        dma_memory_read(&address_space_memory, sa, buf, n,
                        MEMTXATTRS_UNSPECIFIED);
        dma_memory_write(&address_space_memory, da, buf, n,
                         MEMTXATTRS_UNSPECIFIED);
        if (!sad_fixed) {
            sa += n;
        }
        if (!dad_fixed) {
            da += n;
        }
        remaining -= n;
    }

    /* Reflect a completed transfer in the current (read-only) registers. */
    c->crsa = sa;
    c->crda = da;
    c->crtb = 0;
}

static void rza1l_dmac_chctrl_write(RzA1lDmacState *s, int ch, uint32_t val)
{
    RzA1lDmacChannel *c = &s->ch[ch];

    if (val & CHCTRL_SWRST) {
        c->chstat &= ~(CHSTAT_EN | CHSTAT_TACT | CHSTAT_ER |
                       CHSTAT_END | CHSTAT_TC);
    }
    if (val & CHCTRL_CLREN) {
        c->chstat &= ~CHSTAT_EN;
    }
    if (val & CHCTRL_CLREND) {
        c->chstat &= ~CHSTAT_END;
    }
    if (val & CHCTRL_CLRTC) {
        c->chstat &= ~CHSTAT_TC;
    }
    if (val & CHCTRL_SETEN) {
        /*
         * Run the transfer to completion immediately. The channel never
         * lingers in the active state, so the firmware's "wait for TACT/EN to
         * clear" and "wait for END/TC" loops both succeed on the next poll.
         */
        rza1l_dmac_do_transfer(s, ch);
        c->chstat &= ~(CHSTAT_EN | CHSTAT_TACT | CHSTAT_ER);
        c->chstat |= CHSTAT_END | CHSTAT_TC;

        /*
         * A channel registered as a peripheral receive ring is additionally
         * latched onto its descriptor's destination buffer so a peripheral can
         * push bytes into it (see rza1l_dmac_peripheral_rx_push). Only the
         * self-linking ring descriptors used for SCIF receive are handled; all
         * other channels (including audio ring buffers) keep the plain
         * synchronous behaviour above.
         *
         * Descriptor layout (8 little-endian words at NXLA):
         *   [0] header [1] src addr [2] dst addr [3] byte count
         *   [4] config [5] interval [6] extension [7] next link
         */
        if (c->rx_ring_peripheral && c->nxla != 0) {
            uint8_t d[32];

            dma_memory_read(&address_space_memory, c->nxla, d, sizeof(d),
                            MEMTXATTRS_UNSPECIFIED);
            if (ldl_le_p(d + 28) == c->nxla) {
                c->crda = ldl_le_p(d + 8);
                c->rx_ring_base = c->crda;
                c->rx_ring_size = ldl_le_p(d + 12);
                c->rx_ring_active = (c->rx_ring_size != 0);
            }
        }
    }
}

void rza1l_dmac_register_rx_ring(RzA1lDmacState *s, int ch)
{
    if (ch >= 0 && ch < RZA1L_DMAC_NUM_CH) {
        s->ch[ch].rx_ring_peripheral = true;
    }
}

bool rza1l_dmac_peripheral_rx_push(RzA1lDmacState *s, int ch, uint8_t byte)
{
    RzA1lDmacChannel *c;
    uint32_t off;

    if (ch < 0 || ch >= RZA1L_DMAC_NUM_CH) {
        return false;
    }
    c = &s->ch[ch];
    if (!c->rx_ring_active || c->rx_ring_size == 0) {
        return false;
    }

    dma_memory_write(&address_space_memory, c->crda, &byte, 1,
                     MEMTXATTRS_UNSPECIFIED);

    off = (c->crda - c->rx_ring_base + 1) % c->rx_ring_size;
    c->crda = c->rx_ring_base + off;
    return true;
}

static uint64_t rza1l_dmac_read(void *opaque, hwaddr offset, unsigned size)
{
    RzA1lDmacState *s = opaque;
    uint32_t reg = 0;
    hwaddr base = offset & ~3;
    unsigned shift = (offset & 3) * 8;
    int ch;
    hwaddr chreg;

    if (rza1l_dmac_decode_ch(base, &ch, &chreg)) {
        if (chreg == DMAC_CHCTRL) {
            reg = 0; /* control bits are write-only / self-clearing */
        } else {
            uint32_t *p = rza1l_dmac_ch_reg_ptr(&s->ch[ch], chreg);
            reg = p ? *p : 0;
        }
    } else {
        switch (base) {
        case DMAC_DCTRL_G0:    reg = s->dctrl[0]; break;
        case DMAC_DCTRL_G1:    reg = s->dctrl[1]; break;
        case DMAC_DSTAT_EN_G0:  reg = rza1l_dmac_dstat(s, 0, CHSTAT_EN); break;
        case DMAC_DSTAT_ER_G0:  reg = rza1l_dmac_dstat(s, 0, CHSTAT_ER); break;
        case DMAC_DSTAT_END_G0: reg = rza1l_dmac_dstat(s, 0, CHSTAT_END); break;
        case DMAC_DSTAT_TC_G0:  reg = rza1l_dmac_dstat(s, 0, CHSTAT_TC); break;
        case DMAC_DSTAT_SUS_G0: reg = 0; break;
        case DMAC_DSTAT_EN_G1:  reg = rza1l_dmac_dstat(s, 1, CHSTAT_EN); break;
        case DMAC_DSTAT_ER_G1:  reg = rza1l_dmac_dstat(s, 1, CHSTAT_ER); break;
        case DMAC_DSTAT_END_G1: reg = rza1l_dmac_dstat(s, 1, CHSTAT_END); break;
        case DMAC_DSTAT_TC_G1:  reg = rza1l_dmac_dstat(s, 1, CHSTAT_TC); break;
        case DMAC_DSTAT_SUS_G1: reg = 0; break;
        default:                reg = 0; break;
        }
    }

    return (reg >> shift) & (size == 4 ? 0xffffffffu
                                       : (1u << (size * 8)) - 1);
}

static void rza1l_dmac_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    RzA1lDmacState *s = opaque;
    hwaddr base = offset & ~3;
    unsigned shift = (offset & 3) * 8;
    uint32_t mask = (size == 4) ? 0xffffffffu : ((1u << (size * 8)) - 1) << shift;
    uint32_t val32 = (uint32_t)value << shift;
    int ch;
    hwaddr chreg;

    if (rza1l_dmac_decode_ch(base, &ch, &chreg)) {
        RzA1lDmacChannel *c = &s->ch[ch];

        if (chreg == DMAC_CHCTRL) {
            rza1l_dmac_chctrl_write(s, ch, val32);
            return;
        }
        if (chreg == DMAC_CHSTAT ||
            chreg == DMAC_CRSA || chreg == DMAC_CRDA || chreg == DMAC_CRTB) {
            return; /* status / current registers are read-only */
        }

        uint32_t *p = rza1l_dmac_ch_reg_ptr(c, chreg);
        if (p) {
            *p = (*p & ~mask) | (val32 & mask);
        }
        return;
    }

    switch (base) {
    case DMAC_DCTRL_G0:
        s->dctrl[0] = (s->dctrl[0] & ~mask) | (val32 & mask);
        break;
    case DMAC_DCTRL_G1:
        s->dctrl[1] = (s->dctrl[1] & ~mask) | (val32 & mask);
        break;
    default:
        /* DSTAT_* are read-only aggregates; other offsets are ignored. */
        break;
    }
}

static const MemoryRegionOps rza1l_dmac_ops = {
    .read = rza1l_dmac_read,
    .write = rza1l_dmac_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void rza1l_dmac_reset(DeviceState *dev)
{
    RzA1lDmacState *s = RZA1L_DMAC(dev);

    memset(s->ch, 0, sizeof(s->ch));
    memset(s->dctrl, 0, sizeof(s->dctrl));
}

static void rza1l_dmac_realize(DeviceState *dev, Error **errp)
{
    RzA1lDmacState *s = RZA1L_DMAC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rza1l_dmac_ops, s,
                          TYPE_RZA1L_DMAC, RZA1L_DMAC_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_rza1l_dmac_channel = {
    .name = "rza1l-dmac-channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(n0sa, RzA1lDmacChannel),
        VMSTATE_UINT32(n0da, RzA1lDmacChannel),
        VMSTATE_UINT32(n0tb, RzA1lDmacChannel),
        VMSTATE_UINT32(n1sa, RzA1lDmacChannel),
        VMSTATE_UINT32(n1da, RzA1lDmacChannel),
        VMSTATE_UINT32(n1tb, RzA1lDmacChannel),
        VMSTATE_UINT32(crsa, RzA1lDmacChannel),
        VMSTATE_UINT32(crda, RzA1lDmacChannel),
        VMSTATE_UINT32(crtb, RzA1lDmacChannel),
        VMSTATE_UINT32(chstat, RzA1lDmacChannel),
        VMSTATE_UINT32(chcfg, RzA1lDmacChannel),
        VMSTATE_UINT32(chitvl, RzA1lDmacChannel),
        VMSTATE_UINT32(chext, RzA1lDmacChannel),
        VMSTATE_UINT32(nxla, RzA1lDmacChannel),
        VMSTATE_UINT32(crla, RzA1lDmacChannel),
        VMSTATE_BOOL(rx_ring_peripheral, RzA1lDmacChannel),
        VMSTATE_UINT32(rx_ring_base, RzA1lDmacChannel),
        VMSTATE_UINT32(rx_ring_size, RzA1lDmacChannel),
        VMSTATE_BOOL(rx_ring_active, RzA1lDmacChannel),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_rza1l_dmac = {
    .name = TYPE_RZA1L_DMAC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(ch, RzA1lDmacState, RZA1L_DMAC_NUM_CH, 1,
                             vmstate_rza1l_dmac_channel, RzA1lDmacChannel),
        VMSTATE_UINT32_ARRAY(dctrl, RzA1lDmacState, 2),
        VMSTATE_END_OF_LIST()
    },
};

static void rza1l_dmac_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rza1l_dmac_realize;
    dc->vmsd = &vmstate_rza1l_dmac;
    device_class_set_legacy_reset(dc, rza1l_dmac_reset);
}

static const TypeInfo rza1l_dmac_info = {
    .name          = TYPE_RZA1L_DMAC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RzA1lDmacState),
    .class_init    = rza1l_dmac_class_init,
};

static void rza1l_dmac_register_types(void)
{
    type_register_static(&rza1l_dmac_info);
}

type_init(rza1l_dmac_register_types)
