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
#include "hw/display/deluge_skin.h"
#include "hw/misc/deluge_pic.h"
#include "hw/gpio/rza1l_gpio.h"

/*
 * Optional input tracing. Each call is a synchronous, unbuffered console write
 * on the vCPU thread; on slow consoles (notably Windows) a burst of these while
 * playing pads can stall emulation and disturb the audio ring, so they are off
 * unless DELUGE_INPUT_DEBUG is set in the environment.
 */
static int deluge_input_debug(void)
{
    static int v = -1;
    if (v < 0) {
        v = getenv("DELUGE_INPUT_DEBUG") != NULL;
    }
    return v;
}

#define INPUT_DBG(fmt, ...) \
    do { \
        if (deluge_input_debug()) { \
            fprintf(stderr, "deluge_input: " fmt "\n", ##__VA_ARGS__); \
        } \
    } while (0)

/*
 * Minimum time a pointer (mouse) press is held before its release is
 * delivered. A host click can deliver button-down and button-up within a
 * single frame, which is too fast for the firmware to register a visible
 * press, so we defer the release until at least this long has elapsed.
 *
 * The floor must stay below the firmware's short-press threshold
 * (FlashStorage::holdTime, 100 ms at the default "hold time" setting of 2).
 * Buttons with press/hold semantics — e.g. SCALE, which toggles scale mode on
 * a short press but shows the current scale on a hold — classify a press as a
 * hold once it exceeds holdTime. A floor at or above holdTime would make every
 * click read as a hold, so a quick click could never toggle. 60 ms is long
 * enough for the firmware to see and render the press, yet comfortably short
 * enough to count as a short press.
 */
#define DELUGE_INPUT_MIN_PRESS_MS 60

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

    INPUT_DBG("wheel %s encoder %s -> step %+d",
            ctrl->name, dir > 0 ? "up" : "down", dir);
    rza1l_gpio_encoder_step(s->gpio, enc, dir);
}

/*
 * Press-and-hold repeat rate for the encoder triangle affordances: the first
 * extra step fires DELAY ms after the initial click, then every INTERVAL ms
 * while the triangle stays held.
 */
#define DELUGE_ENC_REPEAT_DELAY_MS    350
#define DELUGE_ENC_REPEAT_INTERVAL_MS 90

static void deluge_input_encoder_repeat_cb(void *opaque)
{
    DelugeInputState *s = opaque;

    if (s->enc_repeat_id < 0 || !s->gpio) {
        return;
    }
    rza1l_gpio_encoder_step(s->gpio, s->enc_repeat_id, s->enc_repeat_dir);
    timer_mod(s->enc_repeat_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                  DELUGE_ENC_REPEAT_INTERVAL_MS);
}

/* Step an encoder once for a triangle click and arm the hold-repeat timer. */
static void deluge_input_encoder_begin_repeat(DelugeInputState *s, int enc,
                                              int dir)
{
    if (!s->gpio) {
        return;
    }
    INPUT_DBG("encoder %d triangle -> step %+d (hold-repeat)",
            enc, dir);
    rza1l_gpio_encoder_step(s->gpio, enc, dir);
    s->enc_repeat_id = enc;
    s->enc_repeat_dir = dir;
    timer_mod(s->enc_repeat_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                  DELUGE_ENC_REPEAT_DELAY_MS);
}

/* Stop any in-progress encoder hold-repeat (mouse released or new press). */
static void deluge_input_encoder_stop_repeat(DelugeInputState *s)
{
    if (s->enc_repeat_id >= 0) {
        timer_del(s->enc_repeat_timer);
        s->enc_repeat_id = -1;
        s->enc_repeat_dir = 0;
    }
}

/* Deliver a press/release for a pad or button on the appropriate PIC API. */
static void deluge_input_emit(DelugeInputState *s, int x, int y,
                              bool is_button, bool down)
{
    if (is_button) {
        deluge_pic_button_event(s->pic, x, y, down);
    } else {
        deluge_pic_pad_event(s->pic, x, y, down);
    }
}

/* Index of a latched entry matching (x, y, is_button), or -1 if not latched. */
static int deluge_input_latch_find(DelugeInputState *s, int x, int y,
                                   bool is_button)
{
    for (int i = 0; i < s->latched_count; i++) {
        if (s->latched[i].x == x && s->latched[i].y == y &&
            s->latched[i].is_button == is_button) {
            return i;
        }
    }
    return -1;
}

/*
 * Toggle a control's latched state. If it is not yet latched, press and hold it
 * (recording it so it can be released later); if it is already latched, release
 * it and drop it from the table.
 */
static void deluge_input_latch_toggle(DelugeInputState *s, int x, int y,
                                      bool is_button)
{
    int idx = deluge_input_latch_find(s, x, y, is_button);

    if (idx >= 0) {
        deluge_input_emit(s, x, y, is_button, false);
        s->latched[idx] = s->latched[s->latched_count - 1];
        s->latched_count--;
        INPUT_DBG("unlatch %s (%d,%d)",
                is_button ? "button" : "pad", x, y);
        return;
    }

    if (s->latched_count >= DELUGE_INPUT_MAX_LATCHED) {
        INPUT_DBG("latch table full, ignoring (%d,%d)",
                x, y);
        return;
    }

    deluge_input_emit(s, x, y, is_button, true);
    s->latched[s->latched_count].x = x;
    s->latched[s->latched_count].y = y;
    s->latched[s->latched_count].is_button = is_button;
    s->latched_count++;
    INPUT_DBG("latch %s (%d,%d)",
            is_button ? "button" : "pad", x, y);
}

