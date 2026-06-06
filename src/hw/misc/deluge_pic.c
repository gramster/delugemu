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
 * synthesising pad/button input events are handled in later layers; an
 * unrecognised command is framed here as a zero-payload message.
 *
 * Inbound events: this layer also synthesises the PIC-to-firmware input stream.
 * Pad and button presses/releases are encoded with the same matrix indices and
 * NEXT_PAD_OFF framing the hardware uses, and an idle heartbeat reports "no
 * presses happening" whenever nothing is held. The host-input mapping that
 * drives these is a separate layer.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "chardev/char.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "hw/dma/rza1l_dmac.h"
#include "hw/display/deluge_oled.h"
#include "hw/display/deluge_padgrid.h"
#include "hw/display/deluge_segment.h"
#include "hw/misc/deluge_pic.h"

/*
 * Firmware-to-PIC command bytes (see firmware deluge/drivers/pic/pic.h
 * PIC::Message). Several commands occupy contiguous ranges where the low part
 * of the byte encodes an index added to the base value.
 */
#define PIC_MSG_SET_COLOUR_BASE      1    /* 1..9: column-pair RGB block      */
#define PIC_MSG_SET_COLOUR_LAST      9
#define PIC_MSG_SET_DEBOUNCE_TIME    18
#define PIC_MSG_SET_REFRESH_TIME     19
#define PIC_MSG_SET_GOLD_KNOB0       20
#define PIC_MSG_SET_GOLD_KNOB1       21
#define PIC_MSG_SET_FLASH_LENGTH     23
#define PIC_MSG_SET_LED_OFF_BASE     152  /* 152..179: indicator LED off      */
#define PIC_MSG_SET_LED_ON_BASE      188  /* 188..215: indicator LED on       */
#define PIC_MSG_UPDATE_SEVEN_SEGMENT 224
#define PIC_MSG_SET_UART_SPEED       225
#define PIC_MSG_SET_SCROLL_ROW_BASE  228  /* 228..235: scroll-row + 1 RGB     */
#define PIC_MSG_SET_SCROLL_ROW_LAST  235
#define PIC_MSG_SET_SCROLL_UP        241
#define PIC_MSG_SET_SCROLL_DOWN      242
#define PIC_MSG_SET_DIMMER_INTERVAL  243
#define PIC_MSG_SET_MIN_INT_INTERVAL 244
#define PIC_MSG_REQUEST_FW_VERSION   245
#define PIC_MSG_ENABLE_OLED          247
#define PIC_MSG_SELECT_OLED          248
#define PIC_MSG_DESELECT_OLED        249
#define PIC_MSG_SET_DC_LOW           250
#define PIC_MSG_SET_DC_HIGH          251

/* PIC-to-firmware response bytes (see pic.h Response). */
#define PIC_RESP_FIRMWARE_VERSION_NEXT 245
#define PIC_RESP_NEXT_PAD_OFF          252  /* prefixes a release event       */
#define PIC_RESP_NO_PRESSES_HAPPENING  254  /* idle "scan found nothing" beat */

/*
 * Idle-heartbeat period. The hardware PIC reports "no presses happening" at the
 * end of every matrix scan that finds nothing pressed; the firmware uses it to
 * reconcile any state it thinks is held. The exact rate is not observable to
 * the firmware, so a steady 100 ms beat (matching the firmware's READ_INPUTS
 * cadence) is plenty without flooding the receive ring.
 */
#define DELUGE_PIC_HEARTBEAT_MS 100

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

/*
 * Idle heartbeat: while nothing is held, report "no presses happening" so the
 * firmware keeps its view of the matrix in sync. Suppressed during a hold,
 * because that message would make the firmware release the held pad/button.
 */
static void deluge_pic_heartbeat(void *opaque)
{
    DelugePicState *s = opaque;

    if (s->held_count == 0) {
        deluge_pic_respond(s, PIC_RESP_NO_PRESSES_HAPPENING);
    }
    timer_mod(s->heartbeat,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + DELUGE_PIC_HEARTBEAT_MS);
}

/*
 * Report a matrix event to the firmware: NEXT_PAD_OFF first for a release, then
 * the index byte. The firmware distinguishes pads (index < 144) from buttons by
 * range, so callers pass an already-encoded index.
 */
