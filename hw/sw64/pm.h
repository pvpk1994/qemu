#ifndef HW_SW64_PM_H
#define HW_SW64_PM_H

#include "hw/sysbus.h"
#include "hw/acpi/memory_hotplug.h"

#include "hw/acpi/cpu_hotplug.h"
#include "hw/acpi/cpu.h"
#include "hw/irq.h"
#include "hw/acpi/acpi_dev_interface.h"
#include "hw/core/cpu.h"
#include "qemu/option.h"
#include "qemu/option_int.h"
#include "qemu/config-file.h"
#include "qapi/qmp/qdict.h"

#define SUNWAY_CPUHOTPLUG_ADD    0x4
#define SUNWAY_CPUHOTPLUG_REMOVE 0x8
#define OFFSET_CPUID             0x20

extern int cpu_hot_id;
int get_state_cpumask(int cpu_num);
void sw64_cpu_hotplug_hw_init(CPUHotplugState *state, hwaddr base_addr);
void sw64_acpi_switch_to_modern_cphp(CPUHotplugState *cpuhp_state, uint16_t io_port);

#define OFFSET_START_ADDR       0x0
#define OFFSET_LENGTH           0x8
#define OFFSET_STATUS           0x10
#define OFFSET_SLOT             0x18

#define OFFSET_NODE             0x28

#define SUNWAY_MEMHOTPLUG_ADD      0x1
#define SUNWAY_MEMHOTPLUG_REMOVE   0x2

typedef struct SW64PMState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    qemu_irq irq;
    MemHotplugState acpi_memory_hotplug;
    unsigned long addr;
    unsigned long length;
    unsigned long status;
    unsigned long slot;
    unsigned long cpuid;
    unsigned long node;
    bool cpu_hotplug_legacy;
    AcpiCpuHotplug gpe_cpu;
    CPUHotplugState cpuhp_state;
} SW64PMState;

#define TYPE_SW64_PM "SW64_PM"

DECLARE_INSTANCE_CHECKER(SW64PMState, SW64_PM, TYPE_SW64_PM)

void sw64_pm_set_irq(void *opaque, int irq, int level);
#endif
