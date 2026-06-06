/*
 * Deluge 7-segment numeric display (4 digits)
 *
 * The 4-character numeric display fitted to non-OLED Deluges is driven by the
 * firmware as raw segment bitmasks sent to the PIC (command 224). This device
 * holds those bitmasks and renders them to a QEMU graphic console, drawing each
 * lit segment as a bright bar. It is the headless precursor to the hardware
 * skin.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-properties.h"
#include "ui/console.h"
#include "hw/display/deluge_segment.h"

/* Segment-on / segment-off colours (amber LED look). */
#define DELUGE_SEGMENT_ON  0xffffb000u
#define DELUGE_SEGMENT_OFF 0xff201400u
#define DELUGE_SEGMENT_BG  0xff000000u

/*
 * Geometry of the seven bars (and the decimal point) within a digit cell, keyed
 * by the firmware's bitmask layout: bit0=g bit1=f bit2=e bit3=d bit4=c bit5=b
 * bit6=a bit7=dp. Each entry is {bit, x, y, w, h}.
 */
typedef struct { int bit, x, y, w, h; } SegRect;

#define T  DELUGE_SEGMENT_THICK
#define DW DELUGE_SEGMENT_DW
#define VL DELUGE_SEGMENT_VLEN

static const SegRect deluge_segment_rects[] = {
    { 6, T,        0,            DW - 2 * T, T  }, /* a  top            */
    { 5, DW - T,   T,            T,          VL }, /* b  upper-right    */
    { 4, DW - T,   2 * T + VL,   T,          VL }, /* c  lower-right    */
    { 3, T,        DELUGE_SEGMENT_DH - T, DW - 2 * T, T }, /* d  bottom */
    { 2, 0,        2 * T + VL,   T,          VL }, /* e  lower-left     */
    { 1, 0,        T,            T,          VL }, /* f  upper-left     */
    { 0, T,        T + VL,       DW - 2 * T, T  }, /* g  middle         */
    { 7, DW + 2,   DELUGE_SEGMENT_DH - T, T,  T  }, /* dp decimal point */
};

static void deluge_segment_update_gfx(void *opaque);

void deluge_segment_update(DelugeSegmentState *s,
                           const uint8_t digits[DELUGE_SEGMENT_DIGITS])
{
    memcpy(s->digits, digits, sizeof(s->digits));
    s->dirty = true;
}

static void deluge_segment_invalidate(void *opaque)
{
    DelugeSegmentState *s = opaque;
    s->dirty = true;
}

static void deluge_segment_fill(uint32_t *dst, int stride, int x0, int y0,
                                int w, int h, uint32_t argb)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            dst[(y0 + y) * stride + (x0 + x)] = argb;
        }
    }
}

static void deluge_segment_update_gfx(void *opaque)
{
    DelugeSegmentState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t *dst;
    int stride;

    if (!s->dirty) {
        return;
    }
    s->dirty = false;

    if (surface_bits_per_pixel(surface) != 32) {
        dpy_gfx_update_full(s->con);
        return;
    }

    dst = (uint32_t *)surface_data(surface);
    stride = surface_stride(surface) / 4;

    for (int i = 0; i < DELUGE_SEGMENT_HEIGHT * stride; i++) {
        dst[i] = DELUGE_SEGMENT_BG;
    }

    for (int d = 0; d < DELUGE_SEGMENT_DIGITS; d++) {
        int ox = DELUGE_SEGMENT_MARGIN + d * DELUGE_SEGMENT_PITCH;
        int oy = DELUGE_SEGMENT_MARGIN;
        uint8_t mask = s->digits[d];

        for (unsigned r = 0; r < ARRAY_SIZE(deluge_segment_rects); r++) {
            const SegRect *seg = &deluge_segment_rects[r];
            uint32_t argb = (mask & (1u << seg->bit)) ? DELUGE_SEGMENT_ON
                                                      : DELUGE_SEGMENT_OFF;

            deluge_segment_fill(dst, stride, ox + seg->x, oy + seg->y,
                                seg->w, seg->h, argb);
        }
    }

    dpy_gfx_update(s->con, 0, 0, DELUGE_SEGMENT_WIDTH, DELUGE_SEGMENT_HEIGHT);
}

static const GraphicHwOps deluge_segment_gfx_ops = {
    .invalidate = deluge_segment_invalidate,
    .gfx_update = deluge_segment_update_gfx,
};

static void deluge_segment_reset(DeviceState *dev)
{
    DelugeSegmentState *s = DELUGE_SEGMENT(dev);

    memset(s->digits, 0, sizeof(s->digits));
    s->dirty = true;
}

static void deluge_segment_realize(DeviceState *dev, Error **errp)
{
    DelugeSegmentState *s = DELUGE_SEGMENT(dev);

    s->con = graphic_console_init(dev, 0, &deluge_segment_gfx_ops, s);
    qemu_console_resize(s->con, DELUGE_SEGMENT_WIDTH, DELUGE_SEGMENT_HEIGHT);
}

static const VMStateDescription vmstate_deluge_segment = {
    .name = TYPE_DELUGE_SEGMENT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(digits, DelugeSegmentState, DELUGE_SEGMENT_DIGITS),
        VMSTATE_END_OF_LIST()
    },
};

static void deluge_segment_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = deluge_segment_realize;
    dc->vmsd = &vmstate_deluge_segment;
    device_class_set_legacy_reset(dc, deluge_segment_reset);
    dc->desc = "Synthstrom Deluge 7-segment display";
}

static const TypeInfo deluge_segment_info = {
    .name          = TYPE_DELUGE_SEGMENT,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DelugeSegmentState),
    .class_init    = deluge_segment_class_init,
};

static void deluge_segment_register_types(void)
{
    type_register_static(&deluge_segment_info);
}

type_init(deluge_segment_register_types)
