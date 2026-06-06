/*
 * Renesas RZ/A1 USB 2.0 host/function controller
 *
 * Models the USB200/USB201 register window (st_usb20, base 0xE8010000 /
 * 0xE8207000). Two modes:
 *
 *   - Default (disconnected): configuration registers are shadowed so the
 *     firmware reads back what it wrote; status registers report an idle bus
 *     (SYSSTS0.LNST = SE0, INTSTS0/1 = 0). The firmware's USB init completes
 *     without a device, falling back to peripheral mode, and the USBI line is
 *     never asserted.
 *
 *   - "midi" property set (host mode with an attached device): the controller
 *     presents a permanently attached full-speed USB-MIDI device. Once the
 *     firmware finishes host-mode init and enables the attach interrupt, the
 *     model raises ATTCH; the firmware then drives bus reset and enumerates the
 *     device over the DCP (pipe 0). The model answers each control transfer
 *     (GET_DESCRIPTOR / SET_ADDRESS / SET_CONFIGURATION) with synthetic
 *     descriptors and generates the SACK / BRDY / BEMP interrupts the
 *     firmware's host control-transfer state machine expects. The firmware
 *     recognises the MIDIStreaming (audio-class) interface, completes
 *     enumeration (SET_CONFIGURATION), and sets up its bulk MIDI pipes.
 *
 *     Bulk MIDI data is bridged to a host serial-MIDI chardev (the "chardev"
 *     property). The firmware selects a bulk pipe onto the CFIFO data port via
 *     CFIFOSEL.CURPIPE and transfers 32-bit USB-MIDI event packets: the bulk IN
 *     pipe (PIPE2..PIPE5) carries host->device MIDI, delivered by framing raw
 *     chardev bytes into packets and raising the pipe's BRDY interrupt; the bulk
 *     OUT pipe (PIPE1) carries device->host MIDI, deframed from the packets the
 *     firmware writes to CFIFO and emitted as a raw MIDI byte stream, with the
 *     transfer acknowledged by the pipe's BEMP interrupt.
 *
 * Register offsets and bit definitions are taken from the firmware's
 * usb20_iodefine.h and r_usb_bitdefine.h.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "migration/vmstate.h"
#include "hw/usb/rza1l_usb.h"

/* ---- Register offsets (st_usb20) ---------------------------------------- */
#define USB_SYSCFG0  0x00
#define USB_SYSSTS0  0x04
#define USB_DVSTCTR0 0x08
#define USB_CFIFO    0x14
#define USB_CFIFOSEL 0x20
#define USB_CFIFOCTR 0x22
#define USB_INTENB0  0x30
#define USB_INTENB1  0x32
#define USB_BRDYENB  0x36
#define USB_NRDYENB  0x38
#define USB_BEMPENB  0x3A
#define USB_INTSTS0  0x40
#define USB_INTSTS1  0x42
#define USB_BRDYSTS  0x46
#define USB_NRDYSTS  0x48
#define USB_BEMPSTS  0x4A
#define USB_FRMNUM   0x4C
#define USB_UFRMNUM  0x4E
#define USB_USBADDR  0x50
#define USB_USBREQ   0x54
#define USB_USBVAL   0x56
#define USB_USBINDX  0x58
#define USB_USBLENG  0x5A
#define USB_DCPCFG   0x5C
#define USB_DCPMAXP  0x5E
#define USB_DCPCTR   0x60
#define USB_PIPESEL  0x64
#define USB_PIPECFG  0x68
#define USB_PIPE1CTR 0x70   /* PIPEnCTR = PIPE1CTR + (n-1)*2, up to PIPE9CTR */

/* ---- Bit definitions ---------------------------------------------------- */
/* SYSSTS0 */
#define USB_LNST     0x0003
#define USB_FS_JSTS  0x0001

/* DVSTCTR0 */
#define USB_USBRST   0x0040
#define USB_UACT     0x0010
#define USB_HSPROC   0x0004
#define USB_RHST     0x0007
#define USB_FSMODE   0x0002

/* CFIFOSEL */
#define USB_ISEL     0x0020
#define USB_CURPIPE  0x000f

/* CFIFOCTR */
#define USB_BVAL     0x8000
#define USB_BCLR     0x4000
#define USB_FRDY     0x2000
#define USB_DTLN     0x0fff

/* INTSTS0 / INTENB0 */
#define USB_BRDY     0x0100
#define USB_NRDY     0x0200
#define USB_BEMP     0x0400

/* INTSTS1 / INTENB1 */
#define USB_SACK     0x0010
#define USB_ATTCH    0x0800

/* DCPCTR */
#define USB_SUREQ    0x4000
#define USB_CCPL     0x0004   /* control transfer end (issue status stage) */

/* PIPEnCTR */
#define USB_PID      0x0003   /* PID field mask */
#define USB_PID_BUF  0x0001   /* BUF: pipe armed for transfer */

/* Bulk MIDI pipe assignment (firmware r_usb_hmidi_config.h). */
#define USB_BULK_SEND_PIPE 1  /* host OUT: Deluge transmits MIDI */
#define USB_BULK_RECV_MIN  2  /* host IN:  Deluge receives MIDI */
#define USB_BULK_RECV_MAX  5

