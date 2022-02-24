#include "qemu/osdep.h"
#include "qapi/error.h"
#include "cpu.h"
#include "hw/sw64/core.h"
#include "hw/hw.h"
#include "hw/boards.h"
#include "sysemu/sysemu.h"
#include "exec/address-spaces.h"
#include "hw/pci/pci_host.h"
#include "hw/pci/pci.h"
#include "hw/char/serial.h"
#include "hw/irq.h"
#include "net/net.h"
#include "hw/usb.h"
#include "sysemu/numa.h"
#include "sysemu/kvm.h"
#include "sysemu/cpus.h"
#include "hw/pci/msi.h"
#include "hw/sw64/sw64_iommu.h"
#include "hw/sw64/sunway.h"
#include "hw/loader.h"
#include "hw/nvram/fw_cfg.h"
#include "hw/firmware/smbios.h"
#include "sysemu/device_tree.h"
#include "qemu/datadir.h"

#define CORE3_MAX_CPUS_MASK		0x3ff
#define CORE3_CORES_SHIFT		10
#define CORE3_CORES_MASK		0x3ff
#define CORE3_THREADS_SHIFT		20
#define CORE3_THREADS_MASK		0xfff

static const MemMapEntry memmap[] = {
    [VIRT_PCIE_MMIO] =          {     0xe0000000,     0x20000000 },
    [VIRT_MSI] =                { 0x8000fee00000,       0x100000 },
    [VIRT_INTPU] =              { 0x802a00000000,       0x100000 },
    [VIRT_MCU] =                { 0x803000000000,      0x1000000 },
    [VIRT_RTC] =                { 0x804910000000,            0x8 },
    [VIRT_FW_CFG] =             { 0x804920000000,           0x18 },
    [VIRT_PCIE_IO_BASE] =       { 0x880000000000, 0x890000000000 },
    [VIRT_PCIE_PIO] =           { 0x880100000000,    0x100000000 },
    [VIRT_UART] =               { 0x8801000003f8,           0x10 },
    [VIRT_PCIE_CFG] =           { 0x880600000000,    0x100000000 },
    [VIRT_HIGH_PCIE_MMIO] =     { 0x888000000000,   0x8000000000 },
};

static const int irqmap[] = {
    [VIRT_UART] = 12,
    [VIRT_SUNWAY_GED] = 13,
};

static void core3_virt_build_smbios(CORE3MachineState *core3ms)
{
    FWCfgState *fw_cfg = core3ms->fw_cfg;

    if (!fw_cfg)
        return;

    sw64_virt_build_smbios(fw_cfg);
}

static uint64_t mcu_read(void *opaque, hwaddr addr, unsigned size)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    unsigned int smp_cpus = ms->smp.cpus;
    unsigned int smp_threads = ms->smp.threads;
    unsigned int smp_cores = ms->smp.cores;
    unsigned int max_cpus = ms->smp.max_cpus;
    uint64_t ret = 0;
    switch (addr) {
    case 0x0080:
    /* SMP_INFO */
	{
	    ret = (smp_threads & CORE3_THREADS_MASK) << CORE3_THREADS_SHIFT;
	    ret += (smp_cores & CORE3_CORES_MASK) << CORE3_CORES_SHIFT;
	    ret += max_cpus & CORE3_MAX_CPUS_MASK;
	}
	break;
    case 0x0680:
    /* INIT_CTL */
        ret = 0x3ae0000ddd9;
        break;
    case 0x0780:
    /* CORE_ONLINE */
        ret = convert_bit(smp_cpus);
        break;
    case 0x3780:
    /* MC_ONLINE */
        ret = convert_bit(smp_cpus);
        break;
    default:
        fprintf(stderr, "Unsupported MCU addr: 0x%04lx\n", addr);
        return -1;
    }
    return ret;
}

