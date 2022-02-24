/* Support for generating ACPI tables and passing them to Guests
 *
 * SW64 ACPI generation
 *
 * Copyright (C) 2023 Wang Yuanheng <wangyuanheng@wxiat.com>
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bitmap.h"
#include "hw/core/cpu.h"
#include "target/sw64/cpu.h"
#include "hw/acpi/acpi-defs.h"
#include "hw/acpi/acpi.h"
#include "hw/nvram/fw_cfg.h"
#include "hw/acpi/bios-linker-loader.h"
#include "hw/acpi/aml-build.h"
#include "hw/acpi/utils.h"
#include "hw/acpi/pci.h"
#include "hw/acpi/memory_hotplug.h"
#include "hw/acpi/generic_event_device.h"
#include "hw/acpi/tpm.h"
#include "hw/pci/pcie_host.h"
#include "hw/acpi/aml-build.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci-host/gpex.h"
#include "hw/sw64/core.h"
#include "hw/platform-bus.h"
#include "sysemu/numa.h"
#include "sysemu/reset.h"
#include "sysemu/tpm.h"
#include "kvm_sw64.h"
#include "migration/vmstate.h"
#include "hw/acpi/ghes.h"
#include "hw/sw64/gpio.h"
#include "hw/irq.h"
#include "sysemu/runstate.h"

#define SW64_PCIE_IRQMAP 16
#define SW64_MCU_GSI_BASE 64
#define ACPI_BUILD_TABLE_SIZE             0x20000

static void acpi_dsdt_add_uart(Aml *scope, const MemMapEntry *uart_memmap,
                               uint32_t uart_irq)
{
    Aml *method, *dev, *crs;
    Aml *pkg;
    Aml *pkg0, *pkg1, *pkg2, *pkg3;

    dev = aml_device("COM0");
    aml_append(dev, aml_name_decl("_HID", aml_string("PNP0501")));
    aml_append(dev, aml_name_decl("_UID", aml_int(0)));

    crs = aml_resource_template();
    aml_append(crs,
        aml_qword_memory(AML_POS_DECODE, AML_MIN_FIXED, AML_MAX_FIXED,
                         AML_NON_CACHEABLE, AML_READ_WRITE,
                         0, uart_memmap->base,
                         uart_memmap->base + uart_memmap->size - 1,
                         0, uart_memmap->size));
    aml_append(crs, aml_interrupt(AML_CONSUMER, AML_LEVEL, AML_ACTIVE_HIGH,
                                  AML_SHARED, &uart_irq, 1));
    aml_append(dev, aml_name_decl("_CRS", crs));

    pkg0 = aml_package(0x2);
    aml_append(pkg0, aml_string("clock-frequency"));
    aml_append(pkg0, aml_int(20000000));

    pkg1 = aml_package(0x2);
    aml_append(pkg1, aml_string("reg-io-width"));
    aml_append(pkg1, aml_int(0x1));

    pkg2 = aml_package(0x2);
    aml_append(pkg2, aml_string("reg-shift"));
    aml_append(pkg2, aml_int(0x0));

    pkg3 = aml_package(0x3);
    aml_append(pkg3, pkg0);
    aml_append(pkg3, pkg1);
    aml_append(pkg3, pkg2);

    pkg = aml_package(0x2);
    aml_append(pkg, aml_touuid("DAFFD814-6EBA-4D8C-8A91-BC9BBF4AA301"));
    aml_append(pkg, pkg3);

    aml_append(dev, aml_name_decl("_DSD", pkg));    /* Device-Specific Data */
    aml_append(scope, dev);

    method = aml_method("_STA", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_return(aml_int(0xF)));
    aml_append(dev, method);
}

static void acpi_dsdt_add_sunway_ged(Aml *scope, const MemMapEntry *ged_memmap,
                               uint32_t ged_irq)
{
    Aml *method, *dev, *crs;
    Aml *pkg, *pkg0;

    dev = aml_device("SMHP");
    aml_append(dev, aml_name_decl("_HID", aml_string("SUNW1000")));
    aml_append(dev, aml_name_decl("_UID", aml_int(0)));

    crs = aml_resource_template();
    aml_append(crs,
        aml_qword_memory(AML_POS_DECODE, AML_MIN_FIXED, AML_MAX_FIXED,
                         AML_NON_CACHEABLE, AML_READ_WRITE,
                         0, ged_memmap->base,
                         ged_memmap->base + ged_memmap->size - 1,
                         0, ged_memmap->size));
    aml_append(crs, aml_interrupt(AML_CONSUMER, AML_LEVEL, AML_ACTIVE_HIGH,
                                  AML_SHARED, &ged_irq, 1));
    aml_append(dev, aml_name_decl("_CRS", crs));

    pkg0 = aml_package(0x2);
    aml_append(pkg0, aml_string("clock-frequency"));
    aml_append(pkg0, aml_int(20000000));

    pkg = aml_package(0x2);
    aml_append(pkg, aml_touuid("DAFFD814-6EBA-4D8C-8A91-BC9BBF4AA301"));
    aml_append(pkg, pkg0);

    aml_append(dev, aml_name_decl("_DSD", pkg));
    aml_append(scope, dev);

    method = aml_method("_STA", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_return(aml_int(0xF)));
    aml_append(dev, method);
}