/* Standard control requests (bRequest). */
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_CONFIGURATION 0x09

/* GET_DESCRIPTOR types (wValue high byte). */
#define USB_DT_DEVICE        0x01
#define USB_DT_CONFIGURATION 0x02
#define USB_DT_STRING        0x03

#define RZA1L_USB_DCP_MAXP   64   /* control endpoint max packet size */

static bool rza1l_usb_debug(void)
{
    static int v = -1;
    if (v < 0) {
        v = getenv("RZA1L_USB_DEBUG") != NULL;
    }
    return v;
}

#define USB_DBG(fmt, ...) \
    do { \
        if (rza1l_usb_debug()) { \
            fprintf(stderr, "rza1l-usb: " fmt "\n", ##__VA_ARGS__); \
        } \
    } while (0)

/* ---- Synthetic USB-MIDI device descriptors ----------------------------- */

/* Standard 18-byte device descriptor: class defined at interface level. */
static const uint8_t usb_midi_device_desc[18] = {
    0x12,             /* bLength                                            */
    0x01,             /* bDescriptorType = DEVICE                           */
    0x00, 0x02,       /* bcdUSB = 2.00                                      */
    0x00,             /* bDeviceClass (per-interface)                       */
    0x00,             /* bDeviceSubClass                                    */
    0x00,             /* bDeviceProtocol                                    */
    RZA1L_USB_DCP_MAXP, /* bMaxPacketSize0 = 64                             */
    0x66, 0x66,       /* idVendor  (synthetic)                              */
    0x01, 0x01,       /* idProduct (synthetic)                             */
    0x00, 0x01,       /* bcdDevice = 1.00                                   */
    0x00,             /* iManufacturer (no strings)                         */
    0x00,             /* iProduct                                           */
    0x00,             /* iSerialNumber                                      */
    0x01,             /* bNumConfigurations                                 */
};

/*
 * Configuration descriptor for a minimal USB-MIDI device:
 *   config -> AudioControl interface -> MIDIStreaming interface
 *   -> bulk OUT endpoint -> bulk IN endpoint, each with a class-specific
 *   MS endpoint descriptor binding one embedded MIDI jack.
 */
static const uint8_t usb_midi_config_desc[] = {
    /* Configuration descriptor (9) */
    0x09, 0x02, 0x65, 0x00, 0x02, 0x01, 0x00, 0x80, 0x32,
    /* Interface 0: Audio Control (9) */
    0x09, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    /* AC interface header (9) */
    0x09, 0x24, 0x01, 0x00, 0x01, 0x09, 0x00, 0x01, 0x01,
    /* Interface 1: MIDIStreaming (9) */
    0x09, 0x04, 0x01, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00,
    /* MS interface header (7) */
    0x07, 0x24, 0x01, 0x00, 0x01, 0x41, 0x00,
    /* MIDI IN jack (embedded) (6) */
    0x06, 0x24, 0x02, 0x01, 0x01, 0x00,
    /* MIDI OUT jack (embedded) (9) */
    0x09, 0x24, 0x03, 0x01, 0x02, 0x01, 0x01, 0x01, 0x00,
    /* Bulk OUT endpoint (9) */
    0x09, 0x05, 0x01, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00,
    /* MS bulk OUT class endpoint (5) */
    0x05, 0x25, 0x01, 0x01, 0x01,
    /* Bulk IN endpoint (9) */
    0x09, 0x05, 0x81, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00,
    /* MS bulk IN class endpoint (5) */
    0x05, 0x25, 0x01, 0x01, 0x03,
};

/* String descriptor 0: supported language IDs (English, US). */
static const uint8_t usb_midi_string_langid[4] = {
    0x04, 0x03, 0x09, 0x04,
};

/* Minimal empty string for any other index (device declares no string ids). */
static const uint8_t usb_midi_string_empty[2] = {
    0x02, 0x03,
};

/* ---- Interrupt handling ------------------------------------------------- */

/*
 * INTSTS0.BRDY/NRDY/BEMP are summary bits: each is the OR of the corresponding
 * per-pipe status register masked by its enable. Synthesise them live so that
 * clearing a pipe's BRDYSTS/BEMPSTS bit also clears the INTSTS0 summary, just
 * like the hardware. Other INTSTS0 bits are stored normally.
 */
static uint16_t rza1l_usb_intsts0(RzA1lUsbState *s)
{
    uint16_t v = s->regs[USB_INTSTS0 >> 1] &
                 ~(uint16_t)(USB_BRDY | USB_NRDY | USB_BEMP);

    if (s->regs[USB_BRDYSTS >> 1] & s->regs[USB_BRDYENB >> 1]) {
        v |= USB_BRDY;
    }
    if (s->regs[USB_NRDYSTS >> 1] & s->regs[USB_NRDYENB >> 1]) {
        v |= USB_NRDY;
    }
    if (s->regs[USB_BEMPSTS >> 1] & s->regs[USB_BEMPENB >> 1]) {
        v |= USB_BEMP;
    }
    return v;
}

static void rza1l_usb_update_irq(RzA1lUsbState *s)
{
    uint16_t ints0 = rza1l_usb_intsts0(s) & s->regs[USB_INTENB0 >> 1];
    uint16_t ints1 = s->regs[USB_INTSTS1 >> 1] & s->regs[USB_INTENB1 >> 1];
    int level = (ints0 || ints1) ? 1 : 0;

    qemu_set_irq(s->irq, level);
}

/*
 * Bottom half: apply any interrupt events queued by an MMIO write, decoupled
 * from the write itself so the firmware sees the interrupt on a later tick
 * (matching hardware latency and avoiding reentrancy into the register file).
 */
static void rza1l_usb_event(void *opaque)
{
    RzA1lUsbState *s = opaque;

    if (s->pending_attach) {
        s->pending_attach = false;
        s->regs[USB_INTSTS1 >> 1] |= USB_ATTCH;
        USB_DBG("raise ATTCH (device present)");
    }
    if (s->pending_brdy) {
        s->regs[USB_BRDYSTS >> 1] |= s->pending_brdy;
        s->pending_brdy = 0;
    }
    if (s->pending_bemp) {
        s->regs[USB_BEMPSTS >> 1] |= s->pending_bemp;
        s->pending_bemp = 0;
    }
    s->regs[USB_INTSTS0 >> 1] |= s->pending_ints0;
    s->regs[USB_INTSTS1 >> 1] |= s->pending_ints1;
    s->pending_ints0 = 0;
    s->pending_ints1 = 0;

    rza1l_usb_update_irq(s);
}

static void rza1l_usb_schedule_event(RzA1lUsbState *s)
{
    qemu_bh_schedule(s->event_bh);
}

/* ---- Bulk MIDI data bridge ---------------------------------------------- */

/* Per-pipe interrupt status bit (BRDYSTS/BEMPSTS/BRDYENB/...): pipe n -> 1<<n. */
static inline uint16_t rza1l_usb_pipe_bit(uint16_t pipe)
{
    return (uint16_t)(1u << pipe);
}

/* Number of MIDI bytes carried by a USB-MIDI event packet, indexed by CIN. */
static const uint8_t usb_midi_cin_len[16] = {
    0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1,
};

/*
 * Enqueue one complete 4-byte USB-MIDI event packet onto the bulk IN ring.
 * Packets are dropped if the firmware has fallen too far behind (the ring is
 * full) - the same lossy behaviour a real MIDI input would exhibit.
 */
static void rza1l_usb_rxq_push(RzA1lUsbState *s, uint8_t cin, uint8_t b0,
                               uint8_t b1, uint8_t b2)
{
    if (s->rx_q_count + 4 > RZA1L_USB_RXQ_BYTES) {
        USB_DBG("bulk IN ring full, dropping MIDI packet");
        return;
    }
    s->rx_q[s->rx_q_tail] = cin;
    s->rx_q[(s->rx_q_tail + 1) % RZA1L_USB_RXQ_BYTES] = b0;
    s->rx_q[(s->rx_q_tail + 2) % RZA1L_USB_RXQ_BYTES] = b1;
    s->rx_q[(s->rx_q_tail + 3) % RZA1L_USB_RXQ_BYTES] = b2;
    s->rx_q_tail = (s->rx_q_tail + 4) % RZA1L_USB_RXQ_BYTES;
    s->rx_q_count += 4;
}

/*
 * Frame one raw host-MIDI byte into USB-MIDI event packets (cable 0). Handles
 * channel-voice messages (with running status), system common / real-time
 * messages, and SysEx. Completed packets are pushed onto the bulk IN ring.
 */
static void rza1l_usb_midi_frame_byte(RzA1lUsbState *s, uint8_t b)
{
    if (b >= 0xf8) {
        /* System real-time: a single status byte, may interleave anywhere. */
        rza1l_usb_rxq_push(s, 0x0f, b, 0, 0);
        return;
    }

    if (b & 0x80) {
        /* Status byte. */
        if (b == 0xf0) {
            s->in_sysex = true;
            s->sysex_buf[0] = 0xf0;
            s->sysex_len = 1;
            return;
        }
        if (b == 0xf7) {
            if (s->in_sysex) {
                s->sysex_buf[s->sysex_len++] = 0xf7;
                switch (s->sysex_len) {
                case 1:
                    rza1l_usb_rxq_push(s, 0x05, 0xf7, 0, 0);
                    break;
                case 2:
                    rza1l_usb_rxq_push(s, 0x06, s->sysex_buf[0],
                                       s->sysex_buf[1], 0);
                    break;
                default:
                    rza1l_usb_rxq_push(s, 0x07, s->sysex_buf[0],
                                       s->sysex_buf[1], s->sysex_buf[2]);
                    break;
                }
                s->in_sysex = false;
                s->sysex_len = 0;
            }
            return;
        }
        /* Any other status terminates an in-progress SysEx. */
        s->in_sysex = false;
        s->sysex_len = 0;

        if (b < 0xf0) {
            /* Channel-voice status: set running status. */
            s->midi_status = b;
            s->midi_ndata = 0;
            s->midi_need = (b < 0xc0 || b >= 0xe0) ? 2 : 1;
        } else {
            /* System common (0xf1..0xf6). */
            s->midi_status = b;
            s->midi_ndata = 0;
            switch (b) {
            case 0xf2: s->midi_need = 2; break;       /* song position */
            case 0xf1: case 0xf3: s->midi_need = 1; break; /* MTC / song sel */
            default:                                   /* f4,f5,f6: no data */
                rza1l_usb_rxq_push(s, 0x05, b, 0, 0);
                s->midi_status = 0;
                s->midi_need = 0;
                break;
            }
        }
        return;
    }

    /* Data byte. */
    if (s->in_sysex) {
        s->sysex_buf[s->sysex_len++] = b;
        if (s->sysex_len == 3) {
            rza1l_usb_rxq_push(s, 0x04, s->sysex_buf[0], s->sysex_buf[1],
                               s->sysex_buf[2]);
            s->sysex_len = 0;
        }
        return;
    }

    if (s->midi_status == 0) {
        return; /* orphan data byte */
    }

    s->midi_data[s->midi_ndata++] = b;
    if (s->midi_ndata < s->midi_need) {
        return;
    }

    /* A complete message has been collected; emit its USB-MIDI packet. */
    if (s->midi_status < 0xf0) {
        uint8_t cin = s->midi_status >> 4;
        rza1l_usb_rxq_push(s, cin, s->midi_status, s->midi_data[0],
                           s->midi_need == 2 ? s->midi_data[1] : 0);
        /* Channel-voice messages support running status. */
        s->midi_ndata = 0;
    } else {
        uint8_t cin = (s->midi_status == 0xf2) ? 0x03 : 0x02;
        rza1l_usb_rxq_push(s, cin, s->midi_status, s->midi_data[0],
                           s->midi_need == 2 ? s->midi_data[1] : 0);
        s->midi_status = 0; /* system common does not run */
        s->midi_ndata = 0;
    }
}

/*
 * If a bulk IN pipe is armed (PID=BUF), its BRDY interrupt is enabled, no
 * transfer is already in flight, and packets are queued, latch up to one
 * bulk-max-packet transfer and raise the pipe's BRDY interrupt so the firmware
 * drains it via CFIFO.
 */
static void rza1l_usb_try_deliver_rx(RzA1lUsbState *s)
{
    uint16_t pipe;

    if (!s->midi || s->rx_xfer_len != 0 || s->rx_q_count == 0) {
        return;
    }

    for (pipe = USB_BULK_RECV_MIN; pipe <= USB_BULK_RECV_MAX; pipe++) {
        uint16_t bit = rza1l_usb_pipe_bit(pipe);

        if (!(s->recv_armed & bit)) {
            continue;
        }
        if (!(s->regs[USB_BRDYENB >> 1] & bit)) {
            continue;
        }

        /* Latch up to one max-size transfer from the ring. */
        uint32_t n = MIN(s->rx_q_count, RZA1L_USB_BULK_MAXP);
        uint32_t i;
        for (i = 0; i < n; i++) {
            s->rx_xfer[i] = s->rx_q[s->rx_q_head];
            s->rx_q_head = (s->rx_q_head + 1) % RZA1L_USB_RXQ_BYTES;
        }
        s->rx_q_count -= n;
        s->rx_xfer_len = n;
        s->rx_xfer_pos = 0;
        s->rx_xfer_pipe = pipe;

        /* This transfer consumes the pipe's armed state; the firmware re-arms
         * (PID=BUF) after draining the data in checkIncomingUsbMidi(). */
        s->recv_armed &= ~bit;

        s->pending_brdy |= bit;
        rza1l_usb_schedule_event(s);
        USB_DBG("bulk IN pipe %u: deliver %u bytes (BRDY)", pipe, n);
        return;
    }
}

/*
 * Deframe the bulk OUT transfer the firmware just committed (CFIFO + BVAL) back
 * into a raw MIDI byte stream and write it to the host chardev.
 */
static void rza1l_usb_flush_tx(RzA1lUsbState *s)
{
    uint32_t i;

    for (i = 0; i + 4 <= s->tx_len; i += 4) {
        uint8_t cin = s->tx_buf[i] & 0x0f;
        uint8_t len = usb_midi_cin_len[cin];
        if (len) {
            /* qemu_chr_fe_write_all is a no-op if no chardev is connected. */
            qemu_chr_fe_write_all(&s->chr, &s->tx_buf[i + 1], len);
        }
    }
    s->tx_len = 0;
}

/* ---- Control-transfer engine (DCP / pipe 0) ----------------------------- */

/*
 * The firmware has written USBREQ/USBVAL/USBINDX/USBLENG and triggered the
 * SETUP packet (DCPCTR.SUREQ). Decode the request, stage any IN response, and
 * queue the SETUP-ACK interrupt. Data-stage BRDY / status-stage BEMP follow as
 * the firmware advances the transfer.
 */
static void rza1l_usb_setup(RzA1lUsbState *s)
{
    uint16_t req = s->regs[USB_USBREQ >> 1];
    uint16_t val = s->regs[USB_USBVAL >> 1];
    uint16_t leng = s->regs[USB_USBLENG >> 1];
    uint8_t bmreqtype = req & 0xff;
    uint8_t brequest = (req >> 8) & 0xff;

    s->resp_len = 0;
    s->resp_pos = 0;
    s->data_ready = false;
    s->ctrl_in = (bmreqtype & 0x80) != 0;
    s->ctrl_active = true;

    USB_DBG("SETUP bmReqType=0x%02x bRequest=0x%02x wValue=0x%04x wLength=%u",
            bmreqtype, brequest, val, leng);

    switch (brequest) {
    case USB_REQ_GET_DESCRIPTOR: {
        uint8_t type = (val >> 8) & 0xff;
        const uint8_t *src = NULL;
        uint32_t srclen = 0;

        if (type == USB_DT_DEVICE) {
            src = usb_midi_device_desc;
            srclen = sizeof(usb_midi_device_desc);
        } else if (type == USB_DT_CONFIGURATION) {
            src = usb_midi_config_desc;
            srclen = sizeof(usb_midi_config_desc);
        } else if (type == USB_DT_STRING) {
            if ((val & 0xff) == 0) {
                src = usb_midi_string_langid;
                srclen = sizeof(usb_midi_string_langid);
            } else {
                src = usb_midi_string_empty;
                srclen = sizeof(usb_midi_string_empty);
            }
        }
        if (src) {
            uint32_t n = MIN(leng, srclen);
            n = MIN(n, RZA1L_USB_RESP_MAX);
            memcpy(s->resp, src, n);
            s->resp_len = n;
            s->data_ready = (n > 0);
        }
        break;
    }
    case USB_REQ_SET_ADDRESS:
        s->dev_address = val;
        break;
    case USB_REQ_SET_CONFIGURATION:
        s->configured = (val != 0);
        USB_DBG("SET_CONFIGURATION %u -> configured=%d", val, s->configured);
        break;
    default:
        USB_DBG("unhandled control request 0x%02x", brequest);
        break;
    }

    /* SETUP transaction acknowledged by the (synthetic) device. */
    s->pending_ints1 |= USB_SACK;

    rza1l_usb_schedule_event(s);
}

/* ---- MMIO read ---------------------------------------------------------- */

static uint16_t rza1l_usb_read16(RzA1lUsbState *s, hwaddr offset)
{
    switch (offset) {
    case USB_SYSSTS0:
        /* Report the modelled line state; host-sequencer bits read 0. */
        return s->line_state & USB_LNST;

    case USB_DVSTCTR0: {
        uint16_t v = s->regs[offset >> 1] & ~(uint16_t)(USB_RHST | USB_HSPROC);
        /* Once the bus is active with a device, report full-speed, no HSPROC. */
        if (s->attached && (s->regs[offset >> 1] & USB_UACT)) {
            v |= USB_FSMODE;
        }
        return v;
    }

    case USB_CFIFOCTR: {
        uint16_t pipe = s->regs[USB_CFIFOSEL >> 1] & USB_CURPIPE;

        /* Bulk IN pipe with a latched transfer: report FRDY and the number of
         * bytes the firmware should read out of CFIFO. */
        if (pipe >= USB_BULK_RECV_MIN && pipe <= USB_BULK_RECV_MAX &&
            pipe == s->rx_xfer_pipe && s->rx_xfer_len > s->rx_xfer_pos) {
            uint32_t rem = s->rx_xfer_len - s->rx_xfer_pos;
            return USB_FRDY | (rem & USB_DTLN);
        }

        /* Control IN data: FIFO ready, DTLN = bytes left in this packet. */
        if (s->data_ready) {
            uint32_t rem = s->resp_len - s->resp_pos;
            uint32_t pkt = MIN(rem, RZA1L_USB_DCP_MAXP);
            return USB_FRDY | (pkt & USB_DTLN);
        }
        /* Otherwise the FIFO is ready/empty (e.g. bulk OUT write, no IN data). */
        return USB_FRDY;
    }

    case USB_INTSTS0:
        return rza1l_usb_intsts0(s);

    case USB_INTSTS1:
    case USB_BRDYSTS:
    case USB_NRDYSTS:
    case USB_BEMPSTS:
        return s->regs[offset >> 1];

    case USB_FRMNUM:
    case USB_UFRMNUM:
        return 0;

    default:
        return s->regs[offset >> 1];
    }
}

static uint32_t rza1l_usb_read_cfifo(RzA1lUsbState *s)
{
    uint16_t pipe = s->regs[USB_CFIFOSEL >> 1] & USB_CURPIPE;
    uint32_t v = 0;
    int i;

    /* Bulk IN pipe: drain the latched transfer four bytes at a time, LE. */
    if (pipe >= USB_BULK_RECV_MIN && pipe <= USB_BULK_RECV_MAX &&
        pipe == s->rx_xfer_pipe && s->rx_xfer_len > 0) {
        for (i = 0; i < 4; i++) {
            if (s->rx_xfer_pos < s->rx_xfer_len) {
                v |= (uint32_t)s->rx_xfer[s->rx_xfer_pos++] << (8 * i);
            }
        }
        if (s->rx_xfer_pos >= s->rx_xfer_len) {
            /* Transfer drained; release the latch and deliver any backlog once
             * the firmware re-arms the pipe. */
            s->rx_xfer_len = 0;
            s->rx_xfer_pos = 0;
            s->rx_xfer_pipe = 0;
        }
        return v;
    }

    /* Return up to four bytes of the staged control-IN response, LE order. */
    for (i = 0; i < 4; i++) {
        if (s->resp_pos < s->resp_len) {
            v |= (uint32_t)s->resp[s->resp_pos++] << (8 * i);
        }
    }
    if (s->resp_pos >= s->resp_len) {
        /* Whole response consumed; the final (short) packet ends the stage. */
        s->data_ready = false;
    } else if ((s->resp_pos % RZA1L_USB_DCP_MAXP) == 0) {
        /*
         * A full max-size packet has just been drained and more data remains:
         * the controller makes the next packet available, raising BRDY again.
         */
        s->pending_brdy |= 0x0001;
        rza1l_usb_schedule_event(s);
    }
    return v;
}

/*
 * CFIFO write. For the bulk OUT (send) pipe the firmware streams 32-bit
 * USB-MIDI event packets here before committing the transfer with CFIFOCTR.BVAL;
 * accumulate them. Writes on any other pipe (control OUT status stages) are
 * accepted and discarded.
 */
static void rza1l_usb_write_cfifo(RzA1lUsbState *s, uint32_t value)
{
    uint16_t pipe = s->regs[USB_CFIFOSEL >> 1] & USB_CURPIPE;
    int i;

    if (s->midi && pipe == USB_BULK_SEND_PIPE) {
        for (i = 0; i < 4; i++) {
            if (s->tx_len < RZA1L_USB_TXBUF_BYTES) {
                s->tx_buf[s->tx_len++] = (value >> (8 * i)) & 0xff;
            }
        }
    }
}

static uint64_t rza1l_usb_read(void *opaque, hwaddr offset, unsigned size)
{
    RzA1lUsbState *s = opaque;
    uint64_t value;

    if (offset == USB_CFIFO) {
        return rza1l_usb_read_cfifo(s);
    }

    switch (size) {
    case 1: {
        uint16_t word = rza1l_usb_read16(s, offset & ~1u);
        value = (offset & 1) ? (word >> 8) : (word & 0xff);
        break;
    }
    case 2:
        value = rza1l_usb_read16(s, offset);
        break;
    case 4:
        value = rza1l_usb_read16(s, offset) |
                ((uint32_t)rza1l_usb_read16(s, offset + 2) << 16);
        break;
    default:
        value = 0;
        break;
    }
    return value;
}

/* ---- MMIO write --------------------------------------------------------- */

static void rza1l_usb_write16(RzA1lUsbState *s, hwaddr offset, uint16_t value)
{
    switch (offset) {
    case USB_INTSTS0:
    case USB_INTSTS1:
    case USB_BRDYSTS:
    case USB_NRDYSTS:
    case USB_BEMPSTS:
        /* Write-0-to-clear status: the firmware ANDs to acknowledge. */
        s->regs[offset >> 1] &= value;
        rza1l_usb_update_irq(s);
        return;

    case USB_INTENB0:
    case USB_INTENB1:
        s->regs[offset >> 1] = value;
        /*
         * hw_usb_hmodule_init() detects the already-attached device via
         * SYSSTS0 and performs the bus reset inline, then enables the attach
         * interrupt. The MGR task, however, only starts enumeration in
         * response to an ATTCH interrupt. Mirror real hardware (which raises
         * ATTCH once DRPD sees the device) by delivering a one-shot ATTCH the
         * first time the firmware arms it with a device present.
         */
        if (offset == USB_INTENB1 && s->midi && s->attached &&
            !s->attach_signalled && (value & USB_ATTCH)) {
            s->attach_signalled = true;
            s->pending_attach = true;
            rza1l_usb_schedule_event(s);
        }
        rza1l_usb_update_irq(s);
        return;

    case USB_DCPCTR:
        /*
         * SUREQ (issue SETUP) and CCPL (complete control transfer / status
         * stage) are self-clearing command bits. Process the command and store
         * the register with them cleared so the firmware's read-back polls see
         * the operation has finished.
         */
        if (s->midi && (value & USB_SUREQ)) {
            s->regs[offset >> 1] = value & ~USB_SUREQ;
            rza1l_usb_setup(s);
        } else if (s->midi && (value & USB_CCPL)) {
            s->regs[offset >> 1] = value & ~USB_CCPL;
            /* Status stage completed via CCPL -> pipe-0 BEMP. */
            if (s->ctrl_active) {
                s->ctrl_active = false;
                s->pending_bemp |= 0x0001;
                rza1l_usb_schedule_event(s);
            }
        } else {
            s->regs[offset >> 1] = value;
        }
        return;

    case USB_BRDYENB:
        s->regs[offset >> 1] = value;
        if (s->midi && (value & 0x0001)) {
            if (s->data_ready && s->resp_pos < s->resp_len) {
                /*
                 * Control-read data stage: the firmware is asking for the IN
                 * descriptor data staged at SETUP time.
                 */
                s->pending_brdy |= 0x0001;
                rza1l_usb_schedule_event(s);
            } else if (s->ctrl_active && !s->ctrl_in) {
                /*
                 * Status stage of a no-data control write (SET_ADDRESS /
                 * SET_CONFIGURATION) is an IN zero-length packet, completed via
                 * pipe-0 BRDY.
                 */
                s->ctrl_active = false;
                s->pending_brdy |= 0x0001;
                rza1l_usb_schedule_event(s);
            }
        }
        /* Enabling a bulk IN pipe's BRDY may unblock a queued transfer. */
        rza1l_usb_try_deliver_rx(s);
        /*
         * Re-evaluate the interrupt: a per-pipe BRDYSTS bit may already be
         * latched (the status is set independently of its enable), so enabling
         * it here must raise the level-triggered interrupt immediately.
         */
        rza1l_usb_update_irq(s);
        return;

    case USB_BEMPENB:
        s->regs[offset >> 1] = value;
        /*
         * The firmware arms pipe-0 BEMP to await the control transfer's status
         * stage (a zero-length packet driven via CFIFO/BVAL). Once the data
         * stage (if any) has been consumed, signal that the status stage has
         * completed.
         */
        if (s->midi && (value & 0x0001) && s->ctrl_active && !s->data_ready) {
            s->ctrl_active = false;
            s->pending_bemp |= 0x0001;
            rza1l_usb_schedule_event(s);
        }
        /*
         * Re-evaluate the interrupt: the bulk OUT transfer completes (BEMPSTS
         * latched) as soon as the firmware commits the FIFO with BVAL, which
         * happens before it enables this pipe's BEMP. Enabling it here must
         * therefore raise the level-triggered interrupt for the already-latched
         * completion so multi-transfer sends continue.
         */
        rza1l_usb_update_irq(s);
        return;

    case USB_CFIFOCTR: {
        uint16_t pipe = s->regs[USB_CFIFOSEL >> 1] & USB_CURPIPE;
        /*
         * BVAL on the bulk OUT pipe commits a transmit buffer: deframe the
         * accumulated USB-MIDI packets to raw MIDI on the chardev and signal
         * transmit completion via the pipe's BEMP interrupt. BCLR discards a
         * (zero-length) buffer. Pipe-0 control writes are no-ops here.
         */
        if (s->midi && pipe == USB_BULK_SEND_PIPE && (value & USB_BVAL)) {
            rza1l_usb_flush_tx(s);
            s->pending_bemp |= rza1l_usb_pipe_bit(USB_BULK_SEND_PIPE);
            rza1l_usb_schedule_event(s);
        } else if (value & USB_BCLR) {
            s->tx_len = 0;
        }
        return;
    }

    case USB_PIPE1CTR + 0:  /* PIPE1CTR */
    case USB_PIPE1CTR + 2:  /* PIPE2CTR */
    case USB_PIPE1CTR + 4:  /* PIPE3CTR */
    case USB_PIPE1CTR + 6:  /* PIPE4CTR */
    case USB_PIPE1CTR + 8:  /* PIPE5CTR */
    case USB_PIPE1CTR + 10: /* PIPE6CTR */
    case USB_PIPE1CTR + 12: /* PIPE7CTR */
    case USB_PIPE1CTR + 14: /* PIPE8CTR */
    case USB_PIPE1CTR + 16: /* PIPE9CTR */ {
        uint16_t pipe = ((offset - USB_PIPE1CTR) >> 1) + 1;
        s->regs[offset >> 1] = value;
        /*
         * Writing PID=BUF to a bulk IN pipe arms it to receive: the firmware
         * is ready for the next USB-MIDI transfer. Deliver any queued data.
         */
        if (s->midi && pipe >= USB_BULK_RECV_MIN && pipe <= USB_BULK_RECV_MAX &&
            (value & USB_PID) == USB_PID_BUF) {
            s->recv_armed |= rza1l_usb_pipe_bit(pipe);
            USB_DBG("arm bulk IN pipe %u (PID=BUF), recv_armed=0x%x",
                    pipe, s->recv_armed);
            rza1l_usb_try_deliver_rx(s);
        }
        return;
    }

    case USB_SYSSTS0:
        return; /* read-only */

    default:
        s->regs[offset >> 1] = value;
        return;
    }
}

static void rza1l_usb_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    RzA1lUsbState *s = opaque;

    if (offset == USB_CFIFO) {
        rza1l_usb_write_cfifo(s, value);
        return;
    }

    switch (size) {
    case 1: {
        hwaddr word_off = offset & ~1u;
        uint16_t word = s->regs[word_off >> 1];
        if (offset & 1) {
            word = (word & 0x00ff) | ((value & 0xff) << 8);
        } else {
            word = (word & 0xff00) | (value & 0xff);
        }
        rza1l_usb_write16(s, word_off, word);
        break;
    }
    case 2:
        rza1l_usb_write16(s, offset, value);
        break;
    case 4:
        rza1l_usb_write16(s, offset, value & 0xffff);
        rza1l_usb_write16(s, offset + 2, (value >> 16) & 0xffff);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps rza1l_usb_ops = {
    .read = rza1l_usb_read,
    .write = rza1l_usb_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/* ---- Host MIDI chardev (bulk IN source) -------------------------------- */

/*
 * Flow control for the host->device direction: only accept as many raw MIDI
 * bytes as could still fit in the bulk IN ring once framed. Each raw byte
 * frames into at most one 4-byte packet, so report a quarter of the free ring.
 */
static int rza1l_usb_chr_can_receive(void *opaque)
{
    RzA1lUsbState *s = opaque;
    uint32_t free_bytes = RZA1L_USB_RXQ_BYTES - s->rx_q_count;

    return free_bytes / 4;
}

static void rza1l_usb_chr_receive(void *opaque, const uint8_t *buf, int size)
{
    RzA1lUsbState *s = opaque;
    int i;

    USB_DBG("chardev RX %d bytes (first=0x%02x)", size, size ? buf[0] : 0);
    for (i = 0; i < size; i++) {
        rza1l_usb_midi_frame_byte(s, buf[i]);
    }
    USB_DBG("after framing: rx_q_count=%u recv_armed=0x%x brdyenb=0x%x",
            s->rx_q_count, s->recv_armed, s->regs[USB_BRDYENB >> 1]);
    rza1l_usb_try_deliver_rx(s);
}

static void rza1l_usb_reset(DeviceState *dev)
{
    RzA1lUsbState *s = RZA1L_USB(dev);

    memset(s->regs, 0, sizeof(s->regs));
    /*
     * When acting as a USB-MIDI device, present a full-speed device on the
     * host port from boot. The firmware's synchronous host init
     * (hw_usb_hmodule_init) samples SYSSTS0.LNST via usb_chattaring(); seeing
     * FS_JSTS there sets anythingInitiallyAttachedAsUSBHost, keeps the
     * controller in host mode, and drives the bus reset + enumeration inline.
     */
    s->attached = s->midi;
    s->line_state = s->midi ? USB_FS_JSTS : 0;
    s->dev_address = 0;
    s->configured = false;
    s->resp_len = 0;
    s->resp_pos = 0;
    s->ctrl_in = false;
    s->data_ready = false;
    s->ctrl_active = false;
    s->pending_ints0 = 0;
    s->pending_ints1 = 0;
    s->pending_brdy = 0;
    s->pending_bemp = 0;
    s->pending_attach = false;
    s->attach_signalled = false;

    /* Bulk MIDI bridge state. */
    s->rx_q_head = 0;
    s->rx_q_tail = 0;
    s->rx_q_count = 0;
    s->rx_xfer_len = 0;
    s->rx_xfer_pos = 0;
    s->rx_xfer_pipe = 0;
    s->recv_armed = 0;
    s->midi_status = 0;
    s->midi_ndata = 0;
    s->midi_need = 0;
    s->in_sysex = false;
    s->sysex_len = 0;
    s->tx_len = 0;

    qemu_set_irq(s->irq, 0);
}

static void rza1l_usb_realize(DeviceState *dev, Error **errp)
{
    RzA1lUsbState *s = RZA1L_USB(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rza1l_usb_ops, s,
                          TYPE_RZA1L_USB, RZA1L_USB_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    s->event_bh = qemu_bh_new(rza1l_usb_event, s);

    /*
     * Bridge the bulk MIDI pipes to the host chardev (if one is configured).
     * Incoming raw MIDI bytes are framed and delivered over the bulk IN pipe;
     * the receive handler runs in the main loop, the same context as MMIO.
     */
    USB_DBG("realize: chardev connected=%d",
            qemu_chr_fe_backend_connected(&s->chr));
    qemu_chr_fe_set_handlers(&s->chr, rza1l_usb_chr_can_receive,
                             rza1l_usb_chr_receive, NULL, NULL, s, NULL, true);
}

static const VMStateDescription vmstate_rza1l_usb = {
    .name = TYPE_RZA1L_USB,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(regs, RzA1lUsbState, RZA1L_USB_NUM_REGS),
        VMSTATE_BOOL(attached, RzA1lUsbState),
        VMSTATE_UINT16(line_state, RzA1lUsbState),
        VMSTATE_UINT16(dev_address, RzA1lUsbState),
        VMSTATE_BOOL(configured, RzA1lUsbState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property rza1l_usb_properties[] = {
    DEFINE_PROP_BOOL("midi", RzA1lUsbState, midi, false),
    DEFINE_PROP_CHR("chardev", RzA1lUsbState, chr),
};

static void rza1l_usb_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rza1l_usb_realize;
    dc->vmsd = &vmstate_rza1l_usb;
    device_class_set_props(dc, rza1l_usb_properties);
    device_class_set_legacy_reset(dc, rza1l_usb_reset);
}

static const TypeInfo rza1l_usb_info = {
    .name = TYPE_RZA1L_USB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RzA1lUsbState),
    .class_init = rza1l_usb_class_init,
};

static void rza1l_usb_register_types(void)
{
    type_register_static(&rza1l_usb_info);
}

type_init(rza1l_usb_register_types)
