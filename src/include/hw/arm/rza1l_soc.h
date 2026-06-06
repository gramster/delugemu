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
 * Peripheral region catch-all. The RZ/A1 maps its on-chip peripherals in the
 * upper part of the address space; until individual blocks are modelled the SoC
 * installs an "unimplemented device" over this window so firmware accesses are
 * logged (-d unimp) instead of aborting. Base/size are intentionally broad and
 * will be narrowed as real devices claim their sub-regions.
 */
#define RZA1L_PERIPH_BASE 0xE8000000
#define RZA1L_PERIPH_SIZE 0x10000000

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
    MemoryRegion periph_unimp;

    /*
     * Link to the system memory region the SoC maps into. Set by the board
     * before realize via the "memory" link property.
     */
    MemoryRegion *system_memory;
};

#endif /* HW_ARM_RZA1L_SOC_H */