static void acpi_dsdt_add_fw_cfg(Aml *scope, const MemMapEntry *fw_cfg_memmap)
{
    Aml *dev = aml_device("FWCF");
    aml_append(dev, aml_name_decl("_HID", aml_string("QEMU0002")));
    /* device present, functioning, decoding, not shown in UI */
    aml_append(dev, aml_name_decl("_STA", aml_int(0xB)));
    aml_append(dev, aml_name_decl("_CCA", aml_int(1)));

    Aml *crs = aml_resource_template();
    aml_append(crs,
        aml_qword_memory(AML_POS_DECODE, AML_MIN_FIXED, AML_MAX_FIXED,
                         AML_NON_CACHEABLE, AML_READ_WRITE,
                         0, fw_cfg_memmap->base,
                         fw_cfg_memmap->base + fw_cfg_memmap->size - 1,
                         0, fw_cfg_memmap->size));
    aml_append(dev, aml_name_decl("_CRS", crs));
    aml_append(scope, dev);
}

static void acpi_dsdt_add_pci_osc(Aml *dev)
{
    Aml *method, *UUID, *ifctx, *ifctx1, *elsectx;

    /* Declare an _OSC (OS Control Handoff) method */
    aml_append(dev, aml_name_decl("SUPP", aml_int(0)));
    aml_append(dev, aml_name_decl("CTRL", aml_int(0)));
    method = aml_method("_OSC", 4, AML_NOTSERIALIZED);
    aml_append(method,
        aml_create_dword_field(aml_arg(3), aml_int(0), "CDW1"));

    /* PCI Firmware Specification 3.0
     * 4.5.1. _OSC Interface for PCI Host Bridge Devices
     * The _OSC interface for a PCI/PCI-X/PCI Express hierarchy is
     * identified by the Universal Unique IDentifier (UUID)
     * 33DB4D5B-1FF7-401C-9657-7441C03DD766
     */
    UUID = aml_touuid("33DB4D5B-1FF7-401C-9657-7441C03DD766");
    ifctx = aml_if(aml_equal(aml_arg(0), UUID));
    aml_append(ifctx,
        aml_create_dword_field(aml_arg(3), aml_int(4), "CDW2"));
    aml_append(ifctx,
        aml_create_dword_field(aml_arg(3), aml_int(8), "CDW3"));
    aml_append(ifctx, aml_store(aml_name("CDW2"), aml_name("SUPP")));
    aml_append(ifctx, aml_store(aml_name("CDW3"), aml_name("CTRL")));
    /*
     * Allow OS control for all 5 features:
     * PCIeHotplug SHPCHotplug PME AER PCIeCapability.
     */
    aml_append(ifctx, aml_and(aml_name("CTRL"), aml_int(0x1F),
                              aml_name("CTRL")));

    ifctx1 = aml_if(aml_lnot(aml_equal(aml_arg(1), aml_int(0x1))));
    aml_append(ifctx1, aml_or(aml_name("CDW1"), aml_int(0x08),
                              aml_name("CDW1")));
    aml_append(ifctx, ifctx1);

    ifctx1 = aml_if(aml_lnot(aml_equal(aml_name("CDW3"), aml_name("CTRL"))));
    aml_append(ifctx1, aml_or(aml_name("CDW1"), aml_int(0x10),
                              aml_name("CDW1")));
    aml_append(ifctx, ifctx1);

    aml_append(ifctx, aml_store(aml_name("CTRL"), aml_name("CDW3")));
    aml_append(ifctx, aml_return(aml_arg(3)));
    aml_append(method, ifctx);

    elsectx = aml_else();
    aml_append(elsectx, aml_or(aml_name("CDW1"), aml_int(4),
                               aml_name("CDW1")));
    aml_append(elsectx, aml_return(aml_arg(3)));
    aml_append(method, elsectx);
    aml_append(dev, method);
}

