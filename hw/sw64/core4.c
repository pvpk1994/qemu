/*
 * QEMU CORE4 hardware system emulator.
 */

#include "qemu/osdep.h"
#include "qemu/datadir.h"
#include "cpu.h"
#include "hw/hw.h"
#include "elf.h"
#include "hw/loader.h"
#include "hw/boards.h"
#include "qemu/error-report.h"
#include "sysemu/sysemu.h"
#include "sysemu/kvm.h"
#include "sysemu/reset.h"
#include "hw/char/serial.h"
#include "qemu/cutils.h"
#include "ui/console.h"
#include "hw/sw64/core.h"
#include "hw/sw64/sunway.h"
#include "sysemu/numa.h"
#include "hw/mem/pc-dimm.h"
#include "qapi/error.h"
#include "sysemu/device_tree.h"
#include "hw/core/cpu.h"
#include "hw/qdev-core.h"
#include "qapi/qapi-visit-common.h"

#define C4_UEFI_BIOS_NAME "c4-uefi-bios-sw"

static unsigned long cpu_masks[4];
static int hot_cpu_num;
int cpu_hot_id;

static void core4_init(MachineState *machine)
{
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    const CPUArchIdList *possible_cpus = mc->possible_cpu_arch_ids(machine);
    assert(possible_cpus->len);

    ram_addr_t ram_size = machine->ram_size;
    const char *kernel_filename = machine->kernel_filename;
    const char *kernel_cmdline = machine->kernel_cmdline;
    const char *initrd_filename = machine->initrd_filename;
    const char *hmcode_name = kvm_enabled() ? "core4-reset":"core4-hmcode";
    const char *bios_name = C4_UEFI_BIOS_NAME;
    BOOT_PARAMS *sunway_boot_params = g_new0(BOOT_PARAMS, 1);
    char *hmcode_filename;
    uint64_t hmcode_entry, kernel_entry;

    core4_board_init(machine);

    sw64_set_ram_size(ram_size);
    sw64_clear_smp_rcb();
    sw64_clear_kernel_print();

    hmcode_filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, hmcode_name);
    if (hmcode_filename == NULL) {
	error_report("no '%s' provided", hmcode_name);
        exit(1);
    }
    sw64_load_hmcode(hmcode_filename, &hmcode_entry);

    g_free(hmcode_filename);

    if (!kernel_filename)
        sw64_find_and_load_bios(bios_name);
    else {
        sw64_clear_uefi_bios();
        sw64_load_kernel(kernel_filename, &kernel_entry, kernel_cmdline);

        if (initrd_filename) {
            unsigned long initrd_end_va;

            sw64_load_initrd(initrd_filename, sunway_boot_params);
            initrd_end_va = sunway_boot_params->initrd_start +
                            (sunway_boot_params->initrd_size &
                            (~0xfff0000000000000UL));
            qemu_fdt_setprop_cell(machine->fdt, "/chosen", "linux,initrd-start",
                                  sunway_boot_params->initrd_start);
            qemu_fdt_setprop_cell(machine->fdt, "/chosen", "linux,initrd-end",
                                  initrd_end_va);
        }
    }

    if (sw64_load_dtb(machine, sunway_boot_params) < 0) {
        exit(1);
    }

    /* Retained for forward compatibility */
    rom_add_blob_fixed("sunway_boot_params", (sunway_boot_params), 0x48, 0x90A100);

    if (!kvm_enabled()) {
        CPUState *cpu;
        SW64CPU *sw64_cpu;
        CPU_FOREACH(cpu) {
            sw64_cpu = SW64_CPU(cpu);
            sw64_cpu->env.pc = hmcode_entry;
            sw64_cpu->env.hm_entry = hmcode_entry;
            sw64_cpu->env.csr[CID] = sw64_cpu->core_id;
            qemu_register_reset(sw64_cpu_reset, sw64_cpu);
            if (sw64_cpu->core_id == 0) {
                sw64_cpu->env.ir[16] = 0xA2024;
                sw64_cpu->env.ir[17] = dtb_start_c4;
            }
        }
    }
}

static void set_on_cpumask(int cpu_num)
{
    set_bit(cpu_num, cpu_masks);
}

static void set_off_cpumask(int cpu_num)
{
    clear_bit(cpu_num, cpu_masks);
}

int get_state_cpumask(int cpu_num)
{
    return test_bit(cpu_num, cpu_masks);
}

static HotplugHandler *sw64_get_hotplug_handler(MachineState *machine,
                                             DeviceState *dev)
{
    if (object_dynamic_cast(OBJECT(dev), TYPE_PC_DIMM))
        return HOTPLUG_HANDLER(machine);

    if (object_dynamic_cast(OBJECT(dev), TYPE_CPU)) {
        return HOTPLUG_HANDLER(machine);
    }

    return NULL;
}

