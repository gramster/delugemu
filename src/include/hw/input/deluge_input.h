/*
 * Deluge host-input mapping — keyboard to PIC pad/button events
 *
 * Bridges QEMU's host input to the modelled Deluge so the firmware can be
 * driven headlessly, before the graphical hardware skin exists. Host key
 * presses are translated into the same pad/button matrix events the PIC
 * coprocessor would report for the physical surface, and injected through the
 * PIC's input-synthesis API (deluge_pic_pad_event / deluge_pic_button_event).
 *
 * The device carries no MMIO; it exists purely to own a QEMU keyboard input
 * handler and a reference to the PIC it feeds.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_INPUT_DELUGE_INPUT_H
#define HW_INPUT_DELUGE_INPUT_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

struct Chardev;
struct QEMUTimer;
struct QemuInputHandlerState;

#define TYPE_DELUGE_INPUT "deluge-input"
typedef struct DelugeInputState DelugeInputState;
DECLARE_INSTANCE_CHECKER(DelugeInputState, DELUGE_INPUT, TYPE_DELUGE_INPUT)

/* Maximum number of pads/buttons that can be latched down simultaneously. */
#define DELUGE_INPUT_MAX_LATCHED 64

struct DelugeInputState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    /* The PIC the synthesised pad/button events are fed into (set by board). */
    struct Chardev *pic;

    /* The GPIO model driven for rotary-encoder quadrature steps (board setup). */
    DeviceState *gpio;

    /*
     * The SSIF audio model whose host monitor OUTPUT LEVEL the master volume
     * knob's triangles attenuate (board setup). May be NULL (headless).
     */
    DeviceState *ssif;

    /* The registered QEMU keyboard/pointer handler. */
    struct QemuInputHandlerState *handler;

    /* Last tracked pointer position, in skin-image pixel coordinates. */
    int pointer_x;
    int pointer_y;

    /*
     * Currently held control from a pointer press. held_x < 0 means nothing is
     * held; held_is_button selects the button matrix vs. the pad grid for the
     * matching release.
     */
    int held_x;
    int held_y;
    bool held_is_button;

    /* Pointer press timing and deferred-release state. */
    struct QEMUTimer *release_timer;
    int64_t press_time_ms;
    bool release_armed;

    /*
     * Rotary-encoder triangle affordance press-and-hold repeat. enc_repeat_id
     * is the DelugeEncoder currently being stepped while a triangle is held
     * (-1 when idle); enc_repeat_dir is its direction (+1 CW / -1 CCW).
     */
    struct QEMUTimer *enc_repeat_timer;
    int enc_repeat_id;
    int enc_repeat_dir;

    /*
     * Multi-press latch. While the host latch modifier (Left Alt/Option) is
     * held, latch_active is true and a left-click toggles a control's latched
     * state instead of doing a momentary press: the first click presses and
     * holds it, a second click on the same control releases it. Releasing the
     * modifier releases every still-latched control. latched[] records the held
     * pad/button coordinates so they can all be released together.
     */
    bool latch_active;
    int latched_count;
    struct {
        int x;
        int y;
        bool is_button;
    } latched[DELUGE_INPUT_MAX_LATCHED];

    /*
     * The composited skin device, toggled into/out of audio-only mode from the
     * keyboard (Right Ctrl). May be NULL when no skin is attached (headless).
     */
    DeviceState *skin;

    /* Audio-only toggle state and edge-detect for the Right Ctrl key. */
    bool render_suspended;
    bool render_key_down;
};

/* Bind the PIC whose input stream this device drives (board setup). */
void deluge_input_set_pic(DeviceState *dev, struct Chardev *pic);

/* Bind the GPIO model that rotary-encoder turns are injected into. */
void deluge_input_set_gpio(DeviceState *dev, DeviceState *gpio);

/* Bind the SSIF model that the master OUTPUT LEVEL knob attenuates. */
void deluge_input_set_ssif(DeviceState *dev, DeviceState *ssif);

/* Bind the skin device that the audio-only keyboard toggle suspends. */
void deluge_input_set_skin(DeviceState *dev, DeviceState *skin);

#endif /* HW_INPUT_DELUGE_INPUT_H */