static void acpi_dsdt_add_pci(Aml *scope, uint32_t irq, SW64MachineState *vms)
{
    CrsRangeEntry *entry;
    Aml *dev, *rbuf, *method, *crs;
    CrsRangeSet crs_range_set;
    int i;
    Aml *pkg0, *pkg1, *pkg;

    struct GPEXConfig cfg = {
        .mmio32 = vms->memmap[VIRT_PCIE_MMIO],
	.mmio64 = vms->memmap[VIRT_HIGH_PCIE_MMIO],
        .pio    = vms->memmap[VIRT_PCIE_PIO],
        .ecam   = vms->memmap[VIRT_PCIE_CFG],
        .irq    = irq,
        .bus    = vms->bus,
    };

    /* PCI0 */
    dev = aml_device("PCI0");
    aml_append(dev, aml_name_decl("_HID", aml_eisaid("PNP0A08")));
    aml_append(dev, aml_name_decl("_CID", aml_eisaid("PNP0A03")));
    aml_append(dev, aml_name_decl("_SEG", aml_int(0)));
    aml_append(dev, aml_name_decl("_BBN", aml_int(0)));
    method = aml_method("_PXM", 0, AML_SERIALIZED);
    aml_append(method, aml_return(aml_int(0x0)));
    aml_append(dev, method);

    method = aml_method("_STA", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_return(aml_int(0xF)));
    aml_append(dev, method);

    pkg1 = aml_package(0x9);

    pkg0 = aml_package(0x2);
    aml_append(pkg0, aml_string("sunway,rc-config-base"));
    aml_append(pkg0, aml_int(0x0));
    aml_append(pkg1, pkg0);

    pkg0 = aml_package(0x2);
    aml_append(pkg0, aml_string("sunway,ep-config-base"));
    aml_append(pkg0, aml_int(vms->memmap[VIRT_PCIE_CFG].base));
    aml_append(pkg1, pkg0);

    pkg0 = aml_package(0x2);
    aml_append(pkg0, aml_string("sunway,ep-mem-32-base"));
    aml_append(pkg0, aml_int(vms->memmap[VIRT_PCIE_IO_BASE].base
                             | vms->memmap[VIRT_PCIE_MMIO].base));
    aml_append(pkg1, pkg0);

    pkg0 = aml_package(0x2);
    aml_append(pkg0, aml_string("sunway,ep-mem-64-base"));
    aml_append(pkg0, aml_int(vms->memmap[VIRT_HIGH_PCIE_MMIO].base));
    aml_append(pkg1, pkg0);

    pkg0 = aml_package(0x2);
    aml_append(pkg0, aml_string("sunway,ep-io-base"));
    aml_append(pkg0, aml_int(vms->memmap[VIRT_PCIE_PIO].base));
    aml_append(pkg1, pkg0);

    pkg0 = aml_package(0x2);
    aml_append(pkg0, aml_string("sunway,piu-ior0-base"));
    aml_append(pkg0, aml_int(0x0));
    aml_append(pkg1, pkg0);

    pkg0 = aml_package(0x2);
    aml_append(pkg0, aml_string("sunway,piu-ior1-base"));
    aml_append(pkg0, aml_int(0x0));
    aml_append(pkg1, pkg0);

    pkg0 = aml_package(0x2);
    aml_append(pkg0, aml_string("sunway,rc-index"));
    aml_append(pkg0, aml_int(0));
    aml_append(pkg1, pkg0);

    pkg0 = aml_package(0x2);
    aml_append(pkg0, aml_string("sunway,pcie-io-base"));
    aml_append(pkg0, aml_int(vms->memmap[VIRT_PCIE_IO_BASE].base));
    aml_append(pkg1, pkg0);

    pkg = aml_package(0x2);
    aml_append(pkg, aml_touuid("DAFFD814-6EBA-4D8C-8A91-BC9BBF4AA301"));
    aml_append(pkg, pkg1);
    aml_append(dev, aml_name_decl("_DSD", pkg));

    rbuf = aml_resource_template();
    aml_append(dev, aml_name_decl("CRS0", rbuf));
    aml_append(rbuf,
        aml_word_bus_number(AML_MIN_FIXED, AML_MAX_FIXED, AML_POS_DECODE,
                            0x0000, 0x0000, 0xFF, 0x0000, 0x100));
    crs_range_set_init(&crs_range_set);
    if (cfg.mmio32.size) {
        crs_replace_with_free_ranges(crs_range_set.mem_ranges,
                                     cfg.mmio32.base,
                                     cfg.mmio32.base + cfg.mmio32.size - 1);
        for (i = 0; i < crs_range_set.mem_ranges->len; i++) {
            entry = g_ptr_array_index(crs_range_set.mem_ranges, i);
            aml_append(rbuf,
                aml_dword_memory(AML_POS_DECODE, AML_MIN_FIXED, AML_MAX_FIXED,
                                 AML_NON_CACHEABLE, AML_READ_WRITE, 0x0000,
                                 entry->base, entry->limit,
                                 0x0000, entry->limit - entry->base + 1));
        }
    }

    if (cfg.pio.size) {
        crs_replace_with_free_ranges(crs_range_set.io_ranges,
                                     cfg.pio.base,
                                     cfg.pio.base + cfg.pio.size - 1);
        for (i = 0; i < crs_range_set.io_ranges->len; i++) {
            entry = g_ptr_array_index(crs_range_set.io_ranges, i);
            aml_append(rbuf,
                aml_qword_io(AML_MIN_FIXED, AML_MAX_FIXED, AML_POS_DECODE,
                             AML_ENTIRE_RANGE, 0x0000, entry->base,
                             entry->limit, 0x0000,
                             entry->limit - entry->base + 1));
        }
    }
    if (cfg.mmio64.size) {
        crs_replace_with_free_ranges(crs_range_set.mem_64bit_ranges,
                                     cfg.mmio64.base,
                                     cfg.mmio64.base + cfg.mmio64.size - 1);
        for (i = 0; i < crs_range_set.mem_64bit_ranges->len; i++) {
            entry = g_ptr_array_index(crs_range_set.mem_64bit_ranges, i);
            aml_append(rbuf,
                aml_qword_memory(AML_POS_DECODE, AML_MIN_FIXED, AML_MAX_FIXED,
                                 AML_NON_CACHEABLE, AML_READ_WRITE, 0x0000,
                                 entry->base,
                                 entry->limit, 0x0000,
                                 entry->limit - entry->base + 1));
        }
    }

    method = aml_method("_CRS", 0, AML_SERIALIZED);
    aml_append(method, aml_return(rbuf));
    aml_append(dev, method);
    acpi_dsdt_add_pci_osc(dev);

    /* RES0 */
    Aml *dev_res0 = aml_device("%s", "RES0");
    aml_append(dev_res0, aml_name_decl("_HID", aml_string("PNP0C02")));
    crs = aml_resource_template();
    aml_append(crs,
        aml_qword_memory(AML_POS_DECODE, AML_MIN_FIXED, AML_MAX_FIXED,
                         AML_NON_CACHEABLE, AML_READ_WRITE, 0x0000,
                         cfg.ecam.base,
                         cfg.ecam.base + cfg.ecam.size - 1,
                         0x0000,
                         cfg.ecam.size));
    method = aml_method("_STA", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_return(aml_int(0xF)));
    aml_append(dev, dev_res0);
    aml_append(scope, dev);
    crs_range_set_free(&crs_range_set);
}

