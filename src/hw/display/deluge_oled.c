/*
 * Deluge OLED display (128x48 monochrome, SSD130x-class controller)
 *
 * Models the OLED panel the firmware drives over RSPI0. Pixel/command bytes
 * arrive at deluge_oled_spi_byte() from the RSPI data register (the firmware
 * streams the 768-byte framebuffer via DMA and writes init commands by CPU).
 * The PIC drives the data/command, chip-select and power lines via the
 * deluge_oled_set_* helpers. A graphical console of the panel's dimensions
 * shows the decoded framebuffer.
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
#include "hw/display/deluge_oled.h"

/* Addressing modes (SSD130x command 0x20). */
#define OLED_ADDR_HORIZONTAL 0
#define OLED_ADDR_VERTICAL   1
#define OLED_ADDR_PAGE       2

static void deluge_oled_write_data(DelugeOledState *s, uint8_t b)
{
    if (s->cur_page < DELUGE_OLED_PAGES && s->cur_col < DELUGE_OLED_COLS) {
        s->gddram[s->cur_page][s->cur_col] = b;
        s->dirty = true;
    }

    switch (s->addr_mode) {
    case OLED_ADDR_VERTICAL:
        if (s->cur_page >= s->page_end) {
            s->cur_page = s->page_start;
            s->cur_col = (s->cur_col >= s->col_end) ? s->col_start
                                                    : s->cur_col + 1;
        } else {
            s->cur_page++;
        }
        break;
    case OLED_ADDR_PAGE:
        s->cur_col = (s->cur_col >= s->col_end) ? s->col_start : s->cur_col + 1;
        break;
    case OLED_ADDR_HORIZONTAL:
    default:
        if (s->cur_col >= s->col_end) {
            s->cur_col = s->col_start;
            s->cur_page = (s->cur_page >= s->page_end) ? s->page_start
                                                       : s->cur_page + 1;
        } else {
            s->cur_col++;
        }
        break;
    }
}

static void deluge_oled_run_command(DelugeOledState *s)
{
    switch (s->pending_cmd) {
    case 0x20: /* memory addressing mode */
        s->addr_mode = s->args[0] & 0x03;
        break;
    case 0x21: /* column address window */
        s->col_start = s->args[0] & 0x7f;
        s->col_end = s->args[1] & 0x7f;
        s->cur_col = s->col_start;
        break;
    case 0x22: /* page address window */
        s->page_start = s->args[0] & 0x07;
        s->page_end = s->args[1] & 0x07;
        s->cur_page = s->page_start;
        break;
    default:
        /* contrast (0x81), mux (0xA8), clock (0xD5), etc.: no visible state */
        break;
    }
}

static void deluge_oled_write_command(DelugeOledState *s, uint8_t b)
{
    if (s->args_needed) {
        s->args[s->args_have++] = b;
        if (s->args_have >= s->args_needed) {
            deluge_oled_run_command(s);
            s->args_needed = 0;
        }
        return;
    }

    if (b <= 0x0f) { /* set lower column nibble (page addressing) */
        s->cur_col = (s->cur_col & 0xf0) | b;
        return;
    }
    if (b >= 0x10 && b <= 0x1f) { /* set upper column nibble */
        s->cur_col = (s->cur_col & 0x0f) | ((b & 0x0f) << 4);
        return;
    }
    if (b >= 0xb0 && b <= 0xb7) { /* set page (page addressing) */
        s->cur_page = b & 0x07;
        return;
    }
    if (b >= 0x40 && b <= 0x7f) { /* set display start line */
        return;
    }

    switch (b) {
    case 0x20: case 0x81: case 0xa8: case 0xd3: case 0xda:
    case 0xd5: case 0xd9: case 0xdb: case 0xfd:
        s->pending_cmd = b;
        s->args_needed = 1;
        s->args_have = 0;
        break;
    case 0x21: case 0x22:
        s->pending_cmd = b;
        s->args_needed = 2;
        s->args_have = 0;
        break;
    case 0xa6: s->inverted = false; s->dirty = true; break;
    case 0xa7: s->inverted = true; s->dirty = true; break;
    case 0xae: s->display_on = false; s->dirty = true; break;
    case 0xaf: s->display_on = true; s->dirty = true; break;
    default:
        /* segment remap (0xA0/A1), COM scan (0xC0/C8), entire-on (0xA4/A5) */
        break;
    }
}

