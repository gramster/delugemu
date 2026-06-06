/*
 * Renesas RZ/A1 USB 2.0 host/function controller (register stub)
 *
 * Models the USB200/USB201 register window well enough for the firmware's
 * USB stack to initialise without faulting. No device is ever attached, so
 * status registers report an idle, disconnected bus (SE0 line state, no
 * pending interrupts) and the USBI interrupt line is held deasserted.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_USB_RZA1L_USB_H
#define HW_USB_RZA1L_USB_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RZA1L_USB "rza1l-usb"
OBJECT_DECLARE_SIMPLE_TYPE(RzA1lUsbState, RZA1L_USB)

/* MMIO register window for one USB controller (covers SYSCFG0..PIPExCTR). */
#define RZA1L_USB_MMIO_SIZE 0x200
#define RZA1L_USB_NUM_REGS  (RZA1L_USB_MMIO_SIZE / 2)

struct RzA1lUsbState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;       /* USBIn interrupt to the GIC (held low) */

    /* 16-bit register file; status registers always read back as zero. */
    uint16_t regs[RZA1L_USB_NUM_REGS];
};

#endif /* HW_USB_RZA1L_USB_H */
