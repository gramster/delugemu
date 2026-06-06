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
 * Layout of the boot header at the start of a Deluge raw image, emitted by the
 * firmware's start.S after the 8-entry ARM vector table. The on-chip SPI boot
 * ROM uses these to validate the image and decide where to copy and run it.
 */
#define DELUGE_HDR_CODE_START   0x20  /* .word start   - load address          */
#define DELUGE_HDR_CODE_END     0x24  /* .word end                             */
#define DELUGE_HDR_CODE_EXECUTE 0x28  /* .word execute - entry point           */
#define DELUGE_HDR_SIGNATURE    0x2c  /* validation string                     */
#define DELUGE_SIGNATURE        ".BootLoad_ValidProgramTest."
#define DELUGE_HDR_MIN_SIZE     (DELUGE_HDR_SIGNATURE + sizeof(DELUGE_SIGNATURE))

/*
 * Load the firmware passed with -kernel. Accepts either an ELF (firmware built
 * with symbols, loaded by its program headers) or a raw image such as
 * deluge-c1_2_1.bin.
 *
 * The raw image is what the device ships: it begins with the ARM vector table
 * and a boot header carrying the link/entry address (the code is linked to run
 * from on-chip SRAM at e.g. 0x2007b480, past the NOLOAD bss/stacks/MMU table at
 * the SRAM base). On hardware a small SPI-flash boot ROM checks the validation
 * signature, copies the image to its code_start address and branches to
 * code_execute. We do the same: honour the header's addresses rather than
 * assuming the bare SRAM base.
 */
static void deluge_load_firmware(DelugeMachineState *m, MachineState *machine)
{
    const char *filename = machine->kernel_filename;
    uint64_t entry = 0;
    ssize_t loaded;
    char *data = NULL;
    gsize len = 0;
    uint32_t code_start, code_execute;

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

    /* Otherwise treat it as a raw image: read it and inspect the boot header. */
    if (!g_file_get_contents(filename, &data, &len, NULL)) {
        error_report("could not read firmware image '%s'", filename);
        exit(1);
    }

    if (len >= DELUGE_HDR_MIN_SIZE &&
        memcmp(data + DELUGE_HDR_SIGNATURE, DELUGE_SIGNATURE,
               strlen(DELUGE_SIGNATURE)) == 0) {
        code_start = ldl_le_p(data + DELUGE_HDR_CODE_START);
        code_execute = ldl_le_p(data + DELUGE_HDR_CODE_EXECUTE);

        if (code_start < RZA1L_SRAM_BASE ||
            code_start + len > RZA1L_SRAM_BASE + RZA1L_SRAM_SIZE ||
            code_execute < code_start ||
            code_execute >= code_start + len) {
            error_report("firmware boot header addresses out of range "
                         "(code_start=0x%08x execute=0x%08x len=0x%zx)",
                         code_start, code_execute, (size_t)len);
            exit(1);
        }
    } else {
        /* Not a Deluge image with a recognised header; load at the SRAM base. */
        warn_report("firmware image has no Deluge boot signature; loading at "
                    "the SRAM base 0x%08x", (unsigned)RZA1L_SRAM_BASE);
        code_start = RZA1L_SRAM_BASE;
        code_execute = RZA1L_SRAM_BASE;
    }

    /*
     * Register as a ROM blob so it is reloaded on system reset (the SRAM region
     * itself is not restored automatically).
     */
    rom_add_blob_fixed("deluge.firmware", data, len, code_start);
    m->firmware_entry = code_execute;
    g_free(data);
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
