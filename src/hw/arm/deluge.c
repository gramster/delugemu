/*
 * Synthstrom Deluge board model
 *
 * Wires the RZ/A1L SoC together with the Deluge-specific peripherals (input
 * PIC, display) and loads firmware via -kernel. Registered as machine "deluge".
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "elf.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/machines-qom.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "system/system.h"

#include "hw/arm/rza1l_soc.h"

struct DelugeMachineState {
    /*< private >*/
    MachineState parent_obj;

    /*< public >*/
    RzA1lSocState soc;

    /* Address the CPU starts executing at after reset (firmware entry). */
    uint64_t firmware_entry;
};
typedef struct DelugeMachineState DelugeMachineState;

#define TYPE_DELUGE_MACHINE MACHINE_TYPE_NAME("deluge")
DECLARE_INSTANCE_CHECKER(DelugeMachineState, DELUGE_MACHINE,
                         TYPE_DELUGE_MACHINE)

/*
 * Reset hook: point the CPU at the firmware entry. Registered after the SoC is
 * realized so it runs after the CPU's own reset, mirroring how the Deluge
 * bootloader hands control to the validated user image in on-chip SRAM.
 */
static void deluge_cpu_reset(void *opaque)
{
    DelugeMachineState *m = opaque;

    cpu_set_pc(CPU(&m->soc.cpu), m->firmware_entry);
}

/*
 * Load the firmware passed with -kernel. Accepts either an ELF (firmware built
 * with symbols, loaded by its program headers) or a raw image such as
 * deluge-c1_2_1.bin. On real hardware a small bootloader in SPI flash validates
 * the image, copies it into on-chip SRAM at 0x20000000 and branches to its
 * "start" entry; we emulate the end state by loading the raw image there and
 * starting execution at the SRAM base.
 */
static void deluge_load_firmware(DelugeMachineState *m, MachineState *machine)
{
    const char *filename = machine->kernel_filename;
    uint64_t entry = 0;
    ssize_t loaded;

    if (filename == NULL) {
        warn_report("no firmware image given (-kernel); CPU will idle at the "
                    "SRAM base");
        m->firmware_entry = RZA1L_SRAM_BASE;
        return;
    }

    /* Prefer ELF: it carries its own load addresses and entry point. */
    loaded = load_elf(filename, NULL, NULL, NULL, &entry, NULL, NULL, NULL,
                      ELFDATA2LSB, EM_ARM, 1, 0);
    if (loaded > 0) {
        m->firmware_entry = entry;
        return;
    }

    /* Otherwise treat it as a raw image and load it into on-chip SRAM. */
    loaded = load_image_targphys(filename, RZA1L_SRAM_BASE, RZA1L_SRAM_SIZE,
                                 &error_fatal);
    if (loaded < 0) {
        error_report("could not load firmware image '%s'", filename);
        exit(1);
    }
    m->firmware_entry = RZA1L_SRAM_BASE;
}

static void deluge_machine_init(MachineState *machine)
{
    DelugeMachineState *m = DELUGE_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();

    /* Instantiate and realize the SoC, handing it the system address space. */
    object_initialize_child(OBJECT(machine), "soc", &m->soc, TYPE_RZA1L_SOC);
    object_property_set_link(OBJECT(&m->soc), "memory",
                             OBJECT(system_memory), &error_abort);
    qdev_realize(DEVICE(&m->soc), NULL, &error_fatal);

    /*
     * TODO(M1+): create and wire the Deluge board peripherals here:
     *   - TYPE_DELUGE_PIC  (input bridge)  -> SoC serial + IRQ
     *   - TYPE_DELUGE_OLED / TYPE_DELUGE_SEGMENT (display)
     *   - audio codec on SSI
     */

    deluge_load_firmware(m, machine);
    qemu_register_reset(deluge_cpu_reset, m);
}

static void deluge_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Synthstrom Deluge (Renesas RZ/A1L, Cortex-A9)";
    mc->init = deluge_machine_init;
    mc->default_cpu_type = RZA1L_CPU_TYPE;
    mc->default_ram_size = RZA1L_SDRAM_SIZE;
    mc->min_cpus = 1;
    mc->max_cpus = 1;
    mc->default_cpus = 1;
    /* RAM is created by the SoC, not the generic machine RAM mechanism. */
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
}

static const TypeInfo deluge_machine_info = {
    .name       = TYPE_DELUGE_MACHINE,
    .parent     = TYPE_MACHINE,
    .instance_size = sizeof(DelugeMachineState),
    .class_init = deluge_machine_class_init,
    /*
     * Required for boards built into the shared arm/aarch64 source set so the
     * machine is exposed in qemu-system-arm (see hw/arm/machines-qom.h).
     */
    .interfaces = arm_machine_interfaces,
};

static void deluge_machine_register_types(void)
{
    type_register_static(&deluge_machine_info);
}

type_init(deluge_machine_register_types)
