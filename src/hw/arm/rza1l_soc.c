/*
 * Renesas RZ/A1L SoC model (as used by the Synthstrom Deluge)
 *
 * Builds the Cortex-A9 CPU, on-chip SRAM and external SDRAM regions, and a
 * logging catch-all over the peripheral space. Individual peripherals (SCIF,
 * SSI, INTC, CPG, BSC, SD, ...) are added incrementally.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/unimp.h"
#include "hw/core/sysbus.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "hw/arm/rza1l_soc.h"
#include "hw/ssi/rza1l_rspi.h"
#include "hw/ssi/rza1l_spibsc.h"
#include "hw/timer/rza1l_mtu2.h"
#include "hw/timer/rza1l_ostm.h"
#include "hw/dma/rza1l_dmac.h"
#include "hw/gpio/rza1l_gpio.h"
#include "hw/intc/arm_gic.h"
#include "hw/char/rza1l_scif.h"
#include "hw/misc/deluge_pic.h"
#include "hw/sd/rza1l_sdhi.h"
#include "chardev/char.h"

static void rza1l_soc_init(Object *obj)
{
    RzA1lSocState *s = RZA1L_SOC(obj);

    object_initialize_child(obj, "cpu", &s->cpu, RZA1L_CPU_TYPE);

    object_initialize_child(obj, "rspi0", &s->rspi0, TYPE_RZA1L_RSPI);
    object_initialize_child(obj, "mtu2", &s->mtu2, TYPE_RZA1L_MTU2);
    object_initialize_child(obj, "dmac", &s->dmac, TYPE_RZA1L_DMAC);
    object_initialize_child(obj, "spibsc", &s->spibsc, TYPE_RZA1L_SPIBSC);
    object_initialize_child(obj, "ostm", &s->ostm, TYPE_RZA1L_OSTM);
    object_initialize_child(obj, "gpio", &s->gpio, TYPE_RZA1L_GPIO);
    object_initialize_child(obj, "gic", &s->gic, TYPE_ARM_GIC);
    object_initialize_child(obj, "scif0", &s->scif0, TYPE_RZA1L_SCIF);
    object_initialize_child(obj, "scif1", &s->scif1, TYPE_RZA1L_SCIF);
    object_initialize_child(obj, "cpg", &s->cpg, TYPE_RZA1L_CPG);
    object_initialize_child(obj, "wdt", &s->wdt, TYPE_RZA1L_WDT);
    object_initialize_child(obj, "bsc", &s->bsc, TYPE_RZA1L_BSC);
    object_initialize_child(obj, "oled", &s->oled, TYPE_DELUGE_OLED);
    object_initialize_child(obj, "padgrid", &s->padgrid, TYPE_DELUGE_PADGRID);
    object_initialize_child(obj, "segment", &s->segment, TYPE_DELUGE_SEGMENT);
    object_initialize_child(obj, "input", &s->input, TYPE_DELUGE_INPUT);
    object_initialize_child(obj, "sdhi", &s->sdhi, TYPE_RZA1L_SDHI);

    /*
     * The board points this at its system address space before realize. The
     * SoC maps SRAM/SDRAM/peripherals into it.
     */
    object_property_add_link(obj, "memory", TYPE_MEMORY_REGION,
                             (Object **)&s->system_memory,
                             qdev_prop_allow_set_link_before_realize,
                             OBJ_PROP_LINK_STRONG);
}

