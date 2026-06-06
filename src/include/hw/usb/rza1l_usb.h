/*
 * Renesas RZ/A1 USB 2.0 host/function controller
 *
 * Models the USB200/USB201 register window. By default no device is attached,
 * so status registers report an idle, disconnected bus (SE0 line state, no
 * pending interrupts) and the USBI interrupt line is held deasserted.
 *
 * When the "midi" property is set, the controller presents a permanently
 * attached full-speed USB-MIDI device on the host port: it drives the attach
 * interrupt, answers the firmware's control-transfer enumeration over the DCP
 * (pipe 0) with synthetic descriptors, and lets the firmware's USB host-MIDI
 * driver enumerate and configure the device.
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

/* Maximum synthetic control-transfer response we ever return (config + class). */
#define RZA1L_USB_RESP_MAX  128

struct RzA1lUsbState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;       /* USBIn interrupt to the GIC */

    /* Properties. */
    bool midi;          /* present an attached USB-MIDI device on the host port */

    /* 16-bit register file; status registers are synthesised on read. */
    uint16_t regs[RZA1L_USB_NUM_REGS];

    /* Attached-device / enumeration state (only used when midi == true). */
    bool     attached;      /* device currently presented on the bus */
    uint16_t line_state;    /* SYSSTS0.LNST value reported on read */
    uint16_t dev_address;   /* address assigned by SET_ADDRESS */
    bool     configured;    /* SET_CONFIGURATION completed */

    /* Control-transfer (DCP / pipe 0) data stage. */
    uint8_t  resp[RZA1L_USB_RESP_MAX]; /* IN data to return to the host */
    uint32_t resp_len;                 /* total bytes staged */
    uint32_t resp_pos;                 /* read cursor within resp[] */
    bool     ctrl_in;                  /* current control transfer is device->host */
    bool     data_ready;               /* a control IN packet is waiting in CFIFO */
    bool     ctrl_active;              /* a control transfer is awaiting its status stage */

    /* Deferred interrupt delivery (decouples from the triggering MMIO write). */
    QEMUBH  *event_bh;
    uint16_t pending_ints0;  /* INTSTS0 bits to assert from the bottom half */
    uint16_t pending_ints1;  /* INTSTS1 bits to assert from the bottom half */
    uint16_t pending_brdy;   /* BRDYSTS pipe bits to set */
    uint16_t pending_bemp;   /* BEMPSTS pipe bits to set */
    bool     pending_attach; /* drive the initial device attach */
    bool     attach_signalled; /* ATTCH interrupt already raised (one-shot) */
};

#endif /* HW_USB_RZA1L_USB_H */