static void core4_machine_device_pre_plug_cb(HotplugHandler *hotplug_dev,
                                      DeviceState *dev, Error **errp)
{
    MachineState *ms = MACHINE(hotplug_dev);
    Error *local_err = NULL;

    if (object_dynamic_cast(OBJECT(dev), TYPE_PC_DIMM))
        pc_dimm_pre_plug(PC_DIMM(dev), ms, NULL, &local_err);

    return;
}

static CPUArchId *sw64_find_cpu_slot(MachineState *ms, uint32_t id)
{
    if (id >= ms->possible_cpus->len) {
        return NULL;
    }
    if (hot_cpu_num < ms->smp.cpus) {
        ++hot_cpu_num;
    }
    set_on_cpumask(id);
    return &ms->possible_cpus->cpus[id];
}

static void sw64_cpu_plug(HotplugHandler *hotplug_dev, DeviceState *dev,
                         Error **errp)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    uint16_t smp_cpus = ms->smp.cpus;
    CPUArchId *found_cpu;
    HotplugHandlerClass *hhc;
    Error *local_err = NULL;
    CPUState *cs = NULL;
    SW64CPU *cpu = NULL;
    CORE4MachineState *pcms = CORE4_MACHINE(hotplug_dev);

    if (pcms->acpi_dev) {
        if (get_state_cpumask(cpu_hot_id)) {
            error_setg(&local_err, "error: Unable to add already online cpu!");
            return;
        }

        hhc = HOTPLUG_HANDLER_GET_CLASS(pcms->acpi_dev);
        hhc->plug(HOTPLUG_HANDLER(pcms->acpi_dev), dev, &local_err);
        if (local_err) {
            goto out;
        }
    }

    cs = CPU(dev);
    cpu = SW64_CPU(dev);
    if (hot_cpu_num < smp_cpus) {
        cs->cpu_index = hot_cpu_num;
        cpu->core_id = hot_cpu_num;
        found_cpu = sw64_find_cpu_slot(MACHINE(pcms), hot_cpu_num);
    } else {
        hot_cpu_num = smp_cpus;
        cs->cpu_index = cpu_hot_id;
        cpu->core_id = cpu_hot_id;
        found_cpu = sw64_find_cpu_slot(MACHINE(pcms), cpu_hot_id);
    }
    if (!found_cpu) {
        error_setg(&local_err, "error: No slot found for new hot add cpu!");
        return;
    }
    found_cpu->cpu = OBJECT(dev);
out:
    error_propagate(errp, local_err);
}

static void sw64_qdev_unrealize(DeviceState *dev)
{
    Error *err = NULL;
    object_property_set_bool(OBJECT(dev), "realized", false, &err);
}


static void sw64_cpu_unplug_request(HotplugHandler *hotplug_dev,
                                  DeviceState *dev, Error **errp)
{
    Error *local_err = NULL;
    SW64CPU *cpu = NULL;
    CPUState *cs = NULL;
    HotplugHandlerClass *hhc;
    CORE4MachineState *pcms = CORE4_MACHINE(hotplug_dev);

    if (!pcms->acpi_dev) {
        error_setg(&local_err, "CPU hot unplug not supported without ACPI");
        goto out;
    }

    cpu = SW64_CPU(dev);
    cpu_hot_id = cpu->core_id;
    cs = CPU(dev);
    cs->cpu_index = cpu_hot_id;

    if (!cpu_hot_id) {
        error_setg(&local_err, "Boot CPU is unpluggable");
        goto out;
    }

    if (!get_state_cpumask(cpu_hot_id) || !cpu_hot_id) {
        error_setg(&local_err, "error:"
            "Unable to delete already offline cpu and cpu 0 can not offline!");
        return;
    }

    hhc = HOTPLUG_HANDLER_GET_CLASS(pcms->acpi_dev);
    hhc->unplug_request(HOTPLUG_HANDLER(pcms->acpi_dev), dev, &local_err);

out:
    error_propagate(errp, local_err);
}

static void core4_machine_device_plug_cb(HotplugHandler *hotplug_dev,
                                      DeviceState *dev, Error **errp)
{
    MachineState *ms = MACHINE(hotplug_dev);
    CORE4MachineState *core4ms = CORE4_MACHINE(hotplug_dev);
    Error *local_err = NULL;

    if (object_dynamic_cast(OBJECT(dev), TYPE_PC_DIMM)) {
        pc_dimm_plug(PC_DIMM(dev), ms);
        hotplug_handler_plug(HOTPLUG_HANDLER(core4ms->acpi_dev),
                            dev, &local_err);
    }
    if (object_dynamic_cast(OBJECT(dev), TYPE_CPU))
        sw64_cpu_plug(hotplug_dev, dev, &local_err);
}

