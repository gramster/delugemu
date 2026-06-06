/*
 * Deluge composited front-panel skin view
 *
 * Draws a calibrated panel image and overlays live display content. First pass
 * composites the OLED viewport; pad-grid and 7-segment overlays follow in the
 * next task.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "ui/console.h"
#include "qemu/timer.h"
#include "png.h"

#include "hw/display/deluge_skin.h"
#include "hw/display/deluge_oled.h"
#include "hw/display/deluge_skin_layout.h"

#define DELUGE_SKIN_REFRESH_MS 33

static bool deluge_skin_load_png_argb(const char *path, uint32_t *dst)
{
    FILE *fp;
    png_structp png;
    png_infop info;
    png_bytep *rows = NULL;
    int w, h;
    bool ok = false;

    fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }

    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        fclose(fp);
        return false;
    }

    info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        fclose(fp);
        return false;
    }

    if (setjmp(png_jmpbuf(png))) {
        goto out;
    }

    png_init_io(png, fp);
    png_read_info(png, info);

    w = png_get_image_width(png, info);
    h = png_get_image_height(png, info);
    if (w != DELUGE_SKIN_IMAGE_WIDTH || h != DELUGE_SKIN_IMAGE_HEIGHT) {
        goto out;
    }

    if (png_get_bit_depth(png, info) == 16) {
        png_set_strip_16(png);
    }
    if (png_get_color_type(png, info) == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png);
    }
    if (png_get_color_type(png, info) == PNG_COLOR_TYPE_GRAY &&
        png_get_bit_depth(png, info) < 8) {
        png_set_expand_gray_1_2_4_to_8(png);
    }
    if (png_get_valid(png, info, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png);
    }
    if (png_get_color_type(png, info) == PNG_COLOR_TYPE_GRAY ||
        png_get_color_type(png, info) == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    if (!(png_get_color_type(png, info) & PNG_COLOR_MASK_ALPHA)) {
        png_set_add_alpha(png, 0xff, PNG_FILLER_AFTER);
    }

    png_read_update_info(png, info);

    rows = g_new0(png_bytep, h);
    for (int y = 0; y < h; y++) {
        rows[y] = g_malloc(png_get_rowbytes(png, info));
    }

    png_read_image(png, rows);

    for (int y = 0; y < h; y++) {
        uint8_t *row = rows[y];
        for (int x = 0; x < w; x++) {
            uint8_t r = row[x * 4 + 0];
            uint8_t g = row[x * 4 + 1];
            uint8_t b = row[x * 4 + 2];
            uint8_t a = row[x * 4 + 3];
            dst[y * w + x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                             ((uint32_t)g << 8) | (uint32_t)b;
        }
    }

    ok = true;

out:
    if (rows) {
        for (int y = 0; y < DELUGE_SKIN_IMAGE_HEIGHT; y++) {
            g_free(rows[y]);
        }
        g_free(rows);
    }
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    return ok;
}

static void deluge_skin_draw_oled(DelugeSkinState *s, uint32_t *dst, int stride)
{
    DelugeOledState *o = s->oled;

    if (!o) {
        return;
    }

    for (int y = 0; y < DELUGE_SKIN_OLED_H; y++) {
        int sy = (y * DELUGE_OLED_HEIGHT) / DELUGE_SKIN_OLED_H;
        int page = o->page_start + (sy >> 3);
        int bit = sy & 7;

        for (int x = 0; x < DELUGE_SKIN_OLED_W; x++) {
            int sx = (x * DELUGE_OLED_WIDTH) / DELUGE_SKIN_OLED_W;
            bool on = false;
            uint32_t argb;

            if (o->display_on && o->enabled && page < DELUGE_OLED_PAGES) {
                on = (o->gddram[page][sx] >> bit) & 1;
            }
            if (o->inverted) {
                on = !on;
            }

            argb = on ? 0xffd8ffd8u : 0xff000000u;
            dst[(DELUGE_SKIN_OLED_Y + y) * stride + (DELUGE_SKIN_OLED_X + x)] =
                argb;
        }
    }
}

static void deluge_skin_render(DelugeSkinState *s)
{
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t *dst;
    int stride;

    if (surface_bits_per_pixel(surface) != 32) {
        dpy_gfx_update_full(s->con);
        return;
    }

    dst = (uint32_t *)surface_data(surface);
    stride = surface_stride(surface) / 4;

    if (s->bg_loaded) {
        for (int y = 0; y < DELUGE_SKIN_IMAGE_HEIGHT; y++) {
            memcpy(&dst[y * stride],
                   &s->bg_argb[y * DELUGE_SKIN_IMAGE_WIDTH],
                   DELUGE_SKIN_IMAGE_WIDTH * sizeof(uint32_t));
        }
    } else {
        for (int i = 0; i < DELUGE_SKIN_IMAGE_HEIGHT * stride; i++) {
            dst[i] = 0xff101018u;
        }
    }

    deluge_skin_draw_oled(s, dst, stride);

    dpy_gfx_update(s->con, 0, 0,
                   DELUGE_SKIN_IMAGE_WIDTH,
                   DELUGE_SKIN_IMAGE_HEIGHT);
}

static void deluge_skin_refresh(void *opaque)
{
    DelugeSkinState *s = opaque;

    deluge_skin_render(s);
    timer_mod(s->refresh_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + DELUGE_SKIN_REFRESH_MS);
}

static void deluge_skin_invalidate(void *opaque)
{
    DelugeSkinState *s = opaque;

    s->dirty = true;
}

static void deluge_skin_update(void *opaque)
{
    DelugeSkinState *s = opaque;

    if (s->dirty) {
        s->dirty = false;
        deluge_skin_render(s);
    }
}

static const GraphicHwOps deluge_skin_gfx_ops = {
    .invalidate = deluge_skin_invalidate,
    .gfx_update = deluge_skin_update,
};

void deluge_skin_set_oled(DeviceState *dev, DelugeOledState *oled)
{
    DelugeSkinState *s = DELUGE_SKIN(dev);

    s->oled = oled;
    s->dirty = true;
}

static void deluge_skin_reset(DeviceState *dev)
{
    DelugeSkinState *s = DELUGE_SKIN(dev);

    s->dirty = true;
}

static void deluge_skin_realize(DeviceState *dev, Error **errp)
{
    DelugeSkinState *s = DELUGE_SKIN(dev);
    const char *path = s->image_path ? s->image_path : "Synthstrom_Deluge_Skin.png";

    s->bg_argb = g_new0(uint32_t, DELUGE_SKIN_IMAGE_WIDTH * DELUGE_SKIN_IMAGE_HEIGHT);
    s->bg_loaded = deluge_skin_load_png_argb(path, s->bg_argb);

    s->con = graphic_console_init(dev, 0, &deluge_skin_gfx_ops, s);
    qemu_console_resize(s->con, DELUGE_SKIN_IMAGE_WIDTH, DELUGE_SKIN_IMAGE_HEIGHT);

    s->refresh_timer = timer_new_ms(QEMU_CLOCK_REALTIME, deluge_skin_refresh, s);
    timer_mod(s->refresh_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + DELUGE_SKIN_REFRESH_MS);
}

static void deluge_skin_unrealize(DeviceState *dev)
{
    DelugeSkinState *s = DELUGE_SKIN(dev);

    if (s->refresh_timer) {
        timer_del(s->refresh_timer);
        timer_free(s->refresh_timer);
        s->refresh_timer = NULL;
    }

    g_free(s->bg_argb);
    s->bg_argb = NULL;
}

static const Property deluge_skin_props[] = {
    DEFINE_PROP_STRING("image", DelugeSkinState, image_path),
};

static void deluge_skin_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = deluge_skin_realize;
    dc->unrealize = deluge_skin_unrealize;
    device_class_set_legacy_reset(dc, deluge_skin_reset);
    device_class_set_props(dc, deluge_skin_props);
    dc->desc = "Synthstrom Deluge composited skin";
}

static const TypeInfo deluge_skin_info = {
    .name          = TYPE_DELUGE_SKIN,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DelugeSkinState),
    .class_init    = deluge_skin_class_init,
};

static void deluge_skin_register_types(void)
{
    type_register_static(&deluge_skin_info);
}

type_init(deluge_skin_register_types)
