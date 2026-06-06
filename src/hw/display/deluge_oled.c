/*
 * Deluge OLED display (128x48 monochrome) — stub
 *
 * Registers a graphical console of the correct dimensions and clears it. The
 * controller command set and pixel updates are implemented in M3.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "ui/console.h"
#include "hw/display/deluge_oled.h"

static void deluge_oled_invalidate(void *opaque)
{
    DelugeOledState *s = opaque;
    s->dirty = true;
}

static void deluge_oled_update(void *opaque)
{
    DelugeOledState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);

    if (!s->dirty) {
        return;
    }
    s->dirty = false;

    /*
     * TODO(M3): blit s->framebuffer into the surface. For now the surface is
     * left as allocated (blank) so the window simply appears at the right size.
     */
    dpy_gfx_update(s->con, 0, 0, surface_width(surface),
                   surface_height(surface));
}

static const GraphicHwOps deluge_oled_gfx_ops = {
    .invalidate = deluge_oled_invalidate,
    .gfx_update = deluge_oled_update,
};

static void deluge_oled_realize(DeviceState *dev, Error **errp)
{
    DelugeOledState *s = DELUGE_OLED(dev);

    memset(s->framebuffer, 0, sizeof(s->framebuffer));
    s->dirty = true;

    s->con = graphic_console_init(dev, 0, &deluge_oled_gfx_ops, s);
    qemu_console_resize(s->con, DELUGE_OLED_WIDTH, DELUGE_OLED_HEIGHT);
}

static void deluge_oled_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = deluge_oled_realize;
    dc->desc = "Synthstrom Deluge OLED display";
}

static const TypeInfo deluge_oled_info = {
    .name          = TYPE_DELUGE_OLED,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DelugeOledState),
    .class_init    = deluge_oled_class_init,
};

static void deluge_oled_register_types(void)
{
    type_register_static(&deluge_oled_info);
}

type_init(deluge_oled_register_types)
