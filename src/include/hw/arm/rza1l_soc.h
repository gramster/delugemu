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

    /*
     * Link to the system memory region the SoC maps into. Set by the board
     * before realize via the "memory" link property.
     */
    MemoryRegion *system_memory;
};

#endif /* HW_ARM_RZA1L_SOC_H */
