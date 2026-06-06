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
#include "hw/display/deluge_padgrid.h"
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

static inline uint8_t blend_chan(uint8_t dst, uint8_t src, uint8_t a)
{
    return (uint8_t)(((int)dst * (255 - a) + (int)src * a) / 255);
}

static void deluge_skin_blend_pad(uint32_t *dst, int stride,
                                  int cx, int cy,
                                  uint8_t rr, uint8_t gg, uint8_t bb,
                                  uint8_t alpha)
{
    int half = DELUGE_SKIN_PAD_SIZE / 2;
    int round = DELUGE_SKIN_PAD_ROUND;
    int x0 = cx - half;
    int y0 = cy - half;
    int x1 = cx + half;
    int y1 = cy + half;

    for (int py = y0; py < y1; py++) {
        if (py < 0 || py >= DELUGE_SKIN_IMAGE_HEIGHT) {
            continue;
        }
        for (int px = x0; px < x1; px++) {
            int dx = 0, dy = 0;
            uint32_t p;
            uint8_t pr, pg, pb;
            uint8_t a = alpha;

            if (px < 0 || px >= DELUGE_SKIN_IMAGE_WIDTH) {
                continue;
            }

            /* Rounded corners: fade alpha near corners only. */
            if (px < x0 + round) {
                dx = x0 + round - px;
            } else if (px >= x1 - round) {
                dx = px - (x1 - round - 1);
            }
            if (py < y0 + round) {
                dy = y0 + round - py;
            } else if (py >= y1 - round) {
                dy = py - (y1 - round - 1);
            }
            if (dx || dy) {
                int d2 = dx * dx + dy * dy;
                int r2 = round * round;
                if (d2 > r2) {
                    continue;
                }
                a = (uint8_t)((alpha * (r2 - d2)) / MAX(1, r2));
            }

            p = dst[py * stride + px];
            pr = (p >> 16) & 0xff;
            pg = (p >> 8) & 0xff;
            pb = p & 0xff;

            pr = blend_chan(pr, rr, a);
            pg = blend_chan(pg, gg, a);
            pb = blend_chan(pb, bb, a);

            dst[py * stride + px] = 0xff000000u |
                                     ((uint32_t)pr << 16) |
                                     ((uint32_t)pg << 8) |
                                     (uint32_t)pb;
        }
    }
}

static void deluge_skin_draw_pads(DelugeSkinState *s, uint32_t *dst, int stride)
{
    DelugePadGridState *p = s->padgrid;

    if (!p) {
        return;
    }

    for (int row = 0; row < DELUGE_PADGRID_ROWS; row++) {
        int y = DELUGE_SKIN_PAD_ROWS_Y0 + row * DELUGE_SKIN_PAD_ROWS_DY;

        for (int col = 0; col < 16; col++) {
            int x = DELUGE_SKIN_PAD_MAIN_X0 + col * DELUGE_SKIN_PAD_MAIN_DX;
            uint8_t rr = p->rgb[col][row][0];
            uint8_t gg = p->rgb[col][row][1];
            uint8_t bb = p->rgb[col][row][2];

            if (rr || gg || bb) {
                deluge_skin_blend_pad(dst, stride, x, y, rr, gg, bb, 215);
            }
        }

        for (int side = 0; side < 2; side++) {
            int x = DELUGE_SKIN_PAD_SIDE_X0 + side * DELUGE_SKIN_PAD_SIDE_DX;
            int side_y = DELUGE_SKIN_PAD_SIDE_Y0 + row * DELUGE_SKIN_PAD_SIDE_DY;
            int col = 16 + side;
            uint8_t rr = p->rgb[col][row][0];
            uint8_t gg = p->rgb[col][row][1];
            uint8_t bb = p->rgb[col][row][2];

            if (rr || gg || bb) {
                deluge_skin_blend_pad(dst, stride, x, side_y, rr, gg, bb, 215);
            }
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
    deluge_skin_draw_pads(s, dst, stride);

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

void deluge_skin_set_padgrid(DeviceState *dev, DelugePadGridState *padgrid)
{
    DelugeSkinState *s = DELUGE_SKIN(dev);

    s->padgrid = padgrid;
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
