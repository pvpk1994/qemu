/*
 * QEMU CORE3 hardware system emulator.
 *
 * Copyright (c) 2018 Li Hainan
 *
 * This work is licensed under the GNU GPL license version 2 or later.
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

#define MAX_CPUS_CORE3 64
#define C3_UEFI_BIOS_NAME "c3-uefi-bios-sw"

static void core3_init(MachineState *machine)
{
    ram_addr_t ram_size = machine->ram_size;
    const char *kernel_filename = machine->kernel_filename;
    const char *kernel_cmdline = machine->kernel_cmdline;
    const char *initrd_filename = machine->initrd_filename;
    const char *hmcode_name = kvm_enabled() ? "core3-reset":"core3-hmcode";
    const char *bios_name = C3_UEFI_BIOS_NAME;
    BOOT_PARAMS *sunway_boot_params = g_new0(BOOT_PARAMS, 1);
    char *hmcode_filename;
    uint64_t hmcode_entry, kernel_entry;

    if (kvm_enabled())
        sw64_set_clocksource();

    core3_board_init(machine);

    sw64_set_ram_size(ram_size);
    sw64_clear_smp_rcb();

    hmcode_filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, hmcode_name);
    if (hmcode_filename == NULL) {
	error_report("no '%s' provided", hmcode_name);
	exit(1);
    }
    sw64_load_hmcode(hmcode_filename, &hmcode_entry);

    if (!kvm_enabled()) {
        CPUState *cpu;
        SW64CPU *sw64_cpu;
        CPU_FOREACH(cpu) {
            sw64_cpu = SW64_CPU(cpu);
            sw64_cpu->env.pc = hmcode_entry;
            sw64_cpu->env.hm_entry = hmcode_entry;
            sw64_cpu->env.csr[CID] = sw64_cpu->core_id;
            qemu_register_reset(sw64_cpu_reset, sw64_cpu);
        }
    }
    g_free(hmcode_filename);

    if (!kernel_filename)
	sw64_find_and_load_bios(bios_name);
    else {
        sw64_clear_uefi_bios();
        sw64_load_kernel(kernel_filename, &kernel_entry, kernel_cmdline);

        if (initrd_filename) {
            sw64_load_initrd(initrd_filename, sunway_boot_params);
        }
    }

    if (sw64_load_dtb(machine, sunway_boot_params) < 0) {
        exit(1);
    }

    rom_add_blob_fixed("sunway_boot_params", (sunway_boot_params), 0x48, 0x90A100);
}

static void core3_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "CORE3 BOARD";
    mc->init = core3_init;
    mc->block_default_type = IF_VIRTIO;
    mc->max_cpus = MAX_CPUS_CORE3;
    mc->no_cdrom = 1;
    mc->pci_allow_0_address = true;
    mc->reset = sw64_board_reset;
    mc->possible_cpu_arch_ids = sw64_possible_cpu_arch_ids;
    mc->cpu_index_to_instance_props = sw64_cpu_index_to_props;
    mc->default_cpu_type = SW64_CPU_TYPE_NAME("core3");
    mc->default_ram_id = "ram";
    mc->get_default_cpu_node_id = sw64_get_default_cpu_node_id;
}

static const TypeInfo core3_machine_info = {
    .name          = TYPE_CORE3_MACHINE,
    .parent        = TYPE_MACHINE,
    .class_init    = core3_machine_class_init,
    .instance_size = sizeof(CORE3MachineState),
};

static void core3_machine_init(void)
{
    type_register_static(&core3_machine_info);
}
type_init(core3_machine_init);