static void deluge_pic_send_event(DelugePicState *s, uint8_t index, bool pressed)
{
    if (!pressed) {
        deluge_pic_respond(s, PIC_RESP_NEXT_PAD_OFF);
    }
    deluge_pic_respond(s, index);

    if (pressed) {
        s->held_count++;
    } else if (s->held_count > 0) {
        s->held_count--;
    }
}

/*
 * Number of payload bytes that follow a command byte. The PIC firmware has no
 * length field; both ends agree on a fixed payload size per command, so the
 * model must reproduce the same sizes to stay framed. Commands not listed here
 * (single-byte controls and the index-encoded pad-flash / LED / scroll-flag
 * ranges) carry no payload.
 */
static unsigned deluge_pic_payload_len(uint8_t cmd)
{
    if (cmd >= PIC_MSG_SET_COLOUR_BASE && cmd <= PIC_MSG_SET_COLOUR_LAST) {
        /* Two columns of kDisplayHeight pads, RGB each. */
        return DELUGE_PIC_GRID_ROWS * 2 * 3;
    }
    if (cmd >= PIC_MSG_SET_SCROLL_ROW_BASE &&
        cmd <= PIC_MSG_SET_SCROLL_ROW_LAST) {
        return 3; /* one RGB colour */
    }
    switch (cmd) {
    case PIC_MSG_SET_DEBOUNCE_TIME:
    case PIC_MSG_SET_REFRESH_TIME:
    case PIC_MSG_SET_FLASH_LENGTH:
    case PIC_MSG_SET_UART_SPEED:
    case PIC_MSG_SET_DIMMER_INTERVAL:
    case PIC_MSG_SET_MIN_INT_INTERVAL:
        return 1;
    case PIC_MSG_SET_GOLD_KNOB0:
    case PIC_MSG_SET_GOLD_KNOB1:
        return DELUGE_PIC_GOLD_LEDS;
    case PIC_MSG_UPDATE_SEVEN_SEGMENT:
        return DELUGE_PIC_SEG_DIGITS;
    case PIC_MSG_SET_SCROLL_UP:
    case PIC_MSG_SET_SCROLL_DOWN:
        return (DELUGE_PIC_GRID_COLS) * 3; /* (width + sidebar) RGB colours */
    default:
        return 0;
    }
}

