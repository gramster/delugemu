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
#include "hw/ssi/rza1l_ssif.h"
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
    object_initialize_child(obj, "ssif", &s->ssif, TYPE_RZA1L_SSIF);
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
    object_initialize_child(obj, "adc", &s->adc, TYPE_RZA1L_ADC);
    object_initialize_child(obj, "rtc", &s->rtc, TYPE_RZA1L_RTC);
    object_initialize_child(obj, "oled", &s->oled, TYPE_DELUGE_OLED);
    object_initialize_child(obj, "skin", &s->skin, TYPE_DELUGE_SKIN);
    object_initialize_child(obj, "padgrid", &s->padgrid, TYPE_DELUGE_PADGRID);
    object_initialize_child(obj, "input", &s->input, TYPE_DELUGE_INPUT);
    object_initialize_child(obj, "sdhi", &s->sdhi, TYPE_RZA1L_SDHI);
    object_initialize_child(obj, "usb0", &s->usb0, TYPE_RZA1L_USB);
    object_initialize_child(obj, "usb1", &s->usb1, TYPE_RZA1L_USB);

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
     * Composited front-panel skin (photo background + live overlays).
     * This is a pure UI device: no MMIO, no guest-visible wiring.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->skin), errp)) {
        return;
    }
    deluge_skin_set_oled(DEVICE(&s->skin), &s->oled);

    /*
     * RGB pad-grid display. Driven entirely by PIC commands; the PIC forwards
     * its decoded pad state to this renderer.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->padgrid), errp)) {
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
     * The SSI receive channel streams captured audio into the RX buffer
     * continuously; mark it so its CRDA advances from virtual time at the
     * sample rate, so the firmware's input-latency resync (slowRoutine) tracks
     * a live write position rather than a stuck one.
     */
    rza1l_dmac_register_rx_audio_ring(&s->dmac, RZA1L_SSI_RX_DMA_CH);

    /*
     * SSIF0 (I2S audio). Presents the SSI control/status registers so the
     * firmware's audio bring-up completes, and (with an audio backend) mirrors
     * the transmit DMA ring to QEMU's audio out and feeds the receive ring.
     * Bound to the DMAC's audio transmit/receive channels. Mapped over the
     * io.mid catch-all.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ssif), errp)) {
        return;
    }
    memory_region_add_subregion_overlap(system_memory, RZA1L_SSIF0_BASE,
                                        sysbus_mmio_get_region(
                                            SYS_BUS_DEVICE(&s->ssif), 0),
                                        1);
    rza1l_ssif_set_dma(&s->ssif, &s->dmac, RZA1L_SSI_TX_DMA_CH,
                       RZA1L_SSI_RX_DMA_CH);


    /*
     * USB 2.0 controllers (USB200/USB201). The firmware brings up the Renesas
     * USB stack at start-up; with no cable attached, the register stub reports
     * an idle, disconnected bus so initialisation completes cleanly. Mapped
     * over the io.mid catch-all; interrupts wired below after the GIC.
     *
     * The firmware drives host-mode USB-MIDI on USB200 (IP0) only, so the
     * bulk MIDI bridge attaches to usb0. Its host backend is the second serial
     * chardev (serial_hd(1)), mirroring how SCIF0's DIN MIDI uses serial_hd(0);
     * with no second -serial backend the device simply has no MIDI host.
     */
    if (serial_hd(1)) {
        qdev_prop_set_chr(DEVICE(&s->usb0), "chardev", serial_hd(1));
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->usb0), errp)) {
        return;
    }
    memory_region_add_subregion_overlap(system_memory, RZA1L_USB0_BASE,
                                        sysbus_mmio_get_region(
                                            SYS_BUS_DEVICE(&s->usb0), 0),
                                        1);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->usb1), errp)) {
        return;
    }
    memory_region_add_subregion_overlap(system_memory, RZA1L_USB1_BASE,
                                        sysbus_mmio_get_region(
                                            SYS_BUS_DEVICE(&s->usb1), 0),
                                        1);


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
     * SSI channel-0 FIFO interrupts (idle/error, receive-full, transmit-empty).
     * Wired for completeness; the firmware services audio through DMA, so the
     * model leaves them quiescent.
     */
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->ssif), 0,
                       qdev_get_gpio_in(DEVICE(&s->gic), RZA1L_SSIF_SSII0_SPI));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->ssif), 1,
                       qdev_get_gpio_in(DEVICE(&s->gic), RZA1L_SSIF_RXI0_SPI));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->ssif), 2,
                       qdev_get_gpio_in(DEVICE(&s->gic), RZA1L_SSIF_TXI0_SPI));

    /*
     * USB module interrupts (USBI0/USBI1). Wired for completeness; with no
     * device attached the controllers never raise an event, so the lines stay
     * deasserted.
     */
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->usb0), 0,
                       qdev_get_gpio_in(DEVICE(&s->gic), RZA1L_USB_USBI0_SPI));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->usb1), 0,
                       qdev_get_gpio_in(DEVICE(&s->gic), RZA1L_USB_USBI1_SPI));

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
    rza1l_scif_set_rx_dma(&s->scif0, &s->dmac, RZA1L_MIDI_RX_DMA_CH);
    rza1l_dmac_register_timing_capture(&s->dmac, RZA1L_MIDI_RX_TIMING_DMA_CH,
                                       RZA1L_MIDI_RX_DMA_CH,
                                       RZA1L_SSI_TX_DMA_CH);
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

    /*
     * ADC (supply-voltage sense) and RTC (wall clock) stubs. The firmware
     * polls the ADC for the battery LED; the RTC is presented for completeness.
     * Both overlay their respective I/O catch-all so accesses get plausible
     * values instead of the unimplemented-region default.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->adc), errp)) {
        return;
    }
    memory_region_add_subregion_overlap(system_memory, RZA1L_ADC_BASE,
                                        sysbus_mmio_get_region(
                                            SYS_BUS_DEVICE(&s->adc), 0),
                                        1);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->rtc), errp)) {
        return;
    }
    memory_region_add_subregion_overlap(system_memory, RZA1L_RTC_BASE,
                                        sysbus_mmio_get_region(
                                            SYS_BUS_DEVICE(&s->rtc), 0),
                                        1);
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
