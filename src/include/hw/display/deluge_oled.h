/*
 * Deluge OLED display (128x48 monochrome, SSD130x-class controller)
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_DELUGE_OLED_H
#define HW_DISPLAY_DELUGE_OLED_H

#include "hw/core/sysbus.h"
#include "ui/console.h"
#include "qom/object.h"

#define TYPE_DELUGE_OLED "deluge-oled"
OBJECT_DECLARE_SIMPLE_TYPE(DelugeOledState, DELUGE_OLED)

#define DELUGE_OLED_WIDTH  128
#define DELUGE_OLED_HEIGHT 48
#define DELUGE_OLED_COLS   128
#define DELUGE_OLED_PAGES  8   /* 64-row controller GDDRAM, 8 pages of 8 rows */

struct DelugeOledState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    QemuConsole *con;

    /*
     * Graphics RAM: one byte per (page, column); bit b of the byte is the
     * pixel at row page*8 + b (LSB = topmost pixel within the page).
     */
    uint8_t gddram[DELUGE_OLED_PAGES][DELUGE_OLED_COLS];

    /* SSD130x addressing / display state parsed from the command stream. */
    uint8_t addr_mode;            /* 0 horizontal, 1 vertical, 2 page         */
    uint8_t col_start, col_end;   /* column address window                    */
    uint8_t page_start, page_end; /* page address window                      */
    uint8_t cur_col, cur_page;    /* auto-incrementing write pointer          */
    bool display_on;
    bool inverted;

    /* In-flight multi-byte command awaiting its parameter bytes. */
    uint8_t pending_cmd;
    uint8_t args_needed;
    uint8_t args_have;
    uint8_t args[2];

    /*
     * Control lines driven by the PIC (not the SPI data stream):
     *   dc_high  — false selects command bytes, true selects pixel data.
     *   selected — chip select; SPI bytes are only ours while asserted.
     *   enabled  — panel power.
     */
    bool dc_high;
    bool selected;
    bool enabled;

    bool dirty;
};

/* Feed one byte from the RSPI data register to the OLED controller. */
void deluge_oled_spi_byte(DelugeOledState *s, uint8_t b);

/* Control lines, driven by the PIC command decoder. */
void deluge_oled_set_dc(DelugeOledState *s, bool dc_high);
void deluge_oled_set_select(DelugeOledState *s, bool selected);
void deluge_oled_set_enable(DelugeOledState *s, bool enabled);

#endif /* HW_DISPLAY_DELUGE_OLED_H */