static void mcu_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
#ifdef CONFIG_DUMP_PRINTK
    uint64_t print_addr;
    uint32_t len;
    int i;

    if (kvm_enabled())
	return;

    if (addr == 0x40000) {
        print_addr = val & 0x7fffffff;
        len = (uint32_t)(val >> 32);
        uint8_t *buf;
        buf = malloc(len + 10);
        memset(buf, 0, len + 10);
        cpu_physical_memory_rw(print_addr, buf, len, 0);
        for (i = 0; i < len; i++)
            printf("%c", buf[i]);

        free(buf);
        return;
    }
#endif
}

static const MemoryRegionOps mcu_ops = {
    .read = mcu_read,
    .write = mcu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid =
        {
            .min_access_size = 8,
            .max_access_size = 8,
        },
    .impl =
        {
            .min_access_size = 8,
            .max_access_size = 8,
        },
};

static uint64_t intpu_read(void *opaque, hwaddr addr, unsigned size)
{
    uint64_t ret = 0;

    if (kvm_enabled())
	return ret;

    switch (addr) {
    case 0x180:
    /* LONGTIME */
        ret = qemu_clock_get_ns(QEMU_CLOCK_HOST) / 32;
        break;
    }
    return ret;
}

static void intpu_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned size)
{
    SW64CPU *cpu_current = SW64_CPU(current_cpu);

    if (kvm_enabled())
	return;

    switch (addr) {
    case 0x00:
        cpu_interrupt(qemu_get_cpu(val & 0x3f), CPU_INTERRUPT_II0);
        cpu_current->env.csr[II_REQ] &= ~(1 << 20);
        break;
    default:
        fprintf(stderr, "Unsupported IPU addr: 0x%04lx\n", addr);
        break;
    }
}

static const MemoryRegionOps intpu_ops = {
    .read = intpu_read,
    .write = intpu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid =
        {
            .min_access_size = 8,
            .max_access_size = 8,
        },
    .impl =
        {
            .min_access_size = 8,
            .max_access_size = 8,
        },
};

static void create_fdt_misc_platform(CORE3MachineState *c3ms)
{
    char *nodename;
    MachineState *ms = MACHINE(c3ms);

    nodename = g_strdup_printf("/soc/misc_platform@0");
    qemu_fdt_add_subnode(ms->fdt, nodename);
    qemu_fdt_setprop_string(ms->fdt, nodename,
                            "compatible", "sunway,misc-platform");
    qemu_fdt_setprop_cell(ms->fdt, nodename, "numa-node-id", 0);
    qemu_fdt_setprop_sized_cells(ms->fdt, nodename, "sunway,spbu_base",
                                 2, c3ms->memmap[VIRT_MCU].base);
    qemu_fdt_setprop_sized_cells(ms->fdt, nodename, "sunway,intpu_base",
                                 2, c3ms->memmap[VIRT_INTPU].base);
    g_free(nodename);
}