/* Act on a fully-assembled command (its payload, if any, is in s->payload). */
static void deluge_pic_dispatch(DelugePicState *s)
{
    uint8_t cmd = s->cmd;

    if (cmd >= PIC_MSG_SET_COLOUR_BASE && cmd <= PIC_MSG_SET_COLOUR_LAST) {
        /*
         * Column-pair RGB block: 2*kDisplayHeight colours filling the two
         * columns 2*idx and 2*idx+1, eight rows each (column-major).
         */
        unsigned idx = cmd - PIC_MSG_SET_COLOUR_BASE;
        unsigned col0 = idx * 2;

        for (unsigned i = 0; i < DELUGE_PIC_GRID_ROWS * 2; i++) {
            unsigned col = col0 + (i / DELUGE_PIC_GRID_ROWS);
            unsigned row = i % DELUGE_PIC_GRID_ROWS;

            if (col < DELUGE_PIC_GRID_COLS) {
                memcpy(s->pad_grid[col][row], &s->payload[i * 3], 3);
            }
        }
        if (s->padgrid) {
            deluge_padgrid_update(s->padgrid, s->pad_grid);
        }
        return;
    }
    if (cmd >= PIC_MSG_SET_LED_OFF_BASE &&
        cmd < PIC_MSG_SET_LED_OFF_BASE + DELUGE_PIC_NUM_LEDS) {
        s->led_on[cmd - PIC_MSG_SET_LED_OFF_BASE] = false;
        return;
    }
    if (cmd >= PIC_MSG_SET_LED_ON_BASE &&
        cmd < PIC_MSG_SET_LED_ON_BASE + DELUGE_PIC_NUM_LEDS) {
        s->led_on[cmd - PIC_MSG_SET_LED_ON_BASE] = true;
        return;
    }

    switch (cmd) {
    case PIC_MSG_SET_UART_SPEED:
        s->baud_div = s->payload[0];
        break;
    case PIC_MSG_SET_GOLD_KNOB0:
        memcpy(s->gold_knob[0], s->payload, DELUGE_PIC_GOLD_LEDS);
        break;
    case PIC_MSG_SET_GOLD_KNOB1:
        memcpy(s->gold_knob[1], s->payload, DELUGE_PIC_GOLD_LEDS);
        break;
    case PIC_MSG_UPDATE_SEVEN_SEGMENT:
        memcpy(s->seven_seg, s->payload, DELUGE_PIC_SEG_DIGITS);
        if (s->segment) {
            deluge_segment_update(s->segment, s->seven_seg);
        }
        break;
    case PIC_MSG_ENABLE_OLED:
        s->oled_enabled = true;
        if (s->oled) {
            deluge_oled_set_enable(s->oled, true);
        }
        break;
    case PIC_MSG_SELECT_OLED:
        s->oled_selected = true;
        if (s->oled) {
            deluge_oled_set_select(s->oled, true);
        }
        /*
         * The firmware's OLED low-level driver sends SELECT/DESELECT and then
         * blocks until the PIC echoes the same byte back before starting (or
         * tearing down) the framebuffer DMA. Echo it so the transfer proceeds.
         */
        deluge_pic_respond(s, PIC_MSG_SELECT_OLED);
        break;
    case PIC_MSG_DESELECT_OLED:
        s->oled_selected = false;
        if (s->oled) {
            deluge_oled_set_select(s->oled, false);
        }
        deluge_pic_respond(s, PIC_MSG_DESELECT_OLED);
        break;
    case PIC_MSG_SET_DC_LOW:
        s->oled_dc_high = false;
        if (s->oled) {
            deluge_oled_set_dc(s->oled, false);
        }
        break;
    case PIC_MSG_SET_DC_HIGH:
        s->oled_dc_high = true;
        if (s->oled) {
            deluge_oled_set_dc(s->oled, true);
        }
        break;
    case PIC_MSG_REQUEST_FW_VERSION:
        deluge_pic_send_version(s);
        break;
    default:
        /*
         * Remaining commands (flash colour/pad, debounce/refresh/flash timing,
         * dimmer, scroll rows/flags, resend-button-states, done-sending-rows)
         * affect PIC-internal behaviour with no state the renderers need yet;
         * their payloads are framed and consumed above.
         */
        break;
    }
}

/* Frame a single incoming byte from the firmware. */
static void deluge_pic_rx_byte(DelugePicState *s, uint8_t b)
{
    if (s->in_message) {
        s->payload[s->payload_have++] = b;
        if (s->payload_have >= s->payload_needed) {
            s->in_message = false;
            deluge_pic_dispatch(s);
        }
        return;
    }

    s->cmd = b;
    s->payload_needed = deluge_pic_payload_len(b);
    if (s->payload_needed == 0) {
        deluge_pic_dispatch(s);
    } else {
        s->in_message = true;
        s->payload_have = 0;
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

/*
 * Migration state. The PIC carries no MMIO of its own; what matters for a
 * snapshot round-trip is the command-framing position and the decoded display
 * state the renderers mirror. Device pointers, the DMA channel and the
 * heartbeat timer are transient and re-established by deluge_pic_set_dma().
 */
static const VMStateDescription vmstate_deluge_pic = {
    .name = TYPE_DELUGE_PIC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(cmd, DelugePicState),
        VMSTATE_BOOL(in_message, DelugePicState),
        VMSTATE_UINT32(payload_have, DelugePicState),
        VMSTATE_UINT32(payload_needed, DelugePicState),
        VMSTATE_UINT8_ARRAY(payload, DelugePicState, DELUGE_PIC_MAX_PAYLOAD),
        VMSTATE_UINT8(baud_div, DelugePicState),
        VMSTATE_UINT32(held_count, DelugePicState),
        VMSTATE_BUFFER_UNSAFE(pad_grid, DelugePicState, 0,
                              DELUGE_PIC_GRID_COLS * DELUGE_PIC_GRID_ROWS * 3),
        VMSTATE_BOOL_ARRAY(led_on, DelugePicState, DELUGE_PIC_NUM_LEDS),
        VMSTATE_UINT8_ARRAY(seven_seg, DelugePicState, DELUGE_PIC_SEG_DIGITS),
        VMSTATE_UINT8_2DARRAY(gold_knob, DelugePicState,
                              DELUGE_PIC_GOLD_KNOBS, DELUGE_PIC_GOLD_LEDS),
        VMSTATE_BOOL(oled_enabled, DelugePicState),
        VMSTATE_BOOL(oled_selected, DelugePicState),
        VMSTATE_BOOL(oled_dc_high, DelugePicState),
        VMSTATE_END_OF_LIST()
    },
};

void deluge_pic_set_dma(Chardev *chr, struct RzA1lDmacState *dmac,
                        int rx_dma_channel)
{
    DelugePicState *s = DELUGE_PIC(chr);

    s->dmac = dmac;
    s->rx_dma_channel = rx_dma_channel;
    rza1l_dmac_register_rx_ring(dmac, rx_dma_channel);

    /*
     * Start the idle heartbeat now that the receive path exists. The PIC owns
     * its own scan cadence, so a free-running virtual-clock timer models it
     * independently of anything the firmware does.
     */
    if (!s->heartbeat) {
        s->heartbeat = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                    deluge_pic_heartbeat, s);
    }
    timer_mod(s->heartbeat,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + DELUGE_PIC_HEARTBEAT_MS);

    /*
     * Register migration state once the instance is wired up. The PIC is a
     * Chardev, not a qdev, so it has no DeviceClass.vmsd and must register
     * itself manually. Only the decoded protocol/display state is saved; the
     * board re-establishes the device pointers and re-arms the heartbeat on
     * load via this same setup path.
     */
    vmstate_register(NULL, VMSTATE_INSTANCE_ID_ANY, &vmstate_deluge_pic, s);
}

