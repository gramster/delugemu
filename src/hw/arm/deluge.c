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
#include "qemu/units.h"
#include "hw/boards.h"
#include "hw/qdev-properties.h"
#include "hw/arm/boot.h"
#include "exec/address-spaces.h"
#include "sysemu/sysemu.h"

#include "hw/arm/rza1l_soc.h"

struct DelugeMachineState {
    /*< private >*/
    MachineState parent_obj;

    /*< public >*/
    RzA1lSocState soc;
};
typedef struct DelugeMachineState DelugeMachineState;

#define TYPE_DELUGE_MACHINE MACHINE_TYPE_NAME("deluge")
DECLARE_INSTANCE_CHECKER(DelugeMachineState, DELUGE_MACHINE,
                         TYPE_DELUGE_MACHINE)

/* Description of the firmware image for QEMU's Arm boot loader. */
static struct arm_boot_info deluge_boot_info;

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

    /*
     * Boot via the firmware ELF passed with -kernel. The Deluge runs bare
     * metal from on-chip SRAM; the ELF entry point and segments drive the
     * initial PC and memory contents.
     */
    deluge_boot_info.ram_size = RZA1L_SDRAM_SIZE;
    deluge_boot_info.board_id = -1;
    deluge_boot_info.loader_start = RZA1L_SRAM_BASE;
    arm_load_kernel(&m->soc.cpu, machine, &deluge_boot_info);
}

static void deluge_machine_class_init(ObjectClass *oc, void *data)
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
};

static void deluge_machine_register_types(void)
{
    type_register_static(&deluge_machine_info);
}

type_init(deluge_machine_register_types)
