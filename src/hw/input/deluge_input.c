/*
 * Deluge host-input mapping — keyboard to PIC pad/button events
 *
 * Translates host key and pointer events into Deluge pad/button matrix events
 * and injects them through the PIC's input-synthesis API. Named controls map
 * to dedicated keys, while pointer hit-testing over the composited skin maps
 * clicks onto the pad matrix.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "ui/input.h"
#include "hw/core/sysbus.h"
#include "hw/display/deluge_skin_layout.h"
#include "hw/input/deluge_input.h"
#include "hw/display/deluge_skin_controls.h"
#include "hw/misc/deluge_pic.h"
#include "hw/gpio/rza1l_gpio.h"

#define DELUGE_INPUT_MIN_PRESS_MS 120

/*
 * One host-key binding. A binding addresses either the button matrix (9 cols x
 * 4 rows) or the pad grid (18 cols x 8 rows); is_pad selects which API the
 * event is routed to. Button coordinates mirror the firmware's *ButtonCoord
 * constants (definitions_cxx.hpp); pad coordinates are grid (x, y).
 */
typedef struct DelugeInputBinding {
    int qcode;   /* QKeyCode of the host key */
    bool is_pad; /* true => pad grid, false => button matrix */
    int x;
    int y;
} DelugeInputBinding;

/*
 * Default key map. Buttons use letters/control keys that match the silkscreen
 * where practical; the audition column (sidebar column kDisplayWidth + 1 = 17)
 * is bound to the 1..8 number keys, top pad first.
 */
static const DelugeInputBinding deluge_input_bindings[] = {
    /* Transport and editing controls. */
    { Q_KEY_CODE_SPC,       false, 8, 3 }, /* PLAY              */
    { Q_KEY_CODE_R,         false, 8, 2 }, /* RECORD            */
    { Q_KEY_CODE_SHIFT,     false, 8, 0 }, /* SHIFT             */
    { Q_KEY_CODE_SHIFT_R,   false, 8, 0 }, /* SHIFT (right key) */
    { Q_KEY_CODE_BACKSPACE, false, 7, 1 }, /* BACK              */
    { Q_KEY_CODE_RET,       false, 4, 3 }, /* SELECT encoder click */

    /* View / instrument selection. */
    { Q_KEY_CODE_TAB,       false, 3, 1 }, /* SESSION_VIEW      */
    { Q_KEY_CODE_C,         false, 3, 2 }, /* CLIP_VIEW         */
    { Q_KEY_CODE_K,         false, 3, 3 }, /* KEYBOARD          */
    { Q_KEY_CODE_A,         false, 3, 0 }, /* AFFECT_ENTIRE     */
    { Q_KEY_CODE_Q,         false, 5, 0 }, /* SYNTH             */
    { Q_KEY_CODE_W,         false, 5, 1 }, /* KIT               */
    { Q_KEY_CODE_E,         false, 5, 2 }, /* MIDI              */
    { Q_KEY_CODE_T,         false, 5, 3 }, /* CV                */

    /* Function buttons. */
    { Q_KEY_CODE_M,         false, 6, 0 }, /* SCALE_MODE        */
    { Q_KEY_CODE_L,         false, 6, 1 }, /* LOAD              */
    { Q_KEY_CODE_S,         false, 6, 3 }, /* SAVE              */
    { Q_KEY_CODE_X,         false, 6, 2 }, /* CROSS_SCREEN_EDIT */
    { Q_KEY_CODE_N,         false, 7, 0 }, /* LEARN             */
    { Q_KEY_CODE_G,         false, 7, 2 }, /* SYNC_SCALING      */
    { Q_KEY_CODE_Y,         false, 7, 3 }, /* TAP_TEMPO         */
    { Q_KEY_CODE_U,         false, 8, 1 }, /* TRIPLETS          */

    /* Sidebar audition column (column kDisplayWidth + 1 = 17), top row first. */
    { Q_KEY_CODE_1,         true, 17, 0 },
    { Q_KEY_CODE_2,         true, 17, 1 },
    { Q_KEY_CODE_3,         true, 17, 2 },
    { Q_KEY_CODE_4,         true, 17, 3 },
    { Q_KEY_CODE_5,         true, 17, 4 },
    { Q_KEY_CODE_6,         true, 17, 5 },
    { Q_KEY_CODE_7,         true, 17, 6 },
    { Q_KEY_CODE_8,         true, 17, 7 },
};

