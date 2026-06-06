/*
 * Deluge RGB pad-grid display (16x8 main grid + 2x8 sidebar, 144 RGB LEDs)
 *
 * The Deluge's playing surface is a grid of RGB-backlit pads. Their colours are
 * computed by the firmware and sent to the PIC, which decodes them into a per-
 * pad RGB array (see deluge_pic.c). This device renders that array to a QEMU
 * graphic console: one filled square per pad in its current colour. It is the
 * headless precursor to the composited hardware skin.
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
#include "hw/display/deluge_padgrid.h"

void deluge_padgrid_update(DelugePadGridState *s,
                           const uint8_t rgb[DELUGE_PADGRID_COLS]
                                            [DELUGE_PADGRID_ROWS][3])
{
    memcpy(s->rgb, rgb, sizeof(s->rgb));
    s->dirty = true;
}

static void deluge_padgrid_invalidate(void *opaque)
{
    DelugePadGridState *s = opaque;
    s->dirty = true;
}

static void deluge_padgrid_fill_cell(uint32_t *dst, int stride, int x0, int y0,
                                     uint32_t argb)
{
    for (int y = 0; y < DELUGE_PADGRID_CELL; y++) {
        for (int x = 0; x < DELUGE_PADGRID_CELL; x++) {
            dst[(y0 + y) * stride + (x0 + x)] = argb;
        }
    }
}

static void deluge_padgrid_update_gfx(void *opaque)
{
    DelugePadGridState *s = opaque;
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

    /* Dark background between the pads. */
    for (int i = 0; i < DELUGE_PADGRID_HEIGHT * stride; i++) {
        dst[i] = 0xff101010u;
    }

    for (int col = 0; col < DELUGE_PADGRID_COLS; col++) {
        for (int row = 0; row < DELUGE_PADGRID_ROWS; row++) {
            int x0 = DELUGE_PADGRID_GAP +
                     col * (DELUGE_PADGRID_CELL + DELUGE_PADGRID_GAP);
            int y0 = DELUGE_PADGRID_GAP +
                     row * (DELUGE_PADGRID_CELL + DELUGE_PADGRID_GAP);
            uint32_t argb = 0xff000000u |
                            ((uint32_t)s->rgb[col][row][0] << 16) |
                            ((uint32_t)s->rgb[col][row][1] << 8) |
                            (uint32_t)s->rgb[col][row][2];

            deluge_padgrid_fill_cell(dst, stride, x0, y0, argb);
        }
    }

    dpy_gfx_update(s->con, 0, 0, DELUGE_PADGRID_WIDTH, DELUGE_PADGRID_HEIGHT);
}

static const GraphicHwOps deluge_padgrid_gfx_ops = {
    .invalidate = deluge_padgrid_invalidate,
    .gfx_update = deluge_padgrid_update_gfx,
};

static void deluge_padgrid_reset(DeviceState *dev)
{
    DelugePadGridState *s = DELUGE_PADGRID(dev);

    memset(s->rgb, 0, sizeof(s->rgb));
    s->dirty = true;
}

static void deluge_padgrid_realize(DeviceState *dev, Error **errp)
{
    DelugePadGridState *s = DELUGE_PADGRID(dev);

    s->con = graphic_console_init(dev, 0, &deluge_padgrid_gfx_ops, s);
    qemu_console_resize(s->con, DELUGE_PADGRID_WIDTH, DELUGE_PADGRID_HEIGHT);
}

static const VMStateDescription vmstate_deluge_padgrid = {
    .name = TYPE_DELUGE_PADGRID,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BUFFER_UNSAFE(rgb, DelugePadGridState, 0,
                              DELUGE_PADGRID_COLS * DELUGE_PADGRID_ROWS * 3),
        VMSTATE_END_OF_LIST()
    },
};

static void deluge_padgrid_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = deluge_padgrid_realize;
    dc->vmsd = &vmstate_deluge_padgrid;
    device_class_set_legacy_reset(dc, deluge_padgrid_reset);
    dc->desc = "Synthstrom Deluge RGB pad grid";
}

static const TypeInfo deluge_padgrid_info = {
    .name          = TYPE_DELUGE_PADGRID,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DelugePadGridState),
    .class_init    = deluge_padgrid_class_init,
};

static void deluge_padgrid_register_types(void)
{
    type_register_static(&deluge_padgrid_info);
}

type_init(deluge_padgrid_register_types)