/* Release every still-latched control (host latch modifier was released). */
static void deluge_input_latch_release_all(DelugeInputState *s)
{
    if (s->latched_count) {
        INPUT_DBG("releasing %d latched control(s)",
                s->latched_count);
    }
    for (int i = 0; i < s->latched_count; i++) {
        deluge_input_emit(s, s->latched[i].x, s->latched[i].y,
                          s->latched[i].is_button, false);
    }
    s->latched_count = 0;
}

static void deluge_input_pointer_press(DelugeInputState *s)
{
    const DelugeSkinControl *ctrl;
    int pad_x, pad_y;

    if (s->release_armed) {
        timer_del(s->release_timer);
        s->release_armed = false;
    }

    INPUT_DBG("pointer press at (%d,%d)",
            s->pointer_x, s->pointer_y);

    /* Buttons/encoders sit above the pad grid; test them first. */
    ctrl = deluge_input_hit_test_control(s->pointer_x, s->pointer_y);
    if (ctrl) {
        int enc = deluge_input_encoder_for_control(ctrl);

        if (enc >= 0) {
            DelugeEncTriHit hit = deluge_enc_tri_hit(ctrl, s->pointer_x,
                                                     s->pointer_y);

            /*
             * A click on either triangle turns the encoder rather than
             * depressing it; a click inside the circle but outside both
             * triangles falls through to the normal button (depress) path.
             */
            if (hit != DELUGE_ENC_HIT_NONE) {
                int dir = (hit == DELUGE_ENC_HIT_UP) ? +1 : -1;

                deluge_input_encoder_begin_repeat(s, enc, dir);
                return;
            }
        }

        INPUT_DBG("press hit %s button (%d,%d)",
                ctrl->name, ctrl->col, ctrl->row);
        if (s->latch_active) {
            deluge_input_latch_toggle(s, ctrl->col, ctrl->row, true);
            return;
        }
        deluge_pic_button_event(s->pic, ctrl->col, ctrl->row, true);
        s->held_x = ctrl->col;
        s->held_y = ctrl->row;
        s->held_is_button = true;
        s->press_time_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
        return;
    }

    if (!deluge_input_hit_test_pad(s->pointer_x, s->pointer_y, &pad_x, &pad_y)) {
        INPUT_DBG("press miss (no pad/button hit)");
        s->held_x = -1;
        s->held_y = -1;
        return;
    }

    INPUT_DBG("press hit pad (%d,%d)", pad_x, pad_y);

    if (s->latch_active) {
        deluge_input_latch_toggle(s, pad_x, pad_y, false);
        return;
    }

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
        INPUT_DBG("pointer release button (%d,%d)",
                s->held_x, s->held_y);
        deluge_pic_button_event(s->pic, s->held_x, s->held_y, false);
    } else {
        INPUT_DBG("pointer release pad (%d,%d)",
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

        /*
         * The latch modifier (Left Alt/Option, plus the right key) is not a
         * Deluge control: while held it turns left-clicks into latch toggles,
         * and releasing it drops every still-latched pad/button.
         */
        if (qcode == Q_KEY_CODE_ALT || qcode == Q_KEY_CODE_ALT_R) {
            s->latch_active = key->down;
            if (!key->down) {
                deluge_input_latch_release_all(s);
            }
            return;
        }

        /*
         * Right Ctrl is a hold-to-suspend control: while it is held, the
         * front-panel skin rendering is suspended so the host UI does no
         * per-frame scaling/blit and the main loop stays free to feed audio
         * during a performance; releasing it resumes rendering. Edge-detected
         * so the key's auto-repeat does not re-trigger the transition.
         */
        if (qcode == Q_KEY_CODE_CTRL_R) {
            if (key->down != s->render_key_down) {
                s->render_key_down = key->down;
                s->render_suspended = key->down;
                if (s->skin) {
                    deluge_skin_set_render_suspended(s->skin,
                                                     s->render_suspended);
                }
            }
            return;
        }

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
            deluge_input_encoder_stop_repeat(s);
            deluge_input_pointer_release_now(s);
            deluge_input_pointer_press(s);
        } else {
            deluge_input_encoder_stop_repeat(s);
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

void deluge_input_set_skin(DeviceState *dev, DeviceState *skin)
{
    DelugeInputState *s = DELUGE_INPUT(dev);

    s->skin = skin;
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

    s->enc_repeat_id = -1;
    s->enc_repeat_dir = 0;
    s->enc_repeat_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                       deluge_input_encoder_repeat_cb, s);

    s->latch_active = false;
    s->latched_count = 0;

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

    if (s->enc_repeat_timer) {
        timer_del(s->enc_repeat_timer);
        timer_free(s->enc_repeat_timer);
        s->enc_repeat_timer = NULL;
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