static bool deluge_input_hit_test_pad(int px, int py, int *grid_x, int *grid_y)
{
    /*
     * Use full cell extents (pitch/2), not just the illuminated core, so
     * normal user clicks across the pad face register reliably.
     */
    int half_x_main = DELUGE_SKIN_PAD_MAIN_DX / 2;
    int half_x_side = DELUGE_SKIN_PAD_SIDE_DX / 2;
    int half_y = DELUGE_SKIN_PAD_SIDE_DY / 2;

    for (int row = 0; row < DELUGE_PIC_GRID_ROWS; row++) {
        /*
         * Firmware row 0 is the bottom of the grid; screen y grows downward, so
         * row 0 lives at the largest y. Mirror the renderer's mapping here so a
         * click reports the same row index the firmware drew.
         */
        int scr = DELUGE_PIC_GRID_ROWS - 1 - row;
        int cy = DELUGE_SKIN_PAD_SIDE_Y0 + scr * DELUGE_SKIN_PAD_SIDE_DY;

        for (int col = 0; col < 16; col++) {
            int cx = DELUGE_SKIN_PAD_MAIN_X0 + col * DELUGE_SKIN_PAD_MAIN_DX;

            if (abs(px - cx) <= half_x_main && abs(py - cy) <= half_y) {
                *grid_x = col;
                *grid_y = row;
                return true;
            }
        }

        for (int side = 0; side < 2; side++) {
            int col = 16 + side;
            int cx = DELUGE_SKIN_PAD_SIDE_X0 + side * DELUGE_SKIN_PAD_SIDE_DX;

            if (abs(px - cx) <= half_x_side && abs(py - cy) <= half_y) {
                *grid_x = col;
                *grid_y = row;
                return true;
            }
        }
    }

    return false;
}

/* Hit-test the circular buttons and rotary-encoder push-clicks. */
static const DelugeSkinControl *deluge_input_hit_test_control(int px, int py)
{
    for (size_t i = 0; i < ARRAY_SIZE(deluge_skin_controls); i++) {
        const DelugeSkinControl *c = &deluge_skin_controls[i];
        int dx = px - c->cx;
        int dy = py - c->cy;

        if (dx * dx + dy * dy <= c->radius * c->radius) {
            return c;
        }
    }

    return NULL;
}

/*
 * Map a rotary-encoder skin control to its DelugeEncoder id, or -1 if the
 * control is not a turnable encoder. The push-click controls share their names
 * with the encoders they sit under.
 */
static int deluge_input_encoder_for_control(const DelugeSkinControl *c)
{
    static const struct {
        const char *name;
        int enc;
    } map[] = {
        { "Y_ENC",         DELUGE_ENC_SCROLL_Y },
        { "X_ENC",         DELUGE_ENC_SCROLL_X },
        { "TEMPO_ENC",     DELUGE_ENC_TEMPO },
        { "SELECT_ENC",    DELUGE_ENC_SELECT },
        { "MOD_ENCODER_1", DELUGE_ENC_MOD_1 },
        { "MOD_ENCODER_0", DELUGE_ENC_MOD_0 },
    };

    if (!c) {
        return -1;
    }
    for (size_t i = 0; i < ARRAY_SIZE(map); i++) {
        if (strcmp(c->name, map[i].name) == 0) {
            return map[i].enc;
        }
    }
    return -1;
}

