/*
 * Copyright (c) 2024 Wxiat Corporation
 * Written by Wuyacheng
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

// SPDX-License-Identifier: GPL-2.0+

#include "qemu/osdep.h"
#include "exec/address-spaces.h"
#include "migration/vmstate.h"
#include "hw/sw64/gpio.h"
#include "hw/irq.h"

static void sw64_gpio_update_int(SW64GPIOState *s)
{
    qemu_set_irq(s->irq[0], 1);
}

static void sw64_gpio_set(void *opaque, int line, int level)
{
    SW64GPIOState *s = SW64_GPIO(opaque);

    sw64_gpio_update_int(s);
}

static void sw64_gpio_write(void *opaque, hwaddr offset, uint64_t reg_value,
                            unsigned size)
{
    SW64GPIOState *s = SW64_GPIO(opaque);
    reg_value = (uint32_t)reg_value;

    switch (offset) {
    case GPIO_SWPORTA_DR:
        s->padr = reg_value;
        break;
    case GPIO_SWPORTA_DDR:
        s->paddr = reg_value ;
        break;
    case GPIO_INTEN:
        s->inter = reg_value;
        break;
    case GPIO_INTMASK:
        s->intmr = reg_value;
        break;
    case GPIO_INTTYPE_LEVEL:
        s->intlr = reg_value;
        break;
    case GPIO_INTTYPE_POLA:
        s->intpr = reg_value;
        break;
    case GPIO_INTTYPE_STATUS:
        s->intsr = reg_value;
        break;
    case GPIO_RAW_INTTYPE_STATUS:
        s->rintsr = reg_value;
        break;
    case GPIO_DEB_ENABLE:
        s->deber = reg_value;
        break;
    case GPIO_CLEAN_INT:
        s->clintr = reg_value;
        break;
    case GPIO_EXT_PORTA:
        s->expar = reg_value;
        break;
    case GPIO_SYNC_LEVEL:
        s->synlr = reg_value;
        break;
    case GPIO_ID_CODE:
        s->idcr = reg_value;
        break;
    case GPIO_VERSION:
        s->versionr = reg_value;
        break;
    case GPIO_CONF_R1:
        s->conf1r = reg_value;
        break;
    case GPIO_CONF_R2:
        s->conf2r = reg_value;
        break;
    default:
        printf("error: Bad register at offset 0x%lx\n", offset);
    }

    return;
}

static uint64_t sw64_gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    SW64GPIOState *s = SW64_GPIO(opaque);
    uint32_t reg_value = 0;

    switch (offset) {
    case GPIO_SWPORTA_DR:
        reg_value = s->padr;
        break;
    case GPIO_SWPORTA_DDR:
        reg_value = s->paddr;
        break;
    case GPIO_INTEN:
        reg_value = s->inter;
        break;
    case GPIO_INTMASK:
        reg_value = s->intmr;
        break;
    case GPIO_INTTYPE_LEVEL:
        reg_value = s->intlr;
        break;
    case GPIO_INTTYPE_POLA:
        reg_value = s->intpr;
        break;
    case GPIO_INTTYPE_STATUS:
        reg_value = s->intsr;
        break;
    case GPIO_RAW_INTTYPE_STATUS:
        reg_value = s->rintsr;
        break;
    case GPIO_DEB_ENABLE:
        reg_value = s->deber;
        break;
    case GPIO_CLEAN_INT:
        reg_value = s->clintr;
        break;
    case GPIO_EXT_PORTA:
        reg_value = s->expar;
        break;
    case GPIO_SYNC_LEVEL:
        reg_value = s->synlr;
        break;
    case GPIO_ID_CODE:
        reg_value = s->idcr;
        break;
    case GPIO_VERSION:
        reg_value = s->versionr;
        break;
    case GPIO_CONF_R1:
        reg_value = s->conf1r;
        break;
    case GPIO_CONF_R2:
        reg_value = s->conf2r;
        break;
    default:
        printf("error: Bad register at offset 0x%lx\n", offset);
        return (uint64_t)0;
    }

    return (uint64_t)reg_value;
}

static const VMStateDescription vmstate_sw64_gpio = {
    .name = TYPE_SW64_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(padr, SW64GPIOState),
        VMSTATE_UINT32(paddr, SW64GPIOState),
        VMSTATE_UINT32(inter, SW64GPIOState),
        VMSTATE_UINT32(intmr, SW64GPIOState),
        VMSTATE_UINT32(intlr, SW64GPIOState),
        VMSTATE_UINT32(intpr, SW64GPIOState),
        VMSTATE_UINT32(intsr, SW64GPIOState),
        VMSTATE_UINT32(rintsr, SW64GPIOState),
        VMSTATE_UINT32(deber, SW64GPIOState),
        VMSTATE_UINT32(clintr, SW64GPIOState),
        VMSTATE_UINT32(expar, SW64GPIOState),
        VMSTATE_UINT32(synlr, SW64GPIOState),
        VMSTATE_UINT32(idcr, SW64GPIOState),
        VMSTATE_UINT32(versionr, SW64GPIOState),
        VMSTATE_UINT32(conf1r, SW64GPIOState),
        VMSTATE_UINT32(conf2r, SW64GPIOState),
        VMSTATE_END_OF_LIST()
    }
};

static void sw64_gpio_reset(DeviceState *dev)
{
    SW64GPIOState *s = SW64_GPIO(dev);

    s->padr = 0;
    s->paddr = 0;
    s->inter = 0;
    s->intmr = 0;
    s->intlr = 0;
    s->intpr = 0;
    s->intsr = 0;
    s->rintsr = 0;
    s->deber = 0;
    s->clintr = 0;
    s->expar = 0;
    s->synlr = 0;
    s->idcr = 0;
    s->versionr = 0;
    s->conf1r = 0;
    s->conf2r = 0;

    sw64_gpio_update_int(s);
}

static const MemoryRegionOps sw64_gpio_ops = {
    .read = sw64_gpio_read,
    .write = sw64_gpio_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};


static void sw64_gpio_realize(DeviceState *dev, Error **errp)
{
    SW64GPIOState *s = SW64_GPIO(dev);
    MemoryRegion *sw64_gpio = g_new(MemoryRegion, 1);
    memory_region_init_io(sw64_gpio, OBJECT(s), &sw64_gpio_ops, s,
                          TYPE_SW64_GPIO, SW64_GPIO_MEM_SIZE);

    qdev_init_gpio_in(DEVICE(s), sw64_gpio_set, SW64_GPIO_PIN_COUNT);
    qdev_init_gpio_out(DEVICE(s), s->output, SW64_GPIO_PIN_COUNT);
    s->irq[0] = qemu_allocate_irq(sw64_gpio_set_irq, s, 15);
    memory_region_add_subregion(get_system_memory(), 0x804930000000ULL,
                                sw64_gpio);
}

static void sw64_gpio_initfn(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    Error *errp;
    sw64_gpio_realize(dev, &errp);
}


static void sw64_gpio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = sw64_gpio_realize;
    dc->reset = sw64_gpio_reset;
    dc->vmsd = &vmstate_sw64_gpio;
    dc->desc = "SW64 GPIO controller";
}

static const TypeInfo sw64_gpio_info = {
    .name = TYPE_SW64_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SW64GPIOState),
    .instance_init = sw64_gpio_initfn,
    .class_init = sw64_gpio_class_init,
};

static void sw64_gpio_register_types(void)
{
    type_register_static(&sw64_gpio_info);
}

type_init(sw64_gpio_register_types)