static void core3_create_fdt(CORE3MachineState *c3ms)
{
    uint32_t intc_phandle;
    MachineState *ms = MACHINE(c3ms);

    if (ms->dtb) {
        char *filename;

        filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, ms->dtb);
        if (!filename) {
            fprintf(stderr, "Couldn't open dtb file %s\n", ms->dtb);
            exit(1);
        }

        ms->fdt = load_device_tree(ms->dtb, &c3ms->fdt_size);
        if (!ms->fdt) {
            error_report("load_device_tree() failed");
            exit(1);
        }
    } else {
        ms->fdt = create_device_tree(&c3ms->fdt_size);
        if (!ms->fdt) {
            error_report("create_device_tree() failed");
            exit(1);
        }

        qemu_fdt_setprop_string(ms->fdt, "/", "compatible", "sunway,chip3");
        qemu_fdt_setprop_string(ms->fdt, "/", "model", "chip3");
        qemu_fdt_setprop_cell(ms->fdt, "/", "#address-cells", 0x2);
        qemu_fdt_setprop_cell(ms->fdt, "/", "#size-cells", 0x2);

        qemu_fdt_add_subnode(ms->fdt, "/chosen");

        qemu_fdt_add_subnode(ms->fdt, "/soc");
        qemu_fdt_setprop_string(ms->fdt, "/soc", "compatible", "simple-bus");
        qemu_fdt_setprop_cell(ms->fdt, "/soc", "#address-cells", 0x2);
        qemu_fdt_setprop_cell(ms->fdt, "/soc", "#size-cells", 0x2);
        qemu_fdt_setprop(ms->fdt, "/soc", "ranges", NULL, 0);

        intc_phandle = qemu_fdt_alloc_phandle(ms->fdt);
        qemu_fdt_add_subnode(ms->fdt, "/soc/interrupt-controller");
        qemu_fdt_setprop_string(ms->fdt, "/soc/interrupt-controller",
                                "compatible", "sw64,pintc_vt");
        qemu_fdt_setprop(ms->fdt, "/soc/interrupt-controller",
                         "interrupt-controller", NULL, 0);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/interrupt-controller",
                              "sw64,node", 0);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/interrupt-controller",
                              "sw64,irq-num", 16);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/interrupt-controller",
                              "sw64,ver", 0x1);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/interrupt-controller",
                              "#interrupt-cells", 0x1);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/interrupt-controller",
                              "phandle", intc_phandle);

        qemu_fdt_add_subnode(ms->fdt, "/soc/serial0@8801");
        qemu_fdt_setprop_cell(ms->fdt, "/soc/serial0@8801",
                              "#address-cells", 0x2);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/serial0@8801",
                              "#size-cells", 0x2);
        qemu_fdt_setprop_string(ms->fdt, "/soc/serial0@8801",
                                "compatible", "ns16550a");
        qemu_fdt_setprop_sized_cells(ms->fdt, "/soc/serial0@8801", "reg",
                                     2, c3ms->memmap[VIRT_UART].base,
                                     2, c3ms->memmap[VIRT_UART].size);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/serial0@8801",
                              "interrupt-parent", intc_phandle);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/serial0@8801",
                              "interrupts", c3ms->irqmap[VIRT_UART]);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/serial0@8801", "reg-shift", 0x0);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/serial0@8801",
                              "reg-io-width", 0x1);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/serial0@8801",
                              "clock-frequency", 24000000);
        qemu_fdt_setprop_string(ms->fdt, "/soc/serial0@8801",
                                "status", "okay");
    }
        create_fdt_misc_platform(c3ms);
}

static void core3_cpus_init(MachineState *ms)
{
    int i;
    const CPUArchIdList *possible_cpus;
    MachineClass *mc = MACHINE_GET_CLASS(ms);

    possible_cpus = mc->possible_cpu_arch_ids(ms);
    for (i = 0; i < ms->smp.cpus; i++) {
        sw64_new_cpu("core3-sw64-cpu", possible_cpus->cpus[i].arch_id, &error_fatal);
    }
}

