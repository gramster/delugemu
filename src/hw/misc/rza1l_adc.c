/*
 * Renesas RZ/A1 ADC (S12AD) — minimal battery-sense stub
 *
 * The Deluge firmware uses one ADC channel to monitor the system supply
 * voltage (SYS_VOLT_SENSE_PIN = 5, read back from ADDRF) so it can drive the
 * battery LED. Its READ_INPUTS routine (deluge.cpp) does, every 100 ms:
 *
 *     if (ADC.ADCSR & (1 << 15)) {            // conversion result ready
 *         int32_t reading = ADC.ADDRF;        // 16-bit result word
 *         ... batteryMV = (reading * 3300) >> 15; ...
 *     }
 *     ADC.ADCSR = (1 << 13) | (0b011 << 6) | SYS_VOLT_SENSE_PIN;  // re-arm
 *
 * This stub keeps that loop fed: ADCSR always reads back with the
 * conversion-ready bit set, and ADDRF returns a fixed reading that the
 * firmware's filter resolves to a healthy battery voltage (~3.7 V), so the
 * battery LED settles solid. The remaining result/compare registers read 0.
 *
 * Register layout (offsetof on the firmware's st_adc, based at 0xE8005800):
 *   ADDRA..ADDRH 0x00..0x0E (16)   ADCMPHA..ADCMPLH 0x20..0x3E (16)
 *   ADCSR 0x60 (16)  ADCMPER 0x62  ADCMPSR 0x64
 *
 * Copyright (c) 2026 delugemu contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/misc/rza1l_adc.h"

#define ADC_ADDRF  0x0A  /* channel F result = SYS_VOLT_SENSE_PIN (5) */
#define ADC_ADCSR  0x60

#define ADCSR_ADST 0x8000  /* conversion start/ready bit the firmware polls */

/*
 * Fixed result word for the supply-sense channel. The firmware computes
 * batteryMV = (reading * 3300) >> 15, deliberately halving the divisor to undo
 * the board's resistive divider, so 0x9000 (36864) yields ~3.7 V — a healthy
 * battery that drives the LED solid.
 */
#define ADC_BATTERY_READING 0x9000

static uint64_t rza1l_adc_read(void *opaque, hwaddr offset, unsigned size)
{
    RzA1lAdcState *s = opaque;

    switch (offset) {
    case ADC_ADDRF:
        return ADC_BATTERY_READING;
    case ADC_ADCSR:
        /* Always report the most recent conversion as complete. */
        return s->adcsr | ADCSR_ADST;
    default:
        return 0;
    }
}

static void rza1l_adc_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    RzA1lAdcState *s = opaque;

    if (offset == ADC_ADCSR) {
        /* Record the re-arm; the result is reported ready on the next read. */
        s->adcsr = (uint16_t)value & ~ADCSR_ADST;
    }
}

static const MemoryRegionOps rza1l_adc_ops = {
    .read = rza1l_adc_read,
    .write = rza1l_adc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
};

static void rza1l_adc_reset(DeviceState *dev)
{
    RzA1lAdcState *s = RZA1L_ADC(dev);

    s->adcsr = 0;
}

static void rza1l_adc_realize(DeviceState *dev, Error **errp)
{
    RzA1lAdcState *s = RZA1L_ADC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rza1l_adc_ops, s,
                          TYPE_RZA1L_ADC, RZA1L_ADC_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_rza1l_adc = {
    .name = TYPE_RZA1L_ADC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(adcsr, RzA1lAdcState),
        VMSTATE_END_OF_LIST()
    },
};

static void rza1l_adc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rza1l_adc_realize;
    dc->vmsd = &vmstate_rza1l_adc;
    device_class_set_legacy_reset(dc, rza1l_adc_reset);
}

static const TypeInfo rza1l_adc_info = {
    .name          = TYPE_RZA1L_ADC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RzA1lAdcState),
    .class_init    = rza1l_adc_class_init,
};

static void rza1l_adc_register_types(void)
{
    type_register_static(&rza1l_adc_info);
}

type_init(rza1l_adc_register_types)