void deluge_pic_pad_event(Chardev *chr, int x, int y, bool pressed)
{
    DelugePicState *s = DELUGE_PIC(chr);
    uint8_t index;

    if (x < 0 || x >= DELUGE_PIC_GRID_COLS ||
        y < 0 || y >= DELUGE_PIC_GRID_ROWS) {
        return;
    }

    /*
     * Pad index encoding (firmware hid/matrix/pad.cpp Pad::toChar): 9 * y plus
     * the column-pair number x/2, with the odd column of each pair living in
     * the upper half of the index space (+9 * kDisplayHeight).
     */
    index = 9 * y + (x >> 1);
    if (x & 1) {
        index += 9 * DELUGE_PIC_GRID_ROWS;
    }

    deluge_pic_send_event(s, index, pressed);
}

void deluge_pic_button_event(Chardev *chr, int x, int y, bool pressed)
{
    DelugePicState *s = DELUGE_PIC(chr);
    uint8_t index;

    if (x < 0 || x >= DELUGE_PIC_BTN_COLS ||
        y < 0 || y >= DELUGE_PIC_BTN_ROWS) {
        return;
    }

    /*
     * Button index encoding (firmware hid/button.h button::fromXY): the button
     * matrix sits above the pad index space at 9 * (y + kDisplayHeight * 2) + x,
     * i.e. indices 144..179.
     */
    index = 9 * (y + DELUGE_PIC_GRID_ROWS * 2) + x;

    deluge_pic_send_event(s, index, pressed);
}

void deluge_pic_set_oled(Chardev *chr, struct DelugeOledState *oled)
{
    DelugePicState *s = DELUGE_PIC(chr);

    s->oled = oled;
}

void deluge_pic_set_padgrid(Chardev *chr, struct DelugePadGridState *padgrid)
{
    DelugePicState *s = DELUGE_PIC(chr);

    s->padgrid = padgrid;
}

void deluge_pic_set_segment(Chardev *chr, struct DelugeSegmentState *segment)
{
    DelugePicState *s = DELUGE_PIC(chr);

    s->segment = segment;
}

static void deluge_pic_class_init(ObjectClass *oc, const void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->internal = true;
    cc->chr_write = deluge_pic_chr_write;
}

static void deluge_pic_finalize(Object *obj)
{
    DelugePicState *s = DELUGE_PIC(obj);

    if (s->heartbeat) {
        timer_free(s->heartbeat);
        s->heartbeat = NULL;
    }
}

static const TypeInfo deluge_pic_info = {
    .name            = TYPE_DELUGE_PIC,
    .parent          = TYPE_CHARDEV,
    .instance_size   = sizeof(DelugePicState),
    .instance_finalize = deluge_pic_finalize,
    .class_init      = deluge_pic_class_init,
};

static void deluge_pic_register_types(void)
{
    type_register_static(&deluge_pic_info);
}

type_init(deluge_pic_register_types)
