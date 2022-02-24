#ifndef HW_SW64_GPIO_H
#define HW_SW64_GPIO_H

#include "hw/sysbus.h"

#define TYPE_SW64_GPIO "SW64_GPIO"
#define SW64_GPIO(obj) OBJECT_CHECK(SW64GPIOState, (obj), TYPE_SW64_GPIO)

#define GPIO_SWPORTA_DR            (0x00UL)
#define GPIO_SWPORTA_DDR           (0X200UL)
#define GPIO_INTEN                 (0X1800UL)
#define GPIO_INTMASK               (0X1a00UL)
#define GPIO_INTTYPE_LEVEL         (0x1c00UL)
#define GPIO_INTTYPE_POLA          (0x1e00UL)
#define GPIO_INTTYPE_STATUS        (0x2000UL)
#define GPIO_RAW_INTTYPE_STATUS    (0x2200UL)
#define GPIO_DEB_ENABLE            (0x2400UL)
#define GPIO_CLEAN_INT             (0x2600UL)
#define GPIO_EXT_PORTA             (0x2800UL)
#define GPIO_SYNC_LEVEL            (0x3000UL)
#define GPIO_ID_CODE               (0x3200UL)
#define GPIO_VERSION               (0x3600UL)
#define GPIO_CONF_R1               (0x3a00UL)
#define GPIO_CONF_R2               (0x3800UL)

#define SW64_GPIO_MEM_SIZE 0x8000
#define SW64_GPIO_PIN_COUNT 1

typedef struct SW64GPIOState {
    SysBusDevice parent_obj;

    uint32_t padr;
    uint32_t paddr;
    uint32_t inter;
    uint32_t intmr;
    uint32_t intlr;
    uint32_t intpr;
    uint32_t intsr;
    uint32_t rintsr;
    uint32_t deber;
    uint32_t clintr;
    uint32_t expar;
    uint32_t synlr;
    uint32_t idcr;
    uint32_t versionr;
    uint32_t conf1r;
    uint32_t conf2r;

    qemu_irq irq[SW64_GPIO_PIN_COUNT];
    qemu_irq output[SW64_GPIO_PIN_COUNT];
} SW64GPIOState;

void sw64_gpio_set_irq(void *opaque, int irq, int level);
#endif