static void
build_srat(GArray *table_data, BIOSLinker *linker, SW64MachineState *vms)
{
    int i;
    uint64_t mem_base;
    MachineClass *mc = MACHINE_GET_CLASS(vms);
    MachineState *ms = MACHINE(vms);
    const CPUArchIdList *cpu_list = mc->possible_cpu_arch_ids(ms);
    AcpiTable table = { .sig = "SRAT", .rev = 3, .oem_id = vms->oem_id,
                        .oem_table_id = vms->oem_table_id };

    acpi_table_begin(&table, table_data);
    build_append_int_noprefix(table_data, 1, 4);          /* Reserved */
    build_append_int_noprefix(table_data, 0, 8);          /* Reserved */

    for (i = 0; i < cpu_list->len; ++i) {
        uint32_t node_id = cpu_list->cpus[i].props.node_id;

        /* CPU Affinity Structure */
        build_append_int_noprefix(table_data, 2, 1);       /* Type  */
        build_append_int_noprefix(table_data, 24, 1);      /* Length */
        build_append_int_noprefix(table_data, 0, 2);       /* Reserved */
        /* Proximity Domain */
        build_append_int_noprefix(table_data, node_id, 4);
        build_append_int_noprefix(table_data, i, 4);       /* APIC ID */
        /* Flags */
        build_append_int_noprefix(table_data, 1 /* Enabled */, 4);
        build_append_int_noprefix(table_data, 0, 4);       /* Clock Domain */
        build_append_int_noprefix(table_data, 0, 4);       /* Reserved */
    }

    /* Memory Affinity Structure */
    mem_base = 0;
    for (i = 0; i < ms->numa_state->num_nodes; ++i) {
        if (ms->numa_state->nodes[i].node_mem > 0) {
            build_srat_memory(table_data, mem_base,
                              ms->numa_state->nodes[i].node_mem, i,
                              MEM_AFFINITY_ENABLED);
            mem_base += ms->numa_state->nodes[i].node_mem;
        }
    }

    if (ms->device_memory) {
        build_srat_memory(table_data, ms->device_memory->base,
                          memory_region_size(&ms->device_memory->mr),
                          ms->numa_state->num_nodes - 1,
                          MEM_AFFINITY_HOTPLUGGABLE | MEM_AFFINITY_ENABLED);
    }

    acpi_table_end(linker, &table);
}

