/*
 * Deluge host-input mapping — keyboard to PIC pad/button events
 *
 * Translates host key events into Deluge pad/button matrix events and injects
 * them through the PIC's input-synthesis API. This makes the firmware usable
 * headlessly: the named control buttons (transport, view selection, shift,
 * etc.) map to dedicated keys, and the eight audition pads of the sidebar map
 * to the number row. The full 16x8 main pad grid is intentionally left to the
 * future graphical skin, where pointer hit-testing is the natural fit.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "ui/input.h"
#include "hw/core/sysbus.h"
#include "hw/input/deluge_input.h"
#include "hw/misc/deluge_pic.h"

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

static void deluge_input_event(DeviceState *dev, QemuConsole *src,
                               InputEvent *evt)
{
    DelugeInputState *s = DELUGE_INPUT(dev);
    InputKeyEvent *key = evt->u.key.data;
    int qcode = qemu_input_key_value_to_qcode(key->key);

    if (!s->pic) {
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
}

static const QemuInputHandler deluge_input_handler = {
    .name  = "Deluge keyboard",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = deluge_input_event,
};

void deluge_input_set_pic(DeviceState *dev, struct Chardev *pic)
{
    DelugeInputState *s = DELUGE_INPUT(dev);

    s->pic = pic;
}

static void deluge_input_realize(DeviceState *dev, Error **errp)
{
    DelugeInputState *s = DELUGE_INPUT(dev);

    s->handler = qemu_input_handler_register(dev, &deluge_input_handler);
}

static void deluge_input_unrealize(DeviceState *dev)
{
    DelugeInputState *s = DELUGE_INPUT(dev);

    if (s->handler) {
        qemu_input_handler_unregister(s->handler);
        s->handler = NULL;
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
