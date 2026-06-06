/*
 * Deluge input PIC coprocessor — serial-link model
 *
 * The Deluge offloads pad/button/encoder scanning and RGB LED / 7-segment /
 * OLED driving to a dedicated PIC microcontroller that talks to the main SoC
 * over a UART (SCIF channel 1). This models the PIC end of that link: it is a
 * QEMU character backend wired to the SCIF1 frontend, so every byte the
 * firmware transmits to the PIC arrives at deluge_pic's chr_write, and replies
 * are delivered straight into the firmware's receive DMA ring.
 *
 * This file implements the link + framing layer, the boot handshake (baud-rate
 * switch and firmware-version query), and decoding of the outbound command
 * stream into the LED / 7-segment / pad-grid / OLED-control state that the
 * display renderers consume. Synthesising input events is layered on top
 * separately.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_DELUGE_PIC_H
#define HW_MISC_DELUGE_PIC_H

#include "chardev/char.h"
#include "qom/object.h"

struct RzA1lDmacState;
struct DelugeOledState;
struct DelugePadGridState;
struct DelugeSegmentState;

#define TYPE_DELUGE_PIC "chardev-deluge-pic"
typedef struct DelugePicState DelugePicState;
DECLARE_INSTANCE_CHECKER(DelugePicState, DELUGE_PIC, TYPE_DELUGE_PIC)

/*
 * Display geometry, mirrored from the firmware (definitions_cxx.hpp):
 * kDisplayWidth 16, kSideBarWidth 2, kDisplayHeight 8, kNumericDisplayLength 4,
 * kNumGoldKnobIndicatorLEDs 4. The pad grid is (width + sidebar) columns wide.
 */
#define DELUGE_PIC_GRID_COLS  (16 + 2)
#define DELUGE_PIC_GRID_ROWS  8
#define DELUGE_PIC_NUM_LEDS   36  /* NUM_LED_COLS(9) * NUM_LED_ROWS(4) */
#define DELUGE_PIC_SEG_DIGITS 4
#define DELUGE_PIC_GOLD_KNOBS 2
#define DELUGE_PIC_GOLD_LEDS  4

/* Largest message payload: vertical scroll carries (16 + 2) RGB triples. */
#define DELUGE_PIC_MAX_PAYLOAD ((16 + 2) * 3)

struct DelugePicState {
    /*< private >*/
    Chardev parent;

    /*< public >*/
    /*
     * The firmware receives PIC bytes via a DMA ring (SCFRDR -> picRxBuffer)
     * rather than by reading the SCIF data register, so responses are pushed
     * directly through the receive DMA channel. These are set by the board.
     */
    struct RzA1lDmacState *dmac;
    int rx_dma_channel;

    /*
     * The OLED panel whose data/command, chip-select and power lines the PIC
     * drives. Pixel data reaches the panel over SPI, not through here; only
     * the control lines are forwarded. Set by the board.
     */
    struct DelugeOledState *oled;

    /*
     * The RGB pad-grid and 7-segment numeric displays. The PIC forwards its
     * decoded pad colours / segment bitmasks to these renderers. Set by the
     * board; either may be NULL.
     */
    struct DelugePadGridState *padgrid;
    struct DelugeSegmentState *segment;

    /*
     * Command framing. When a command byte carries a payload, cmd holds it and
     * payload_have/payload_needed track collection of the following bytes.
     */
    uint8_t cmd;
    bool in_message;
    unsigned payload_have;
    unsigned payload_needed;
    uint8_t payload[DELUGE_PIC_MAX_PAYLOAD];

    /* Last UART-speed divisor the firmware programmed (PIC message 225). */
    uint8_t baud_div;

    /*
     * Display state decoded from the command stream, for the renderers.
     *   pad_grid  — RGB per pad, [column][row]; columns include the sidebar.
     *   led_on    — the 28 discrete indicator LEDs (mute/CV/etc.).
     *   seven_seg — raw segment bitmasks for the 4-digit numeric display.
     *   gold_knob — per-knob brightness for the 4 indicator LEDs around each
     *               of the two gold (endless) encoders.
     *   oled_*    — the OLED control lines the PIC drives (panel power, chip
     *               select and data/command); pixel data goes over SPI, not
     *               the PIC, so only these control bits are tracked here.
     */
    uint8_t pad_grid[DELUGE_PIC_GRID_COLS][DELUGE_PIC_GRID_ROWS][3];
    bool led_on[DELUGE_PIC_NUM_LEDS];
    uint8_t seven_seg[DELUGE_PIC_SEG_DIGITS];
    uint8_t gold_knob[DELUGE_PIC_GOLD_KNOBS][DELUGE_PIC_GOLD_LEDS];
    bool oled_enabled;
    bool oled_selected;
    bool oled_dc_high;
};

/*
 * Bind the PIC to the receive DMA channel the firmware uses for PIC input,
 * and to the DMAC that owns it. Called by the board after both exist.
 */
void deluge_pic_set_dma(Chardev *chr, struct RzA1lDmacState *dmac,
                        int rx_dma_channel);

/* Bind the OLED panel whose control lines the PIC drives (board setup). */
void deluge_pic_set_oled(Chardev *chr, struct DelugeOledState *oled);

/* Bind the RGB pad-grid renderer the PIC forwards pad colours to. */
void deluge_pic_set_padgrid(Chardev *chr, struct DelugePadGridState *padgrid);

/* Bind the 7-segment renderer the PIC forwards numeric updates to. */
void deluge_pic_set_segment(Chardev *chr, struct DelugeSegmentState *segment);

#endif /* HW_MISC_DELUGE_PIC_H */