static void core4_machine_device_unplug_request_cb(HotplugHandler *hotplug_dev,
                                          DeviceState *dev, Error **errp)
{
    CORE4MachineState *core4ms = CORE4_MACHINE(hotplug_dev);
    Error *local_err = NULL;

    if (object_dynamic_cast(OBJECT(dev), TYPE_PC_DIMM)) {
        hotplug_handler_unplug_request(HOTPLUG_HANDLER(core4ms->acpi_dev),
                                      dev, &local_err);
    } else if (object_dynamic_cast(OBJECT(dev), TYPE_CPU)) {
        sw64_cpu_unplug_request(hotplug_dev, dev, &local_err);
    } else {
        error_setg(&local_err, "device unplug request for unsupported device"
                   " type: %s", object_get_typename(OBJECT(dev)));
    }
}

static void core4_machine_device_unplug_cb(HotplugHandler *hotplug_dev,
                                          DeviceState *dev, Error **errp)
{
    MachineState *ms = MACHINE(hotplug_dev);
    CORE4MachineState *core4ms = CORE4_MACHINE(hotplug_dev);
    Error *local_err = NULL;
    CPUArchId *found_cpu;

    if (object_dynamic_cast(OBJECT(dev), TYPE_PC_DIMM)) {
        hotplug_handler_unplug(HOTPLUG_HANDLER(core4ms->acpi_dev),
                              dev, &local_err);
        if (local_err) {
            goto out;
        }
        pc_dimm_unplug(PC_DIMM(dev), MACHINE(ms));
    } else if (object_dynamic_cast(OBJECT(dev), TYPE_CPU)) {
        hotplug_handler_unplug(HOTPLUG_HANDLER(core4ms->acpi_dev),
                        dev, &local_err);
        if (local_err) {
            goto out;
        }
        found_cpu = sw64_find_cpu_slot(MACHINE(core4ms), cpu_hot_id);
        found_cpu->cpu = NULL;
        sw64_qdev_unrealize(dev);
        set_off_cpumask(cpu_hot_id);
    } else {
        error_setg(&local_err, "device unplug for unsupported device"
                " type: %s", object_get_typename(OBJECT(dev)));
    }

out:
    return;
}

bool sw64_is_acpi_enabled(CORE4MachineState *c4ms)
{
    return c4ms->acpi != ON_OFF_AUTO_OFF;
}

static void core4_get_acpi(Object *obj, Visitor *v, const char *name,
                          void *opaque, Error **errp)
{
    CORE4MachineState *c4ms = CORE4_MACHINE(obj);
    OnOffAuto acpi = c4ms->acpi;

    visit_type_OnOffAuto(v, name, &acpi, errp);
}

static void core4_set_acpi(Object *obj, Visitor *v, const char *name,
                          void *opaque, Error **errp)
{
    CORE4MachineState *c4ms = CORE4_MACHINE(obj);

    visit_type_OnOffAuto(v, name, &c4ms->acpi, errp);
}

static void core4_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    HotplugHandlerClass *hc = HOTPLUG_HANDLER_CLASS(oc);

    mc->desc = "CORE4 BOARD";
    mc->init = core4_init;
    mc->block_default_type = IF_VIRTIO;
    mc->max_cpus = MAX_CPUS_CORE4;
    mc->no_cdrom = 1;
    mc->pci_allow_0_address = true;
    mc->reset = sw64_board_reset;
    mc->possible_cpu_arch_ids = sw64_possible_cpu_arch_ids;
    mc->default_cpu_type = SW64_CPU_TYPE_NAME("core4");
    mc->default_ram_id = "ram";
    mc->cpu_index_to_instance_props = sw64_cpu_index_to_props;
    mc->get_default_cpu_node_id = sw64_get_default_cpu_node_id;
    mc->get_hotplug_handler = sw64_get_hotplug_handler;
    mc->has_hotpluggable_cpus = true;
    hc->pre_plug = core4_machine_device_pre_plug_cb;
    hc->plug = core4_machine_device_plug_cb;
    hc->unplug_request = core4_machine_device_unplug_request_cb;
    hc->unplug = core4_machine_device_unplug_cb;
    mc->auto_enable_numa = true;

    object_class_property_add(oc, "acpi", "OnOffAuto",
                       core4_get_acpi, core4_set_acpi,
                       NULL, NULL);
    object_class_property_set_description(oc, "acpi",
                       "Enable ACPI");
}

static void core4_machine_initfn(Object *obj)
{
    CORE4MachineState *c4ms = CORE4_MACHINE(obj);

    c4ms->oem_id = g_strndup(SW_ACPI_BUILD_APPNAME6, 6);
    c4ms->oem_table_id = g_strndup(SW_ACPI_BUILD_APPNAME8, 8);
    c4ms->acpi = ON_OFF_AUTO_AUTO;
}

static const TypeInfo core4_machine_info = {
    .name = TYPE_CORE4_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(CORE4MachineState),
    .class_size = sizeof(CORE4MachineClass),
    .class_init = core4_machine_class_init,
    .instance_init = core4_machine_initfn,
    .interfaces = (InterfaceInfo[]) {
         { TYPE_HOTPLUG_HANDLER },
         { }
    },
};

static void core4_machine_init(void)
{
    type_register_static(&core4_machine_info);
}

type_init(core4_machine_init)