static void rza1l_soc_realize(DeviceState *dev, Error **errp)
{
    RzA1lSocState *s = RZA1L_SOC(dev);
    MemoryRegion *system_memory = s->system_memory;
    Error *err = NULL;

    if (system_memory == NULL) {
        error_setg(errp, "%s: 'memory' link property not set", __func__);
        return;
    }

    /*
     * Realize the CPU. The Cortex-A9 in the RZ/A1 has no security extensions
     * exposed to firmware in the usual A-profile sense for this use; keep the
     * defaults and let the board/firmware configure the rest.
     */
    if (!qdev_realize(DEVICE(&s->cpu), NULL, errp)) {
        return;
    }

    /* On-chip SRAM (3 MB). */
    memory_region_init_ram(&s->sram, OBJECT(dev), "rza1l.sram",
                           RZA1L_SRAM_SIZE, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    memory_region_add_subregion(system_memory, RZA1L_SRAM_BASE, &s->sram);

    /* External SDRAM (64 MB). */
    memory_region_init_ram(&s->sdram, OBJECT(dev), "rza1l.sdram",
                           RZA1L_SDRAM_SIZE, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    memory_region_add_subregion(system_memory, RZA1L_SDRAM_BASE, &s->sdram);

    /*
     * Uncached mirrors at +0x40000000. The firmware accesses RAM through these
     * aliases for non-cacheable buffers (DMA, etc.); back them with the same
     * RAM so either view sees the same bytes.
     */
    memory_region_init_alias(&s->sram_mirror, OBJECT(dev), "rza1l.sram.mirror",
                             &s->sram, 0, RZA1L_SRAM_SIZE);
    memory_region_add_subregion(system_memory,
                                RZA1L_SRAM_BASE + RZA1L_UNCACHED_MIRROR_OFFSET,
                                &s->sram_mirror);

    memory_region_init_alias(&s->sdram_mirror, OBJECT(dev),
                             "rza1l.sdram.mirror", &s->sdram, 0,
                             RZA1L_SDRAM_SIZE);
    memory_region_add_subregion(system_memory,
                                RZA1L_SDRAM_BASE + RZA1L_UNCACHED_MIRROR_OFFSET,
                                &s->sdram_mirror);

    /*
     * Low boot-mirror region (0x00000000..SDRAM). On the RZ/A1 the address
     * space below the external memories mirrors the SPI multi-I/O boot flash
     * and reads back real (readable) data. The Deluge firmware occasionally
     * reads from these low addresses — e.g. a debug log that prints the PIC
     * firmware-version byte with "%s", which dereferences the small integer as
     * a string pointer. On hardware that harmlessly reads boot-mirror bytes; in
     * the model the region would otherwise be unmapped and the stray read would
     * raise a data abort. Back it with a catch-all that returns zero so such
     * reads terminate immediately (an empty string) instead of faulting.
     */
    create_unimplemented_device("rza1l.boot.mirror", 0, RZA1L_SDRAM_BASE);

    /*
     * Catch-alls for the three peripheral windows. They log unimplemented
     * accesses (-d unimp) so we can see what the firmware touches and decide
     * which device to model next. Real peripherals are mapped over the top of
     * these with higher priority as they are added.
     */
    create_unimplemented_device("rza1l.io.low",
                                RZA1L_IO_LOW_BASE, RZA1L_IO_LOW_SIZE);
    create_unimplemented_device("rza1l.io.mid",
                                RZA1L_IO_MID_BASE, RZA1L_IO_MID_SIZE);
    create_unimplemented_device("rza1l.io.high",
                                RZA1L_IO_HIGH_BASE, RZA1L_IO_HIGH_SIZE);

    /*
     * RSPI0 (OLED display + CV/gate DAC). Mapped over the io.mid catch-all with
     * higher priority so the firmware's SPI status polls complete.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->rspi0), errp)) {
        return;
    }
    memory_region_add_subregion_overlap(system_memory, RZA1L_RSPI0_BASE,
                                        sysbus_mmio_get_region(
                                            SYS_BUS_DEVICE(&s->rspi0), 0),
                                        1);

    /*
     * OLED panel. The firmware streams its command/pixel bytes through RSPI0's
     * data register, so bind it to RSPI0; the PIC drives its control lines.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->oled), errp)) {
        return;
    }
    rza1l_rspi_set_oled(&s->rspi0, &s->oled);

    /*
     * RGB pad-grid and 7-segment numeric displays. Both are driven entirely by
     * PIC commands; the PIC forwards its decoded state to them for rendering.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->padgrid), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->segment), errp)) {
        return;
    }

    /*
     * Host-input mapping. Carries no MMIO; it registers a keyboard handler that
     * feeds pad/button events into the PIC (bound below, once the PIC exists).
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->input), errp)) {
        return;
    }

    /*
     * MTU2 timer. Provides the free-running counters the firmware uses for
     * busy-wait delays. Mapped over the io.high catch-all.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->mtu2), errp)) {
        return;
    }
    memory_region_add_subregion_overlap(system_memory, RZA1L_MTU2_BASE,
                                        sysbus_mmio_get_region(
                                            SYS_BUS_DEVICE(&s->mtu2), 0),
                                        1);

    /*
     * DMAC. Performs the firmware's polled memory/peripheral transfers
     * synchronously. Mapped over the io.mid catch-all.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->dmac), errp)) {
        return;
    }
    memory_region_add_subregion_overlap(system_memory, RZA1L_DMAC_BASE,
                                        sysbus_mmio_get_region(
                                            SYS_BUS_DEVICE(&s->dmac), 0),
                                        1);

    /*
     * The SSI transmit channel streams the audio TX buffer continuously; mark
     * it so its CRSA advances from virtual time at the sample rate, driving the
     * firmware's audioSampleTimer and all UI timers derived from it.
     */
    rza1l_dmac_register_tx_audio_ring(&s->dmac, RZA1L_SSI_TX_DMA_CH);

    /*
     * SPIBSC0 (serial flash controller). Reports transfers complete so the
     * firmware's flash status/command polls succeed. Mapped over the io.low
     * catch-all.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->spibsc), errp)) {
        return;
    }
    memory_region_add_subregion_overlap(system_memory, RZA1L_SPIBSC_BASE,
                                        sysbus_mmio_get_region(
                                            SYS_BUS_DEVICE(&s->spibsc), 0),
                                        1);

    /*
     * OSTM OS timer. The firmware's scheduler uses OSTM0 as a free-running
     * time base. Mapped over the io.high catch-all.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ostm), errp)) {
        return;
    }
    memory_region_add_subregion_overlap(system_memory, RZA1L_OSTM_BASE,
                                        sysbus_mmio_get_region(
                                            SYS_BUS_DEVICE(&s->ostm), 0),
                                        1);

    /*
     * GPIO ports. The firmware polls port pin registers for encoders, buttons
     * and detect lines. Mapped over the io.high catch-all.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gpio), errp)) {
        return;
    }
    memory_region_add_subregion_overlap(system_memory, RZA1L_GPIO_BASE,
                                        sysbus_mmio_get_region(
                                            SYS_BUS_DEVICE(&s->gpio), 0),
                                        1);

    /*
     * INTC interrupt controller, modelled with QEMU's ARM GIC. The RZ/A1 INTC
     * is a GICv1 whose register layout matches the GIC, so the distributor and
     * CPU-interface MMIO regions are mapped directly at the INTC addresses
     * (over the io.mid catch-all). The GIC's IRQ/FIQ outputs drive the CPU.
     */
    qdev_prop_set_uint32(DEVICE(&s->gic), "revision", 1);
    qdev_prop_set_uint32(DEVICE(&s->gic), "num-cpu", 1);
    qdev_prop_set_uint32(DEVICE(&s->gic), "num-irq", RZA1L_INTC_NUM_IRQ);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gic), errp)) {
        return;
    }
    memory_region_add_subregion_overlap(system_memory, RZA1L_INTC_DIST_BASE,
                                        sysbus_mmio_get_region(
                                            SYS_BUS_DEVICE(&s->gic), 0),
                                        1);
    memory_region_add_subregion_overlap(system_memory, RZA1L_INTC_CPU_BASE,
                                        sysbus_mmio_get_region(
                                            SYS_BUS_DEVICE(&s->gic), 1),
                                        1);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), 0,
                       qdev_get_gpio_in(DEVICE(&s->cpu), ARM_CPU_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), 1,
                       qdev_get_gpio_in(DEVICE(&s->cpu), ARM_CPU_FIQ));

    /*
     * DMAC transfer-end interrupts (DMAINT0..DMAINT15). The firmware drains
     * its UART TX queues (PIC on channel 10, MIDI on channel 11) from these
     * interrupts, so each channel's end-of-transfer line is wired to the GIC
     * at SPI (INTC_ID_DMAINT0 + channel - 32).
     */
    for (int ch = 0; ch < RZA1L_DMAC_NUM_CH; ch++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->dmac), ch,
                           qdev_get_gpio_in(DEVICE(&s->gic),
                                            RZA1L_DMAINT_SPI(ch)));
    }

    /*
     * RSPI0 receive interrupt (SPRI0). The firmware completes each CV/gate word
     * and advances its shared SPI transfer queue from this interrupt
     * (cvSPITransferComplete); without it the queue stalls after the first CV
     * transfer and OLED frames never get sent.
     */
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->rspi0), 0,
                       qdev_get_gpio_in(DEVICE(&s->gic),
                                        RZA1L_RSPI0_SPRI_SPI));

    /*
     * SCIF UART channels 0 (MIDI) and 1 (PIC). SCIF0 is wired to a host
     * character backend (-serial) for MIDI; SCIF1 is wired to the on-board PIC
     * coprocessor model. Each channel's receive interrupt connects to the GIC.
     * Mapped over the io.mid catch-all.
     */
    qdev_prop_set_chr(DEVICE(&s->scif0), "chardev", serial_hd(0));
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->scif0), errp)) {
        return;
    }
    memory_region_add_subregion_overlap(system_memory, RZA1L_SCIF0_BASE,
                                        sysbus_mmio_get_region(
                                            SYS_BUS_DEVICE(&s->scif0), 0),
                                        1);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->scif0), 0,
                       qdev_get_gpio_in(DEVICE(&s->gic), RZA1L_SCIF_RXI0_SPI));

    /*
     * The PIC talks to the firmware over SCIF1 and is read back via DMA
     * channel 12 (PIC_RX_DMA_CHANNEL), so bind it to the DMAC before wiring it
     * as SCIF1's backend.
     */
    s->pic = qemu_chardev_new(NULL, TYPE_DELUGE_PIC, NULL, NULL, &error_abort);
    deluge_pic_set_dma(s->pic, &s->dmac, RZA1L_PIC_RX_DMA_CH);
    deluge_pic_set_oled(s->pic, &s->oled);
    deluge_pic_set_padgrid(s->pic, &s->padgrid);
    deluge_pic_set_segment(s->pic, &s->segment);
    deluge_input_set_pic(DEVICE(&s->input), s->pic);
    qdev_prop_set_chr(DEVICE(&s->scif1), "chardev", s->pic);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->scif1), errp)) {
        return;
    }
    memory_region_add_subregion_overlap(system_memory, RZA1L_SCIF1_BASE,
                                        sysbus_mmio_get_region(
                                            SYS_BUS_DEVICE(&s->scif1), 0),
                                        1);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->scif1), 0,
                       qdev_get_gpio_in(DEVICE(&s->gic), RZA1L_SCIF_RXI1_SPI));

    /*
     * CPG (clock pulse generator + module standby) and WDT (watchdog). Both
     * sit in the io.high region; the firmware programs FRQCR/STBCRn during
     * clock setup and refreshes the watchdog from its main loop. Mapped over
     * the io.high catch-all.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->cpg), errp)) {
        return;
    }
    memory_region_add_subregion_overlap(system_memory, RZA1L_CPG_BASE,
                                        sysbus_mmio_get_region(
                                            SYS_BUS_DEVICE(&s->cpg), 0),
                                        1);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->wdt), errp)) {
        return;
    }
    memory_region_add_subregion_overlap(system_memory, RZA1L_WDT_BASE,
                                        sysbus_mmio_get_region(
                                            SYS_BUS_DEVICE(&s->wdt), 0),
                                        1);

    /*
     * PL310 L2 cache controller. QEMU's built-in "l2x0" model implements the
     * register interface (cache ops always report complete); the firmware's
     * L2 init writes land here instead of the io.low catch-all. Created and
     * mapped dynamically since the model exports no public state type.
     */
    sysbus_create_simple(RZA1L_PL310_TYPE, RZA1L_PL310_BASE, NULL);

    /*
     * BSC (bus state controller). The firmware programs it to bring up the
     * external SDRAM on CS3 (already backed by the sdram RAM region); the
     * model absorbs the init writes. Mapped over the io.low catch-all.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->bsc), errp)) {
        return;
    }
    memory_region_add_subregion_overlap(system_memory, RZA1L_BSC_BASE,
                                        sysbus_mmio_get_region(
                                            SYS_BUS_DEVICE(&s->bsc), 0),
                                        1);

    /*
     * SDHI SD host controller (port 1 / IP1). Exposes an SDBus for an sd-card
     * (attached by the board when an SD drive is provided). The firmware mounts
     * the card on demand, so this is dormant during boot. Mapped over the
     * io.mid catch-all; its combined interrupt is wired to the GIC.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sdhi), errp)) {
        return;
    }
    memory_region_add_subregion_overlap(system_memory, RZA1L_SDHI_BASE,
                                        sysbus_mmio_get_region(
                                            SYS_BUS_DEVICE(&s->sdhi), 0),
                                        1);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->sdhi), 0,
                       qdev_get_gpio_in(DEVICE(&s->gic), RZA1L_SDHI_SPI));
}

static void rza1l_soc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rza1l_soc_realize;
    /* This is a container SoC device; it cannot be user-created on the cmdline. */
    dc->user_creatable = false;
}

static const TypeInfo rza1l_soc_info = {
    .name          = TYPE_RZA1L_SOC,
    .parent        = TYPE_DEVICE,
    .instance_size = sizeof(RzA1lSocState),
    .instance_init = rza1l_soc_init,
    .class_init    = rza1l_soc_class_init,
};

static void rza1l_soc_register_types(void)
{
    type_register_static(&rza1l_soc_info);
}

type_init(rza1l_soc_register_types)