void core3_board_init(MachineState *ms)
{
    CORE3MachineState *core3ms = CORE3_MACHINE(ms);
    DeviceState *dev = qdev_new(TYPE_CORE3_BOARD);
    BoardState *bs = CORE3_BOARD(dev);
    PCIHostState *phb = PCI_HOST_BRIDGE(dev);
    PCIBus *b;

    core3ms->memmap = memmap;
    core3ms->irqmap = irqmap;

    /* Create device tree */
    core3_create_fdt(core3ms);

    core3_cpus_init(ms);

    if (kvm_enabled()) {
        if (kvm_has_gsi_routing())
            msi_nonbroken = true;
    }
    else
	sw64_create_alarm_timer(ms, bs);

    memory_region_add_subregion(get_system_memory(), 0, ms->ram);

    memory_region_init_io(&bs->io_mcu, NULL, &mcu_ops, bs, "io_mcu",
                          memmap[VIRT_MCU].size);
    memory_region_add_subregion(get_system_memory(), memmap[VIRT_MCU].base,
                                &bs->io_mcu);

    memory_region_init_io(&bs->io_intpu, NULL, &intpu_ops, bs, "io_intpu",
                          memmap[VIRT_INTPU].size);
    memory_region_add_subregion(get_system_memory(), memmap[VIRT_INTPU].base,
                                &bs->io_intpu);

    memory_region_init_io(&bs->msi_ep, NULL, &msi_ops, bs, "msi_ep",
                          memmap[VIRT_MSI].size);
    memory_region_add_subregion(get_system_memory(), memmap[VIRT_MSI].base,
                                &bs->msi_ep);

    memory_region_init(&bs->mem_ep, OBJECT(bs), "pci0-mem",
                       memmap[VIRT_PCIE_IO_BASE].size);
    memory_region_add_subregion(get_system_memory(),
                                memmap[VIRT_PCIE_IO_BASE].base, &bs->mem_ep);

    memory_region_init_alias(&bs->mem_ep64, NULL, "mem_ep64", &bs->mem_ep,
                             memmap[VIRT_HIGH_PCIE_MMIO].base,
                             memmap[VIRT_HIGH_PCIE_MMIO].size);
    memory_region_add_subregion(get_system_memory(),
                                memmap[VIRT_HIGH_PCIE_MMIO].base, &bs->mem_ep64);

    memory_region_init_io(&bs->io_ep, OBJECT(bs), &sw64_pci_ignore_ops, NULL,
                          "pci0-io-ep", memmap[VIRT_PCIE_PIO].size);
    memory_region_add_subregion(get_system_memory(), memmap[VIRT_PCIE_PIO].base,
                                &bs->io_ep);

    b = pci_register_root_bus(dev, "pcie.0", sw64_board_set_irq,
                              sw64_board_map_irq, bs,
                              &bs->mem_ep, &bs->io_ep, 0, 537, TYPE_PCIE_BUS);
    phb->bus = b;
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    pci_bus_set_route_irq_fn(b, sw64_route_intx_pin_to_irq);
    memory_region_init_io(&bs->conf_piu0, OBJECT(bs), &sw64_pci_config_ops, b,
                          "pci0-ep-conf-io", memmap[VIRT_PCIE_CFG].size);
    memory_region_add_subregion(get_system_memory(), memmap[VIRT_PCIE_CFG].base,
                                &bs->conf_piu0);
    sw64_init_rtc_base_info();
    memory_region_init_io(&bs->io_rtc, OBJECT(bs), &rtc_ops, b,
                          "sw64-rtc", memmap[VIRT_RTC].size);
    memory_region_add_subregion(get_system_memory(), memmap[VIRT_RTC].base,
                                &bs->io_rtc);
    object_property_add_tm(OBJECT(core3ms), "rtc-time", rtc_get_time);
#ifdef CONFIG_SW64_VT_IOMMU
    sw64_vt_iommu_init(b);
#endif

    sw64_create_pcie(bs, b, phb);

    core3ms->fw_cfg = sw64_create_fw_cfg(memmap[VIRT_FW_CFG].base,
                                         memmap[VIRT_FW_CFG].size);
    rom_set_fw(core3ms->fw_cfg);

    core3_virt_build_smbios(core3ms);
}

static Property core3_main_pci_host_props[] = {
    DEFINE_PROP_UINT32("ofw-addr", BoardState, ofw_addr, 0),
    DEFINE_PROP_END_OF_LIST()
};

static char *core3_main_ofw_unit_address(const SysBusDevice *dev)
{
    BoardState *s = CORE3_BOARD(dev);
    return g_strdup_printf("%x", s->ofw_addr);
}

static void core3_board_pcihost_class_init(ObjectClass *obj, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(obj);
    SysBusDeviceClass *sbc = SYS_BUS_DEVICE_CLASS(obj);

    dc->props_ = core3_main_pci_host_props;
    dc->fw_name = "pci";
    sbc->explicit_ofw_unit_address = core3_main_ofw_unit_address;
}

static const TypeInfo swboard_pcihost_info = {
    .name = TYPE_CORE3_BOARD,
    .parent = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(BoardState),
    .class_init = core3_board_pcihost_class_init,
};

static void swboard_register_types(void)
{
    type_register_static(&swboard_pcihost_info);
}

type_init(swboard_register_types)