static void build_append_mcu_intc(GArray *table_data)
{
    build_append_int_noprefix(table_data, 0x0, 1);     /* Type: MCU INTC */
    build_append_int_noprefix(table_data, 1, 1);       /* Status */
    build_append_int_noprefix(table_data, 0, 2);       /* Reserved */
    build_append_int_noprefix(table_data, 0xf0000001, 4);/* Hardware ID */
    build_append_int_noprefix(table_data, 0, 8);       /* Address Base */
    build_append_int_noprefix(table_data, 0, 4);       /* Size */
    build_append_int_noprefix(table_data, SW64_MCU_GSI_BASE, 4);  /* GSI Base */
    build_append_int_noprefix(table_data, 16, 4);      /* GSI Count */
    build_append_int_noprefix(table_data, 0, 4);       /* Cascade Vector */
}

static void
build_madt(GArray *table_data, BIOSLinker *linker, SW64MachineState *vms)
{
    int i;
    const MemMapEntry *memmap = vms->memmap;
    MachineState *ms = MACHINE(vms);
    unsigned int num_cpus = ms->smp.max_cpus;
    uint32_t flags, enabled, virt, online_capable;
    AcpiTable table = { .sig = "APIC", .rev = 4, .oem_id = vms->oem_id,
                        .oem_table_id = vms->oem_table_id };

    acpi_table_begin(&table, table_data);

    /* Local Interrupt Controller Address */
    build_append_int_noprefix(table_data, 0, 4);
    /* Flags */
    build_append_int_noprefix(table_data, 0, 4);

    /* CINTC Structure */
    for (i = 0; i < num_cpus; i++) {
        SW64CPU *sw64cpu = SW64_CPU(qemu_get_cpu(i));
        enabled = sw64cpu ? 1 : 0;
        online_capable = !enabled << 1;
        virt = 0x1 << 2;
        flags = virt | online_capable | enabled;

        build_append_int_noprefix(table_data, 0x80, 1); /* Type */
        build_append_int_noprefix(table_data, 28, 1);   /* Length */
        build_append_int_noprefix(table_data, 0x2, 1);  /* Version  */
        build_append_int_noprefix(table_data, 0, 1);    /* Reserved */
        build_append_int_noprefix(table_data, flags, 4);/* Flags */
        build_append_int_noprefix(table_data, 0, 4);    /* Reserved */
        build_append_int_noprefix(table_data, i, 4);    /* Hardware ID */
        build_append_int_noprefix(table_data, 0, 4);    /* ACPI Processor UID */

        /* Boot Flag Address */
        if (i) {
            build_append_int_noprefix(table_data, 0, 8);
        } else {
            build_append_int_noprefix(table_data,
                                      memmap[VIRT_BOOT_FLAG].base, 8);
        }
    }

    /* PINTC Structure */
    build_append_int_noprefix(table_data, 0x81, 1);      /* Type */
    build_append_int_noprefix(table_data, 60, 1);        /* Length */
    build_append_int_noprefix(table_data, 0x2, 1);       /* Version */
    build_append_int_noprefix(table_data, 0, 1);         /* Reserved */
    /* Flags */
    build_append_int_noprefix(table_data, 0x3 /* Enabled && Virtual */, 4);
    build_append_int_noprefix(table_data, 0, 4);         /* Node */
    build_append_int_noprefix(table_data, 0, 8);         /* Address Base */
    build_append_int_noprefix(table_data, 0, 4);         /* Size*/
    /* Number of sub Interrupt Controller, only support MCU INTC now */
    build_append_int_noprefix(table_data, 1, 4);         /* N */

    /* Sub PINTC Structure[0]: MCU INTC */
    build_append_mcu_intc(table_data);

    /* MSIC Structure */
    build_append_int_noprefix(table_data, 0x82, 1);       /* Type */
    build_append_int_noprefix(table_data, 56, 1);         /* Length */
    build_append_int_noprefix(table_data, 0x2, 1);        /* Version */
    build_append_int_noprefix(table_data, 0, 1);          /* Reserved */
    build_append_int_noprefix(table_data, 0xf2000001, 4); /* Hardware ID */
    /* Flags */
    build_append_int_noprefix(table_data, 0x3 /* Enabled && Virtual */, 4);
    build_append_int_noprefix(table_data, 0, 8);      /* Address Base */
    build_append_int_noprefix(table_data, 0, 4);      /* Size*/
    build_append_int_noprefix(table_data, 0, 4);      /* Cascade Vector */
    build_append_int_noprefix(table_data, 0, 4);      /* Node */
    build_append_int_noprefix(table_data, 0, 4);      /* RC */
    build_append_int_noprefix(table_data, 256, 4);    /* Number of Interrupt */
    build_append_int_noprefix(table_data,
                              memmap[VIRT_MSI].base, 8);  /* Message Address */
    build_append_int_noprefix(table_data, 0, 8);          /* Reserved[2] */

    acpi_table_end(linker, &table);
}