/* Route a host scroll-wheel notch over an encoder to a quadrature step. */
static void deluge_input_wheel(DelugeInputState *s, int dir)
{
    const DelugeSkinControl *ctrl;
    int enc;

    if (!s->gpio) {
        return;
    }

    ctrl = deluge_input_hit_test_control(s->pointer_x, s->pointer_y);
    enc = deluge_input_encoder_for_control(ctrl);
    if (enc < 0) {
        return;
    }

    fprintf(stderr, "deluge_input: wheel %s encoder %s -> step %+d\n",
            ctrl->name, dir > 0 ? "up" : "down", dir);
    rza1l_gpio_encoder_step(s->gpio, enc, dir);
}

static void deluge_input_pointer_press(DelugeInputState *s)
{
    const DelugeSkinControl *ctrl;
    int pad_x, pad_y;

    if (s->release_armed) {
        timer_del(s->release_timer);
        s->release_armed = false;
    }

    fprintf(stderr, "deluge_input: pointer press at (%d,%d)\n",
            s->pointer_x, s->pointer_y);

    /* Buttons/encoders sit above the pad grid; test them first. */
    ctrl = deluge_input_hit_test_control(s->pointer_x, s->pointer_y);
    if (ctrl) {
        fprintf(stderr, "deluge_input: press hit %s button (%d,%d)\n",
                ctrl->name, ctrl->col, ctrl->row);
        deluge_pic_button_event(s->pic, ctrl->col, ctrl->row, true);
        s->held_x = ctrl->col;
        s->held_y = ctrl->row;
        s->held_is_button = true;
        s->press_time_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
        return;
    }

    if (!deluge_input_hit_test_pad(s->pointer_x, s->pointer_y, &pad_x, &pad_y)) {
        fprintf(stderr, "deluge_input: press miss (no pad/button hit)\n");
        s->held_x = -1;
        s->held_y = -1;
        return;
    }

    fprintf(stderr, "deluge_input: press hit pad (%d,%d)\n", pad_x, pad_y);

    deluge_pic_pad_event(s->pic, pad_x, pad_y, true);
    s->held_x = pad_x;
    s->held_y = pad_y;
    s->held_is_button = false;
    s->press_time_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
}

static void deluge_input_pointer_release_now(DelugeInputState *s)
{
    if (s->release_armed) {
        timer_del(s->release_timer);
    }

    if (s->held_x < 0 || s->held_y < 0) {
        s->release_armed = false;
        return;
    }

    if (s->held_is_button) {
        fprintf(stderr, "deluge_input: pointer release button (%d,%d)\n",
                s->held_x, s->held_y);
        deluge_pic_button_event(s->pic, s->held_x, s->held_y, false);
    } else {
        fprintf(stderr, "deluge_input: pointer release pad (%d,%d)\n",
                s->held_x, s->held_y);
        deluge_pic_pad_event(s->pic, s->held_x, s->held_y, false);
    }
    s->held_x = -1;
    s->held_y = -1;
    s->release_armed = false;
}

static void deluge_input_pointer_release_cb(void *opaque)
{
    DelugeInputState *s = opaque;

    deluge_input_pointer_release_now(s);
}

static void deluge_input_pointer_release(DelugeInputState *s)
{
    int64_t now_ms;
    int64_t elapsed_ms;
    int64_t delay_ms;

    if (s->held_x < 0 || s->held_y < 0) {
        return;
    }
    if (s->release_armed) {
        return;
    }

    now_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    elapsed_ms = now_ms - s->press_time_ms;

    if (elapsed_ms >= DELUGE_INPUT_MIN_PRESS_MS) {
        deluge_input_pointer_release_now(s);
        return;
    }

    delay_ms = DELUGE_INPUT_MIN_PRESS_MS - elapsed_ms;
    s->release_armed = true;
    timer_mod(s->release_timer, now_ms + delay_ms);
}

