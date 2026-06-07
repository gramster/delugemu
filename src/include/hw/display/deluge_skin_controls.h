/*
 * Deluge front-panel control layout (buttons + rotary-encoder push-clicks)
 *
 * Pixel geometry of every clickable front-panel control in Deluge_Plain.png,
 * auto-detected by scripts/skin_calibrate_controls.py (black-bordered light
 * interiors flood-filled from hand-measured seed centres) and paired with the
 * firmware's 9x4 button-matrix coordinates (definitions_cxx.hpp).
 *
 * Each control sends a button-matrix event at (col, row). Controls that also
 * carry an indicator LED expose it through has_led; the firmware drives that
 * LED's state at PIC index (col + 9 * row) — see hid/led/indicator_leds.cpp.
 * Encoder push-clicks share the button matrix but have no indicator LED.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_DELUGE_SKIN_CONTROLS_H
#define HW_DISPLAY_DELUGE_SKIN_CONTROLS_H

typedef struct DelugeSkinControl {
    const char *name;
    int col;        /* button-matrix column (0..8) */
    int row;        /* button-matrix row (0..3)    */
    int cx;         /* centre x in skin pixels     */
    int cy;         /* centre y in skin pixels     */
    int radius;     /* hit-test / LED-glow radius  */
    bool has_led;   /* true => indicator LED at led_on[col + 9 * row] */
    uint8_t led_r;  /* colour shown when the LED is lit */
    uint8_t led_g;
    uint8_t led_b;
} DelugeSkinControl;

/* Default "lit" colour for indicator LEDs without a distinctive hardware hue. */
#define DELUGE_LED_AMBER 255, 110, 0
#define DELUGE_LED_GREEN 0, 210, 40
#define DELUGE_LED_RED   235, 20, 20

/*
 * Layout table. Button (col, row) come from the firmware *ButtonCoord / MOD
 * button constants; centres and radii are auto-detected (radius 24 for the
 * ~50px circular buttons, ~64-68 for the ~136px rotary encoders).
 */
static const DelugeSkinControl deluge_skin_controls[] = {
    /* 8 MOD (gold-knob assignment) buttons, top row; LEDs MOD_0..MOD_7. */
    { "MOD0",          1, 0,  321, 332, 24, true,  DELUGE_LED_AMBER },
    { "MOD1",          1, 1,  412, 332, 24, true,  DELUGE_LED_AMBER },
    { "MOD2",          1, 2,  503, 332, 24, true,  DELUGE_LED_AMBER },
    { "MOD3",          1, 3,  593, 332, 24, true,  DELUGE_LED_AMBER },
    { "MOD4",          2, 0,  684, 332, 24, true,  DELUGE_LED_AMBER },
    { "MOD5",          2, 1,  775, 332, 24, true,  DELUGE_LED_AMBER },
    { "MOD6",          2, 2,  865, 332, 24, true,  DELUGE_LED_AMBER },
    { "MOD7",          2, 3,  956, 332, 24, true,  DELUGE_LED_AMBER },

    /* Named function buttons (indicator LED at the same col,row). */
    { "AFFECT_ENTIRE", 3, 0,  684, 468, 24, true,  DELUGE_LED_AMBER },
    { "SESSION_VIEW",  3, 1,  865, 424, 24, true,  DELUGE_LED_AMBER },
    { "CLIP_VIEW",     3, 2,  865, 515, 24, true,  DELUGE_LED_AMBER },
    { "SYNTH",         5, 0, 1187, 424, 24, true,  DELUGE_LED_AMBER },
    { "KIT",           5, 1, 1268, 424, 24, true,  DELUGE_LED_AMBER },
    { "MIDI",          5, 2, 1350, 424, 24, true,  DELUGE_LED_AMBER },
    { "CV",            5, 3, 1432, 424, 24, true,  DELUGE_LED_AMBER },
    { "SCALE_MODE",    6, 0, 1205, 515, 24, true,  DELUGE_LED_AMBER },
    { "CROSS_SCREEN",  6, 2, 1350, 515, 24, true,  DELUGE_LED_AMBER },
    { "BACK",          7, 1, 1529, 241, 24, true,  DELUGE_LED_AMBER },
    { "LOAD",          6, 1, 1529, 332, 24, true,  DELUGE_LED_AMBER },
    { "SAVE",          6, 3, 1529, 424, 24, true,  DELUGE_LED_AMBER },
    { "LEARN",         7, 0, 1529, 515, 24, true,  DELUGE_LED_AMBER },
    { "TAP_TEMPO",     7, 3, 1812, 332, 24, true,  DELUGE_LED_AMBER },
    { "SYNC_SCALING",  7, 2, 1812, 424, 24, true,  DELUGE_LED_AMBER },
    { "TRIPLETS",      8, 1, 1812, 515, 24, true,  DELUGE_LED_AMBER },
    { "PLAY",          8, 3, 2072, 332, 24, true,  DELUGE_LED_GREEN },
    { "RECORD",        8, 2, 2072, 424, 24, true,  DELUGE_LED_RED   },
    { "SHIFT",         8, 0, 2072, 515, 24, true,  DELUGE_LED_AMBER },

    /* Rotary-encoder push-clicks (button matrix, no indicator LED). */
    { "Y_ENC",         0, 0,   94, 469, 60, false, 0, 0, 0 },
    { "X_ENC",         0, 1,  321, 196, 60, false, 0, 0, 0 },
    { "MOD_ENCODER_0", 0, 2,  549, 469, 60, false, 0, 0, 0 },
    { "MOD_ENCODER_1", 0, 3,  775, 196, 60, false, 0, 0, 0 },
    { "SELECT_ENC",    4, 3, 1067, 331, 60, false, 0, 0, 0 },
    { "TEMPO_ENC",     4, 1, 1811, 196, 60, false, 0, 0, 0 },
};

/*
 * Gold-knob level LEDs (the two vertical stacks of 4 square LEDs beside the
 * gold modal encoders). Driven by PIC SET_GOLD_KNOB0/1; brightness per LED is
 * gold_knob[which][0..3]. The meter fills from the bottom up, so index 0 is the
 * bottom LED and cy[] is ordered bottom -> top to match.
 */
typedef struct DelugeSkinKnobLeds {
    int which;      /* PIC gold_knob[] index    */
    int cx;         /* common centre x          */
    int cy[4];      /* centre y, bottom..top    */
    int half;       /* half square size         */
} DelugeSkinKnobLeds;

/* The gold-knob LED table itself lives in the renderer (deluge_skin.c). */

#endif /* HW_DISPLAY_DELUGE_SKIN_CONTROLS_H */
