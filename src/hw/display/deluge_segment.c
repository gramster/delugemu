/*
 * Deluge 7-segment display array — stub
 *
 * Holds the raw segment state for each digit. Rendering (to a console or as
 * text) and the GPIO/serial input path are implemented in M3.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/display/deluge_segment.h"

static void deluge_segment_reset(DeviceState *dev)
{
    DelugeSegmentState *s = DELUGE_SEGMENT(dev);

    memset(s->digits, 0, sizeof(s->digits));
}

static void deluge_segment_realize(DeviceState *dev, Error **errp)
{
    /* Nothing to allocate yet; state lives inline. */
}

static void deluge_segment_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = deluge_segment_realize;
    device_class_set_legacy_reset(dc, deluge_segment_reset);
    dc->desc = "Synthstrom Deluge 7-segment display";
}

static const TypeInfo deluge_segment_info = {
    .name          = TYPE_DELUGE_SEGMENT,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DelugeSegmentState),
    .class_init    = deluge_segment_class_init,
};

static void deluge_segment_register_types(void)
{
    type_register_static(&deluge_segment_info);
}

type_init(deluge_segment_register_types)
