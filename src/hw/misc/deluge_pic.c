/*
 * Deluge input PIC coprocessor — serial-link model
 *
 * Models the PIC microcontroller on the far end of SCIF channel 1. Implemented
 * as a QEMU character backend so it plugs into the SCIF1 frontend directly:
 * every byte the firmware's transmit DMA pushes through SCFTDR is handed to
 * deluge_pic_chr_write, and the PIC's replies are delivered straight into the
 * firmware's receive DMA ring (the firmware reads PIC input from picRxBuffer,
 * filled by DMA from SCFRDR, never from the SCIF data register).
 *
 * Scope of this layer: the byte-stream framing and the boot handshake. The
 * firmware-to-PIC stream is a sequence of single-byte command messages, some
 * carrying a fixed number of payload bytes. Two control messages matter for
 * bring-up:
 *
 *   - SET_UART_SPEED (225): one payload byte selecting the link divisor. The
 *     PIC switches to the faster pad-update rate. Timing is not modelled, so
 *     the divisor is simply recorded.
 *   - REQUEST_FIRMWARE_VERSION (245): the PIC replies with FIRMWARE_VERSION_NEXT
 *     (245) followed by a version byte whose low 7 bits are the version and
 *     whose top bit reports whether an OLED is fitted.
 *
 * Full decoding of the remaining commands (LEDs, scrolling, display data) and
 * synthesising pad/button/encoder input events are handled in later layers; an
 * unrecognised command is framed here as a zero-payload message.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "chardev/char.h"
#include "qom/object.h"
#include "hw/dma/rza1l_dmac.h"
#include "hw/misc/deluge_pic.h"

/* Firmware-to-PIC command bytes (see deluge/drivers/pic/pic.h Message). */
#define PIC_MSG_SET_UART_SPEED       225
#define PIC_MSG_REQUEST_FW_VERSION   245

/* PIC-to-firmware response bytes (see pic.h Response). */
#define PIC_RESP_FIRMWARE_VERSION_NEXT 245

/*
 * Reported PIC firmware version. The low 7 bits are the version number; the
 * top bit tells the firmware an OLED is present (picSaysOLEDPresent). It is
 * reported here without the OLED bit so bring-up drives the 7-segment display;
 * OLED reporting is enabled once the OLED panel is modelled.
 */
#define DELUGE_PIC_FW_VERSION   23
#define DELUGE_PIC_OLED_FLAG    0x80
#define DELUGE_PIC_VERSION_BYTE (DELUGE_PIC_FW_VERSION & 0x7f)

/* Deliver one response byte to the firmware via its receive DMA ring. */
static void deluge_pic_respond(DelugePicState *s, uint8_t b)
{
    if (s->dmac) {
        rza1l_dmac_peripheral_rx_push(s->dmac, s->rx_dma_channel, b);
    }
}

static void deluge_pic_send_version(DelugePicState *s)
{
    deluge_pic_respond(s, PIC_RESP_FIRMWARE_VERSION_NEXT);
    deluge_pic_respond(s, DELUGE_PIC_VERSION_BYTE);
}

/* Frame a single incoming byte from the firmware. */
static void deluge_pic_rx_byte(DelugePicState *s, uint8_t b)
{
    if (s->pending_payload > 0) {
        s->pending_payload--;
        if (s->awaiting_baud) {
            s->baud_div = b;
            s->awaiting_baud = false;
        }
        return;
    }

    switch (b) {
    case PIC_MSG_SET_UART_SPEED:
        s->pending_payload = 1;
        s->awaiting_baud = true;
        break;
    case PIC_MSG_REQUEST_FW_VERSION:
        deluge_pic_send_version(s);
        break;
    default:
        /* Higher layer decodes the rest; frame unknowns as zero-payload. */
        break;
    }
}

static int deluge_pic_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    DelugePicState *s = DELUGE_PIC(chr);

    for (int i = 0; i < len; i++) {
        deluge_pic_rx_byte(s, buf[i]);
    }
    return len;
}

void deluge_pic_set_dma(Chardev *chr, struct RzA1lDmacState *dmac,
                        int rx_dma_channel)
{
    DelugePicState *s = DELUGE_PIC(chr);

    s->dmac = dmac;
    s->rx_dma_channel = rx_dma_channel;
    rza1l_dmac_register_rx_ring(dmac, rx_dma_channel);
}

static void deluge_pic_class_init(ObjectClass *oc, const void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->internal = true;
    cc->chr_write = deluge_pic_chr_write;
}

static const TypeInfo deluge_pic_info = {
    .name          = TYPE_DELUGE_PIC,
    .parent        = TYPE_CHARDEV,
    .instance_size = sizeof(DelugePicState),
    .class_init    = deluge_pic_class_init,
};

static void deluge_pic_register_types(void)
{
    type_register_static(&deluge_pic_info);
}

type_init(deluge_pic_register_types)