void deluge_oled_spi_byte(DelugeOledState *s, uint8_t b)
{
    /* While de-selected the SPI bus carries CV/gate data, not ours. */
    if (!s->selected) {
        return;
    }
    if (s->dc_high) {
        deluge_oled_write_data(s, b);
    } else {
        deluge_oled_write_command(s, b);
    }
}

void deluge_oled_set_dc(DelugeOledState *s, bool dc_high)
{
    s->dc_high = dc_high;
}

void deluge_oled_set_select(DelugeOledState *s, bool selected)
{
    s->selected = selected;
}

void deluge_oled_set_enable(DelugeOledState *s, bool enabled)
{
    s->enabled = enabled;
    s->dirty = true;
}

static void deluge_oled_invalidate(void *opaque)
{
    DelugeOledState *s = opaque;
    s->dirty = true;
}

static void deluge_oled_update(void *opaque)
{
    DelugeOledState *s = opaque;
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

    for (int y = 0; y < DELUGE_OLED_HEIGHT; y++) {
        int page = s->page_start + (y >> 3);
        int bit = y & 7;

        for (int x = 0; x < DELUGE_OLED_WIDTH; x++) {
            bool on = false;

            if (s->display_on && s->enabled && page < DELUGE_OLED_PAGES) {
                on = (s->gddram[page][x] >> bit) & 1;
            }
            if (s->inverted) {
                on = !on;
            }
            dst[y * stride + x] = on ? 0xffffffffu : 0xff000000u;
        }
    }

    dpy_gfx_update(s->con, 0, 0, DELUGE_OLED_WIDTH, DELUGE_OLED_HEIGHT);
}

static const GraphicHwOps deluge_oled_gfx_ops = {
    .invalidate = deluge_oled_invalidate,
    .gfx_update = deluge_oled_update,
};

static void deluge_oled_reset(DeviceState *dev)
{
    DelugeOledState *s = DELUGE_OLED(dev);

    memset(s->gddram, 0, sizeof(s->gddram));
    s->addr_mode = OLED_ADDR_HORIZONTAL;
    s->col_start = 0;
    s->col_end = DELUGE_OLED_COLS - 1;
    s->page_start = 0;
    s->page_end = DELUGE_OLED_PAGES - 1;
    s->cur_col = 0;
    s->cur_page = 0;
    s->display_on = false;
    s->inverted = false;
    s->args_needed = 0;
    s->args_have = 0;
    s->dc_high = false;
    s->selected = false;
    s->enabled = false;
    s->dirty = true;
}

static void deluge_oled_realize(DeviceState *dev, Error **errp)
{
    DelugeOledState *s = DELUGE_OLED(dev);

    s->con = graphic_console_init(dev, 0, &deluge_oled_gfx_ops, s);
    qemu_console_resize(s->con, DELUGE_OLED_WIDTH, DELUGE_OLED_HEIGHT);
}

static const VMStateDescription vmstate_deluge_oled = {
    .name = TYPE_DELUGE_OLED,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_2DARRAY(gddram, DelugeOledState,
                              DELUGE_OLED_PAGES, DELUGE_OLED_COLS),
        VMSTATE_UINT8(addr_mode, DelugeOledState),
        VMSTATE_UINT8(col_start, DelugeOledState),
        VMSTATE_UINT8(col_end, DelugeOledState),
        VMSTATE_UINT8(page_start, DelugeOledState),
        VMSTATE_UINT8(page_end, DelugeOledState),
        VMSTATE_UINT8(cur_col, DelugeOledState),
        VMSTATE_UINT8(cur_page, DelugeOledState),
        VMSTATE_BOOL(display_on, DelugeOledState),
        VMSTATE_BOOL(inverted, DelugeOledState),
        VMSTATE_UINT8(pending_cmd, DelugeOledState),
        VMSTATE_UINT8(args_needed, DelugeOledState),
        VMSTATE_UINT8(args_have, DelugeOledState),
        VMSTATE_UINT8_ARRAY(args, DelugeOledState, 2),
        VMSTATE_BOOL(dc_high, DelugeOledState),
        VMSTATE_BOOL(selected, DelugeOledState),
        VMSTATE_BOOL(enabled, DelugeOledState),
        VMSTATE_END_OF_LIST()
    },
};

static void deluge_oled_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = deluge_oled_realize;
    dc->vmsd = &vmstate_deluge_oled;
    device_class_set_legacy_reset(dc, deluge_oled_reset);
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
