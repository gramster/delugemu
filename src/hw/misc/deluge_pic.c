/*
 * Deluge input PIC bridge — stub
 *
 * Models the serial bridge to the Deluge's input PIC. For now it only
 * establishes the QOM type, the serial backend and an IRQ line; the event
 * protocol (pads/buttons/encoders in, LED/PWM out) is implemented in M3.
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/irq.h"
#include "hw/misc/deluge_pic.h"

static void deluge_pic_realize(DeviceState *dev, Error **errp)
{
    DelugePicState *s = DELUGE_PIC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    sysbus_init_irq(sbd, &s->irq);
    /* TODO(M3): hook chr receive handlers and drive input events. */
}

static const Property deluge_pic_properties[] = {
    DEFINE_PROP_CHR("chardev", DelugePicState, chr),
};

static void deluge_pic_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = deluge_pic_realize;
    dc->desc = "Synthstrom Deluge input PIC bridge";
    device_class_set_props(dc, deluge_pic_properties);
}

static const TypeInfo deluge_pic_info = {
    .name          = TYPE_DELUGE_PIC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DelugePicState),
    .class_init    = deluge_pic_class_init,
};

static void deluge_pic_register_types(void)
{
    type_register_static(&deluge_pic_info);
}

type_init(deluge_pic_register_types)