static void deluge_input_event(DeviceState *dev, QemuConsole *src,
                               InputEvent *evt)
{
    DelugeInputState *s = DELUGE_INPUT(dev);

    if (!s->pic) {
        return;
    }

    switch (evt->type) {
    case INPUT_EVENT_KIND_KEY: {
        InputKeyEvent *key = evt->u.key.data;
        int qcode = qemu_input_key_value_to_qcode(key->key);

        for (size_t i = 0; i < ARRAY_SIZE(deluge_input_bindings); i++) {
            const DelugeInputBinding *b = &deluge_input_bindings[i];

            if (b->qcode != qcode) {
                continue;
            }
            if (b->is_pad) {
                deluge_pic_pad_event(s->pic, b->x, b->y, key->down);
            } else {
                deluge_pic_button_event(s->pic, b->x, b->y, key->down);
            }
            return;
        }

        break;
    }
    case INPUT_EVENT_KIND_ABS: {
        InputMoveEvent *move = evt->u.abs.data;

        if (move->axis == INPUT_AXIS_X) {
            s->pointer_x = qemu_input_scale_axis(move->value,
                                                 INPUT_EVENT_ABS_MIN,
                                                 INPUT_EVENT_ABS_MAX,
                                                 0,
                                                 DELUGE_SKIN_IMAGE_WIDTH - 1);
        } else if (move->axis == INPUT_AXIS_Y) {
            s->pointer_y = qemu_input_scale_axis(move->value,
                                                 INPUT_EVENT_ABS_MIN,
                                                 INPUT_EVENT_ABS_MAX,
                                                 0,
                                                 DELUGE_SKIN_IMAGE_HEIGHT - 1);
        }

        break;
    }
    case INPUT_EVENT_KIND_BTN: {
        InputBtnEvent *btn = evt->u.btn.data;

        if (btn->button == INPUT_BUTTON_WHEEL_UP) {
            if (btn->down) {
                deluge_input_wheel(s, +1);
            }
            break;
        }
        if (btn->button == INPUT_BUTTON_WHEEL_DOWN) {
            if (btn->down) {
                deluge_input_wheel(s, -1);
            }
            break;
        }
        if (btn->button != INPUT_BUTTON_LEFT) {
            break;
        }
        if (btn->down) {
            /* Guard against missing BTN_UP by releasing previous held pad. */
            deluge_input_pointer_release_now(s);
            deluge_input_pointer_press(s);
        } else {
            deluge_input_pointer_release(s);
        }

        break;
    }
    default:
        break;
    }
}

static const QemuInputHandler deluge_input_handler = {
    .name  = "Deluge keyboard/pointer",
    .mask  = INPUT_EVENT_MASK_KEY | INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = deluge_input_event,
};

void deluge_input_set_pic(DeviceState *dev, struct Chardev *pic)
{
    DelugeInputState *s = DELUGE_INPUT(dev);

    s->pic = pic;
}

void deluge_input_set_gpio(DeviceState *dev, DeviceState *gpio)
{
    DelugeInputState *s = DELUGE_INPUT(dev);

    s->gpio = gpio;
}

static void deluge_input_realize(DeviceState *dev, Error **errp)
{
    DelugeInputState *s = DELUGE_INPUT(dev);

    s->pointer_x = 0;
    s->pointer_y = 0;
    s->held_x = -1;
    s->held_y = -1;
    s->held_is_button = false;
    s->press_time_ms = 0;
    s->release_armed = false;
    s->release_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                    deluge_input_pointer_release_cb, s);

    s->handler = qemu_input_handler_register(dev, &deluge_input_handler);
    qemu_input_handler_activate(s->handler);
}

static void deluge_input_unrealize(DeviceState *dev)
{
    DelugeInputState *s = DELUGE_INPUT(dev);

    if (s->handler) {
        qemu_input_handler_unregister(s->handler);
        s->handler = NULL;
    }

    if (s->release_timer) {
        timer_del(s->release_timer);
        timer_free(s->release_timer);
        s->release_timer = NULL;
    }
}

static void deluge_input_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = deluge_input_realize;
    dc->unrealize = deluge_input_unrealize;
    dc->desc = "Synthstrom Deluge host-input mapping";
}

static const TypeInfo deluge_input_info = {
    .name          = TYPE_DELUGE_INPUT,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DelugeInputState),
    .class_init    = deluge_input_class_init,
};

static void deluge_input_register_types(void)
{
    type_register_static(&deluge_input_info);
}

type_init(deluge_input_register_types)