static void acpi_dsdt_add_gpio(Aml *scope, const MemMapEntry *gpio_memmap,
                                           uint32_t gpio_irq)
{
    Aml *method, *dev, *crs, *dev1;
    Aml *pkg, *pkg0, *pkg1, *pkg2, *pkg3;

    dev = aml_device("GPI0");
    aml_append(dev, aml_name_decl("_HID", aml_string("SUNW0002")));
    aml_append(dev, aml_name_decl("_UID", aml_int(0)));

    crs = aml_resource_template();
    aml_append(crs,
        aml_qword_memory(AML_POS_DECODE, AML_MIN_FIXED, AML_MAX_FIXED,
                         AML_NON_CACHEABLE, AML_READ_WRITE,
                         0, gpio_memmap->base,
                         gpio_memmap->base + gpio_memmap->size - 1,
                         0, gpio_memmap->size));
    aml_append(crs, aml_interrupt(AML_CONSUMER, AML_LEVEL, AML_ACTIVE_HIGH,
                                  AML_SHARED, &gpio_irq, 1));
    aml_append(dev, aml_name_decl("_CRS", crs));

    method = aml_method("_STA", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_return(aml_int(0xF)));
    aml_append(dev, method);


   Aml *aei = aml_resource_template();
   const uint32_t pin_list[1] = {0};
   aml_append(aei, aml_gpio_int(AML_CONSUMER, AML_LEVEL, AML_ACTIVE_HIGH,
                                 AML_EXCLUSIVE, AML_PULL_DOWN, 0, pin_list, 1,
                                 "GPI0", NULL, 0));
    aml_append(dev, aml_name_decl("_AEI", aei));

    method = aml_method("_L00", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_notify(aml_name(ACPI_POWER_BUTTON_DEVICE),
                                  aml_int(0x80)));
    aml_append(dev, method);


    dev1 = aml_device("GP00");
    aml_append(dev1, aml_name_decl("_UID", aml_int(0)));

    pkg0 = aml_package(0x2);
    aml_append(pkg0, aml_string("compatible"));
    aml_append(pkg0, aml_string("snps,dw-apb-gpio-port"));

    pkg1 = aml_package(0x2);
    aml_append(pkg1, aml_string("reg"));
    aml_append(pkg1, aml_int(0x0));

    pkg2 = aml_package(0x2);
    aml_append(pkg2, aml_string("snps,nr-gpios"));
    aml_append(pkg2, aml_int(0x8));

    pkg3 = aml_package(0x3);
    aml_append(pkg3, pkg0);
    aml_append(pkg3, pkg1);
    aml_append(pkg3, pkg2);

    pkg = aml_package(0x2);
    aml_append(pkg, aml_touuid("DAFFD814-6EBA-4D8C-8A91-BC9BBF4AA301"));
    aml_append(pkg, pkg3);

    aml_append(dev1, aml_name_decl("_DSD", pkg));

    aml_append(dev, dev1);
    aml_append(scope, dev);

}

static void acpi_dsdt_add_misc_platform(Aml *scope, SW64MachineState *vms)
{
    Aml *method, *dev;
    Aml *pkg, *pkg0, *pkg1;

    dev = aml_device("MIS0");
    aml_append(dev, aml_name_decl("_HID", aml_string("SUNW0200")));
    aml_append(dev, aml_name_decl("_UID", aml_int(0)));

    pkg1 = aml_package(0x2);

    pkg0 = aml_package(0x2);
    aml_append(pkg0, aml_string("sunway,spbu_base"));
    aml_append(pkg0, aml_int(vms->memmap[VIRT_SPBU].base));
    aml_append(pkg1, pkg0);

    pkg0 = aml_package(0x2);
    aml_append(pkg0, aml_string("sunway,intpu_base"));
    aml_append(pkg0, aml_int(vms->memmap[VIRT_INTPU].base));
    aml_append(pkg1, pkg0);

    pkg = aml_package(0x2);
    aml_append(pkg, aml_touuid("DAFFD814-6EBA-4D8C-8A91-BC9BBF4AA301"));
    aml_append(pkg, pkg1);
    aml_append(dev, aml_name_decl("_DSD", pkg));

    method = aml_method("_STA", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_return(aml_int(0xF)));

    method = aml_method("_PXM", 0, AML_SERIALIZED);
    aml_append(method, aml_return(aml_int(0x0)));
    aml_append(dev, method);

    aml_append(scope, dev);
}

