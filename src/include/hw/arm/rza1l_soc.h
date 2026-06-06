/*
 * Renesas RZ/A1L SoC model (as used by the Synthstrom Deluge)
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_RZA1L_SOC_H
#define HW_ARM_RZA1L_SOC_H

#include "hw/core/sysbus.h"
#include "target/arm/cpu.h"
#include "qom/object.h"
#include "hw/ssi/rza1l_rspi.h"
#include "hw/ssi/rza1l_spibsc.h"
#include "hw/timer/rza1l_mtu2.h"
#include "hw/timer/rza1l_ostm.h"
#include "hw/dma/rza1l_dmac.h"
#include "hw/gpio/rza1l_gpio.h"
#include "hw/intc/arm_gic.h"
#include "hw/char/rza1l_scif.h"
#include "hw/misc/rza1l_cpg.h"
#include "hw/misc/rza1l_wdt.h"

#define TYPE_RZA1L_SOC "rza1l-soc"
OBJECT_DECLARE_SIMPLE_TYPE(RzA1lSocState, RZA1L_SOC)

/*
 * Guest physical address map.
 *
 * Values marked (fw) are taken directly from the DelugeFirmware linker script
 * (linker_script_rz_a1l.ld). Peripheral bases are placeholders until confirmed
 * against the Renesas RZ/A1L hardware manual; see docs/memory-map.md.
 */

/* On-chip SRAM: 3 MB. (fw) RAM012L */
#define RZA1L_SRAM_BASE   0x20000000
#define RZA1L_SRAM_SIZE   (3 * 1024 * 1024)

/* External SDRAM: 64 MB device, mapped on bus CS3. (fw) */
#define RZA1L_SDRAM_BASE  0x0C000000
#define RZA1L_SDRAM_SIZE  (64 * 1024 * 1024)

/* Memory-mapped SPI flash window. (fw) ROM */
#define RZA1L_SPI_ROM_BASE 0x18000000
#define RZA1L_SPI_ROM_SIZE (32 * 1024 * 1024)

/*
 * The RZ/A1 exposes the on-chip RAM and the external memories a second time at
 * a +0x40000000 offset; the firmware maps this alias as non-cacheable (the
 * "mirror" / uncached view, UNCACHED_MIRROR_OFFSET in the firmware). Model the
 * mirrors as aliases of the backing RAM so accesses through either view work
 * regardless of the guest MMU attributes.
 */
#define RZA1L_UNCACHED_MIRROR_OFFSET 0x40000000

/*
 * On-chip peripheral I/O windows. The RZ/A1 scatters its peripherals across
 * three regions; until each block is modelled the SoC installs logging
 * "unimplemented device" catch-alls over them (-d unimp) so firmware accesses
 * are observed instead of aborting. Real devices are mapped over the top with
 * higher priority as they are added.
 *
 *  - LOW  0x3FE00000..0x3FFFFFFF: SPIBSC, BSC (SDRAM ctrl), PL310 L2 cache.
 *  - MID  0xE8000000..0xE8FFFFFF: INTC/GIC, DMAC, SCIF, RSPI, SSIF, ADC, USB.
 *  - HIGH 0xFCFE0000..0xFCFFFFFF: CPG, STB, WDT, GPIO, OSTM, MTU2, RIIC, RTC.
 */
#define RZA1L_IO_LOW_BASE   0x3FE00000
#define RZA1L_IO_LOW_SIZE   0x00200000
#define RZA1L_IO_MID_BASE   0xE8000000
#define RZA1L_IO_MID_SIZE   0x01000000
#define RZA1L_IO_HIGH_BASE  0xFCFE0000
#define RZA1L_IO_HIGH_SIZE  0x00020000

/* RSPI0 (channel 0) — drives the OLED display and CV/gate DAC. */
#define RZA1L_RSPI0_BASE    0xE800C800

/* MTU2 multi-function timer pulse unit (free-running time base). */
#define RZA1L_MTU2_BASE     0xFCFF0000

/* DMAC — 16-channel direct memory access controller. */
#define RZA1L_DMAC_BASE     0xE8200000

/* SPIBSC0 — SPI multi-I/O bus controller (serial flash). */
#define RZA1L_SPIBSC_BASE   0x3FEFA000/* OSTM — OS timer (two free-running 32-bit channels). */
#define RZA1L_OSTM_BASE     0xFCFEC000

/* GPIO — general-purpose I/O ports (struct base, per the firmware macro). */
#define RZA1L_GPIO_BASE     0xFCFE3004

/* CPG — clock pulse generator + module-standby control (st_cpg base). */
#define RZA1L_CPG_BASE      0xFCFE0010

/* WDT — watchdog timer. */
#define RZA1L_WDT_BASE      0xFCFE0000

/*
 * PL310 (L2C-310) L2 cache controller. Instantiated from QEMU's built-in
 * "l2x0" model purely to absorb the firmware's cache-init register writes.
 */
#define RZA1L_PL310_BASE    0x3FFFF000
#define RZA1L_PL310_TYPE    "l2x0"

/*
 * INTC — the RZ/A1 interrupt controller is an ARM GICv1: the GIC distributor
 * (ICD* registers) sits at 0xE8201000 and the CPU interface (ICC* registers)
 * at 0xE8202000. The firmware uses interrupt IDs up to ~586, so allocate 608
 * lines (a multiple of 32 that covers them plus the 32 internal SGI/PPI IDs).
 */
#define RZA1L_INTC_DIST_BASE 0xE8201000
#define RZA1L_INTC_CPU_BASE  0xE8202000
#define RZA1L_INTC_NUM_IRQ   608

/*
 * SCIF UART channels. The firmware uses channel 0 (MIDI) and channel 1 (PIC);
 * channels are 0x800 apart from SCIF0 at 0xE8007000. Each channel's receive
 * (RXI) interrupt ID is 223 + 4*ch, i.e. GIC SPI line (ID - 32).
 */
#define RZA1L_SCIF0_BASE     0xE8007000
#define RZA1L_SCIF1_BASE     0xE8007800
#define RZA1L_SCIF_RXI0_SPI  (223 - 32)
#define RZA1L_SCIF_RXI1_SPI  (227 - 32)

/* CPU */
#define RZA1L_CPU_TYPE        ARM_CPU_TYPE_NAME("cortex-a9")
#define RZA1L_CPU_CLK_HZ      400000000ULL

struct RzA1lSocState {
    /*< private >*/
    DeviceState parent_obj;

    /*< public >*/
    ARMCPU cpu;

    MemoryRegion sram;
    MemoryRegion sdram;
    MemoryRegion sram_mirror;
    MemoryRegion sdram_mirror;

    RzA1lRspiState rspi0;
    RzA1lMtu2State mtu2;
    RzA1lDmacState dmac;
    RzA1lSpibscState spibsc;
    RzA1lOstmState ostm;
    RzA1lGpioState gpio;
    GICState gic;
    RzA1lScifState scif0;
    RzA1lScifState scif1;
    RzA1lCpgState cpg;
    RzA1lWdtState wdt;

    /*
     * Link to the system memory region the SoC maps into. Set by the board
     * before realize via the "memory" link property.
     */
    MemoryRegion *system_memory;
};

#endif /* HW_ARM_RZA1L_SOC_H */
