/*
 * Renesas RZ/A1 SPIBSC (SPI multi-I/O bus controller) — minimal model
 *
 * The SPIBSC drives the serial NOR/QSPI flash on the Deluge. The firmware uses
 * it in two ways:
 *   - "SPI mode" manual commands: write the command/address/data registers
 *     (SMCMR/SMADR/SMENR/SMWDR), kick a transfer via SMCR.SPIE, busy-wait on
 *     CMNSR.TEND, then read SMRDR for the response. Used for flash status reads
 *     (read_status -> WIP bit), erase/program, and ID reads.
 *   - "external address space read mode": the flash is memory-mapped (handled
 *     elsewhere by the 0x18000000 SPI-ROM window); the firmware proper is loaded
 *     directly via -kernel, so that path is not exercised during boot here.
 *
 * This model makes every manual transfer complete instantly: CMNSR.TEND always
 * reads set, so the firmware's wait loops succeed. SMRDR reads back zero, which
 * the flash drivers interpret as "status WIP clear / not busy", letting the
 * boot-time Userdef_SFLASH_Busy_Wait() loops exit. CMNSR.SSLF tracks the SSL
 * keep/negate state so spibsc_stop()/spibsc_transfer() chip-select handshakes
 * behave. No real flash device is attached; modelling actual flash content is
 * future work (storage milestone).
 *
 * Register offsets verified via offsetof on the firmware st_spibsc:
 *   CMNCR 0x00 SSLDR 0x04 SPBCR 0x08 DRCR 0x0c DRCMR 0x10 DREAR 0x14
 *   DROPR 0x18 DRENR 0x1c SMCR 0x20 SMCMR 0x24 SMADR 0x28 SMOPR 0x2c
 *   SMENR 0x30 SMRDR0 0x38 SMRDR1 0x3c SMWDR0 0x40 SMWDR1 0x44 CMNSR 0x48
 *   CKDLY 0x50 DRDMCR 0x58 DRDRENR 0x5c SMDMCR 0x60 SMDRENR 0x64 SPODLY 0x68.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/ssi/rza1l_spibsc.h"

/* Register byte offsets. */
#define SPIBSC_DRCR   0x0c
#define SPIBSC_SMCR   0x20
#define SPIBSC_SMRDR0 0x38
#define SPIBSC_SMRDR1 0x3c
#define SPIBSC_CMNSR  0x48

/* SMCR bits. */
#define SMCR_SPIE  0x00000001  /* start transfer */
#define SMCR_SSLKP 0x00000100  /* keep SSL asserted after transfer */

/* DRCR bits. */
#define DRCR_SSLN  0x01000000  /* negate SSL */

/* CMNSR bits. */
#define CMNSR_TEND 0x00000001  /* transfer end (always set in this model) */
#define CMNSR_SSLF 0x00000002  /* SSL line state */

static uint64_t rza1l_spibsc_read(void *opaque, hwaddr offset, unsigned size)
{
    RzA1lSpibscState *s = opaque;
    uint32_t reg;

    switch (offset) {
    case SPIBSC_CMNSR:
        reg = CMNSR_TEND | (s->ssl_asserted ? CMNSR_SSLF : 0);
        break;
    case SPIBSC_SMRDR0:
    case SPIBSC_SMRDR1:
        /* No flash attached: read data is zero (status WIP clear). */
        reg = 0;
        break;
    default:
        reg = (offset < RZA1L_SPIBSC_MMIO_SIZE) ? s->regs[offset / 4] : 0;
        break;
    }

    if (size == 4) {
        return reg;
    }
    return (reg >> ((offset & 3) * 8)) & ((1u << (size * 8)) - 1);
}

static void rza1l_spibsc_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    RzA1lSpibscState *s = opaque;
    hwaddr base = offset & ~3;
    unsigned shift = (offset & 3) * 8;
    uint32_t mask = (size == 4) ? 0xffffffffu
                                : ((1u << (size * 8)) - 1) << shift;
    uint32_t val32 = (uint32_t)value << shift;

    if (base >= RZA1L_SPIBSC_MMIO_SIZE) {
        return;
    }

    /* CMNSR is read-only status. */
    if (base == SPIBSC_CMNSR) {
        return;
    }

    s->regs[base / 4] = (s->regs[base / 4] & ~mask) | (val32 & mask);

    switch (base) {
    case SPIBSC_SMCR:
        if (s->regs[base / 4] & SMCR_SPIE) {
            /*
             * Transfer kicked. It completes immediately (TEND stays set). The
             * SSL line is held asserted only if SSLKP was requested.
             */
            s->ssl_asserted = s->regs[base / 4] & SMCR_SSLKP;
        }
        break;
    case SPIBSC_DRCR:
        if (s->regs[base / 4] & DRCR_SSLN) {
            s->ssl_asserted = false;
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps rza1l_spibsc_ops = {
    .read = rza1l_spibsc_read,
    .write = rza1l_spibsc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void rza1l_spibsc_reset(DeviceState *dev)
{
    RzA1lSpibscState *s = RZA1L_SPIBSC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->ssl_asserted = false;
}

static void rza1l_spibsc_realize(DeviceState *dev, Error **errp)
{
    RzA1lSpibscState *s = RZA1L_SPIBSC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rza1l_spibsc_ops, s,
                          TYPE_RZA1L_SPIBSC, RZA1L_SPIBSC_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_rza1l_spibsc = {
    .name = TYPE_RZA1L_SPIBSC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RzA1lSpibscState,
                             RZA1L_SPIBSC_MMIO_SIZE / 4),
        VMSTATE_BOOL(ssl_asserted, RzA1lSpibscState),
        VMSTATE_END_OF_LIST()
    },
};

static void rza1l_spibsc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rza1l_spibsc_realize;
    dc->vmsd = &vmstate_rza1l_spibsc;
    device_class_set_legacy_reset(dc, rza1l_spibsc_reset);
}

static const TypeInfo rza1l_spibsc_info = {
    .name          = TYPE_RZA1L_SPIBSC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RzA1lSpibscState),
    .class_init    = rza1l_spibsc_class_init,
};

static void rza1l_spibsc_register_types(void)
{
    type_register_static(&rza1l_spibsc_info);
}

type_init(rza1l_spibsc_register_types)