/* DSDT */
static void
build_dsdt(GArray *table_data, BIOSLinker *linker, SW64MachineState *vms)
{
    Aml *scope, *dsdt;
    const MemMapEntry *memmap = vms->memmap;
    const int *irqmap = vms->irqmap;
    AcpiTable table = { .sig = "DSDT", .rev = 2, .oem_id = vms->oem_id,
                        .oem_table_id = vms->oem_table_id };

    acpi_table_begin(&table, table_data);
    dsdt = init_aml_allocator();

    /* When booting the VM with UEFI, UEFI takes ownership of the RTC hardware.
     * While UEFI can use libfdt to disable the RTC device node in the DTB that
     * it passes to the OS, it cannot modify AML. Therefore, we won't generate
     * the RTC ACPI device at all when using UEFI.
     */
    scope = aml_scope("\\_SB");

    acpi_dsdt_add_uart(scope, &memmap[VIRT_UART],
                       (irqmap[VIRT_UART] + SW64_MCU_GSI_BASE));

    acpi_dsdt_add_sunway_ged(scope, &memmap[VIRT_SUNWAY_GED],
                       (irqmap[VIRT_SUNWAY_GED] + SW64_MCU_GSI_BASE));

    acpi_dsdt_add_fw_cfg(scope, &memmap[VIRT_FW_CFG]);
    acpi_dsdt_add_pci(scope, SW64_PCIE_IRQMAP, vms);

    acpi_dsdt_add_gpio(scope, &memmap[VIRT_GPIO],
                      (irqmap[VIRT_GPIO] + SW64_MCU_GSI_BASE));

    acpi_dsdt_add_power_button(scope);

    acpi_dsdt_add_misc_platform(scope, vms);

    aml_append(dsdt, scope);

    /* copy AML table into ACPI tables blob */
    g_array_append_vals(table_data, dsdt->buf->data, dsdt->buf->len);

    acpi_table_end(linker, &table);
    free_aml_allocator();
}

typedef
struct AcpiBuildState {
    /* Copy of table in RAM (for patching). */
    MemoryRegion *table_mr;
    MemoryRegion *rsdp_mr;
    MemoryRegion *linker_mr;
    /* Is table patched? */
    bool patched;
} AcpiBuildState;

static void acpi_align_size(GArray *blob, unsigned align)
{
    /*
     * Align size to multiple of given size. This reduces the chance
     * we need to change size in the future (breaking cross version migration).
     */
    g_array_set_size(blob, ROUND_UP(acpi_data_len(blob), align));
}

static
void sw64_acpi_build(SW64MachineState *vms, AcpiBuildTables *tables)
{
    GArray *table_offsets;
    unsigned dsdt, xsdt;
    GArray *tables_blob = tables->table_data;
    MachineState *ms = MACHINE(vms);

    table_offsets = g_array_new(false, true /* clear */,
                                        sizeof(uint32_t));

    bios_linker_loader_alloc(tables->linker,
                             ACPI_BUILD_TABLE_FILE, tables_blob,
                             64, false /* high memory */);

    /* DSDT is pointed to by FADT */
    dsdt = tables_blob->len;
    build_dsdt(tables_blob, tables->linker, vms);

    /* FADT MADT MCFG is pointed to by XSDT*/
    acpi_add_table(table_offsets, tables_blob);
    {
        AcpiFadtData fadt = {
            .rev = 5,
            .minor_ver = 1,
            .flags = 1 << ACPI_FADT_F_HW_REDUCED_ACPI,
            .xdsdt_tbl_offset = &dsdt,
        };
        build_fadt(tables_blob, tables->linker, &fadt, vms->oem_id,
                   vms->oem_table_id);
    }

    acpi_add_table(table_offsets, tables_blob);
    build_madt(tables_blob, tables->linker, vms);

    acpi_add_table(table_offsets, tables_blob);
    build_pptt(tables_blob, tables->linker, ms,
               vms->oem_id, vms->oem_table_id);

    acpi_add_table(table_offsets, tables_blob);
    {
        AcpiMcfgInfo mcfg = {
           .base = vms->memmap[VIRT_PCIE_CFG].base,
           .size = vms->memmap[VIRT_PCIE_CFG].size,
        };
        build_mcfg(tables_blob, tables->linker, &mcfg, vms->oem_id,
                   vms->oem_table_id);
    }

    /* NUMA support */
    if (ms->numa_state->num_nodes > 0) {
        acpi_add_table(table_offsets, tables_blob);
        build_srat(tables_blob, tables->linker, vms);

        if (ms->numa_state->have_numa_distance) {
            acpi_add_table(table_offsets, tables_blob);
            build_slit(tables_blob, tables->linker, ms, vms->oem_id,
                       vms->oem_table_id);
        }
    }

    /* XSDT is pointed to by RSDP */
    xsdt = tables_blob->len;
    build_xsdt(tables_blob, tables->linker, table_offsets, vms->oem_id,
               vms->oem_table_id);

    /* RSDP is in FSEG memory, so allocate it separately */
    {
        AcpiRsdpData rsdp_data = {
            .revision = 2,
            .oem_id = vms->oem_id,
            .xsdt_tbl_offset = &xsdt,
            .rsdt_tbl_offset = NULL,
        };
        build_rsdp(tables->rsdp, tables->linker, &rsdp_data);
    }

    /*
     * The align size is 128, warn if 64k is not enough therefore
     * the align size could be resized.
     */
    if (tables_blob->len > ACPI_BUILD_TABLE_SIZE / 2) {
        warn_report("ACPI table size %u exceeds %d bytes,"
                    " migration may not work",
                    tables_blob->len, ACPI_BUILD_TABLE_SIZE / 2);
        error_printf("Try removing CPUs, NUMA nodes, memory slots"
                     " or PCI bridges.");
    }
    acpi_align_size(tables_blob, ACPI_BUILD_TABLE_SIZE);


    /* Cleanup memory that's no longer used. */
    g_array_free(table_offsets, true);
}

