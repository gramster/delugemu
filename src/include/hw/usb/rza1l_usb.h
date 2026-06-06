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
 * driver enumerate and configure the device. The bulk MIDI pipes are bridged
 * to a host serial-MIDI chardev (the "chardev" property): bytes arriving on the
 * chardev are framed into 32-bit USB-MIDI event packets and delivered to the
 * firmware over the bulk IN pipe (BRDY), and packets the firmware transmits on
 * the bulk OUT pipe are deframed back to a raw MIDI byte stream on the chardev.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_USB_RZA1L_USB_H
#define HW_USB_RZA1L_USB_H

#include "hw/core/sysbus.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

#define TYPE_RZA1L_USB "rza1l-usb"
OBJECT_DECLARE_SIMPLE_TYPE(RzA1lUsbState, RZA1L_USB)

/* MMIO register window for one USB controller (covers SYSCFG0..PIPExCTR). */
#define RZA1L_USB_MMIO_SIZE 0x200
#define RZA1L_USB_NUM_REGS  (RZA1L_USB_MMIO_SIZE / 2)

/* Maximum synthetic control-transfer response we ever return (config + class). */
#define RZA1L_USB_RESP_MAX  128

/* Bulk MIDI receive queue: complete 4-byte USB-MIDI event packets awaiting
 * delivery to the firmware. Sized to hold a healthy burst of MIDI traffic. */
#define RZA1L_USB_RXQ_BYTES   1024  /* multiple of 4 */
#define RZA1L_USB_BULK_MAXP   64    /* bulk MIDI transfer size (tranlen) */
#define RZA1L_USB_TXBUF_BYTES 64    /* one bulk OUT transfer's worth of packets */

struct RzA1lUsbState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;       /* USBIn interrupt to the GIC */

    /* Properties. */
    bool midi;          /* present an attached USB-MIDI device on the host port */
    CharFrontend chr;   /* host serial-MIDI chardev bridged to the bulk pipes */

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

    /* ---- Bulk MIDI data bridge (only used when midi == true) ------------ */

    /* Bulk IN (device->host): host MIDI framed into USB-MIDI event packets. */
    uint8_t  rx_q[RZA1L_USB_RXQ_BYTES]; /* ring of complete 4-byte packets */
    uint32_t rx_q_head;                 /* dequeue cursor */
    uint32_t rx_q_tail;                 /* enqueue cursor */
    uint32_t rx_q_count;                /* bytes currently queued (mult of 4) */

    /* Current bulk IN transfer latched for the firmware to drain via CFIFO. */
    uint8_t  rx_xfer[RZA1L_USB_BULK_MAXP];
    uint32_t rx_xfer_len;   /* bytes in this transfer (0 = no active transfer) */
    uint32_t rx_xfer_pos;   /* CFIFO read cursor within rx_xfer[] */
    uint16_t rx_xfer_pipe;  /* bulk IN pipe this transfer belongs to */
    uint16_t recv_armed;    /* bitmask of bulk IN pipes armed (PID=BUF) */

    /* Host-MIDI byte-stream parser state (raw MIDI -> USB-MIDI framing). */
    uint8_t  midi_status;   /* running status byte (0 = none) */
    uint8_t  midi_data[2];  /* collected data bytes */
    uint8_t  midi_ndata;    /* data bytes collected so far */
    uint8_t  midi_need;     /* data bytes needed to complete the message */
    bool     in_sysex;      /* inside a SysEx (F0..F7) message */
    uint8_t  sysex_buf[3];  /* SysEx bytes pending a 3-byte packet boundary */
    uint8_t  sysex_len;     /* SysEx bytes buffered */

    /* Bulk OUT (host<-device): packets the firmware transmits over CFIFO. */
    uint8_t  tx_buf[RZA1L_USB_TXBUF_BYTES];
    uint32_t tx_len;        /* bytes accumulated for the current transfer */
};

#endif /* HW_USB_RZA1L_USB_H */