static void acpi_ram_update(MemoryRegion *mr, GArray *data)
{
    uint32_t size = acpi_data_len(data);

    /* Make sure RAM size is correct - in case it got changed
     * e.g. by migration */
    memory_region_ram_resize(mr, size, &error_abort);

    memcpy(memory_region_get_ram_ptr(mr), data->data, size);
    memory_region_set_dirty(mr, 0, size);
}

static void sw64_acpi_build_update(void *build_opaque)
{
    AcpiBuildState *build_state = build_opaque;
    AcpiBuildTables tables;

    /* No state to update or already patched? Nothing to do. */
    if (!build_state || build_state->patched) {
        return;
    }
    build_state->patched = true;

    acpi_build_tables_init(&tables);

    sw64_acpi_build((SW64MachineState *)MACHINE(qdev_get_machine()), &tables);

    acpi_ram_update(build_state->table_mr, tables.table_data);
    acpi_ram_update(build_state->rsdp_mr, tables.rsdp);
    acpi_ram_update(build_state->linker_mr, tables.linker->cmd_blob);

    acpi_build_tables_cleanup(&tables, true);
}

static void sw64_acpi_build_reset(void *build_opaque)
{
    AcpiBuildState *build_state = build_opaque;
    build_state->patched = false;
}

static const VMStateDescription vmstate_sw64_acpi_build = {
    .name = "sw64_acpi_build",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_BOOL(patched, AcpiBuildState),
        VMSTATE_END_OF_LIST()
    },
};

static void sw64_powerdown_req(Notifier *n, void *opaque)
{
    SW64MachineState *s = container_of(n, SW64MachineState, powerdown_notifier);

    if (s->gpio_dev) {
        SW64GPIOState *gpio = SW64_GPIO(s->gpio_dev);
        gpio->intsr = 0x1;
        qemu_irq_pulse(gpio->irq[0]);
    } else {
        printf("error: no gpio device found!\n");
    }

}

void sw64_acpi_setup(SW64MachineState *vms)
{
    AcpiBuildTables tables;
    AcpiBuildState *build_state;

    if (!vms->fw_cfg) {
        return;
    }

    build_state = g_malloc0(sizeof *build_state);

    acpi_build_tables_init(&tables);

    sw64_acpi_build(vms, &tables);
    /* Now expose it all to Guest */
    build_state->table_mr = acpi_add_rom_blob(sw64_acpi_build_update,
                                              build_state, tables.table_data,
                                              ACPI_BUILD_TABLE_FILE);
    assert(build_state->table_mr != NULL);

    build_state->linker_mr = acpi_add_rom_blob(sw64_acpi_build_update,
                                               build_state,
                                               tables.linker->cmd_blob,
                                               ACPI_BUILD_LOADER_FILE);
    build_state->rsdp_mr = acpi_add_rom_blob(sw64_acpi_build_update,
                                             build_state, tables.rsdp,
                                             ACPI_BUILD_RSDP_FILE);

    qemu_register_reset(sw64_acpi_build_reset, build_state);
    vms->powerdown_notifier.notify = sw64_powerdown_req;
    qemu_register_powerdown_notifier(&vms->powerdown_notifier);
    sw64_acpi_build_reset(build_state);
    vmstate_register(NULL, 0, &vmstate_sw64_acpi_build, build_state);

    /* Cleanup tables but don't free the memory: we track it
     * in build_state.
     */
    acpi_build_tables_cleanup(&tables, false);
}
