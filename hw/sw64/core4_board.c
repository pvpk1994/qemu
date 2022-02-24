#include "qemu/osdep.h"
#include "qapi/error.h"
#include "cpu.h"
#include "hw/sw64/core.h"
#include "hw/sw64/sunway.h"
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
#include "hw/pci/msi.h"
#include "sysemu/device_tree.h"
#include "qemu/datadir.h"
#include "hw/sw64/gpio.h"

#define CORE4_MAX_CPUS_MASK             0x3ff
#define CORE4_CORES_SHIFT               10
#define CORE4_CORES_MASK                0x3ff
#define CORE4_THREADS_SHIFT             20
#define CORE4_THREADS_MASK              0xfff

#define DOMAIN_ID_SHIFT                 12
#define CORE_ID_SHIFT                   0

static unsigned long coreonlines[4];

static const MemMapEntry memmap[] = {
    [VIRT_BOOT_FLAG] =          {       0x820000,           0x20 },
    [VIRT_PCIE_MMIO] =          {     0xe0000000,     0x20000000 },
    [VIRT_MSI] =                { 0x8000fee00000,       0x100000 },
    [VIRT_SPBU] =               { 0x803000000000,      0x1000000 },
    [VIRT_SUNWAY_GED] =         { 0x803600000000,           0x20 },
    [VIRT_INTPU] =              { 0x803a00000000,       0x100000 },
    [VIRT_RTC] =                { 0x804910000000,            0x8 },
    [VIRT_FW_CFG] =             { 0x804920000000,           0x18 },
    [VIRT_GPIO] =               { 0x804930000000,   0x0000008000 },
    [VIRT_PCIE_IO_BASE] =       { 0x880000000000, 0x890000000000 },
    [VIRT_PCIE_PIO] =           { 0x880100000000,    0x100000000 },
    [VIRT_UART] =               { 0x8801000003f8,           0x10 },
    [VIRT_PCIE_CFG] =           { 0x880600000000,    0x100000000 },
    [VIRT_HIGH_PCIE_MMIO] =     { 0x888000000000,   0x8000000000 },
};

static const int irqmap[] = {
    [VIRT_UART] = 12,
    [VIRT_SUNWAY_GED] = 13,
    [VIRT_GPIO] = 15,
};

static void core4_virt_build_smbios(CORE4MachineState *core4ms)
{
    FWCfgState *fw_cfg = core4ms->fw_cfg;

    if (!fw_cfg)
	return;

    sw64_virt_build_smbios(fw_cfg);
}

static uint64_t spbu_read(void *opaque, hwaddr addr, unsigned size)
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
	    ret = (smp_threads & CORE4_THREADS_MASK) << CORE4_THREADS_SHIFT;
	    ret += (smp_cores & CORE4_CORES_MASK) << CORE4_CORES_SHIFT;
	    ret += max_cpus & CORE4_MAX_CPUS_MASK;
	}
	break;
    case 0x3a00:
        /* CLU_LV2_SEL_H */
        ret = 1;
        break;
    case 0x0680:
        /* INIT_CTL */
        ret = 0x3ae0007802c;
        break;
    case 0x0780:
        /* CORE_ONLINE */
        ret = convert_bit(max_cpus);
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

static void spbu_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
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

static const MemoryRegionOps spbu_ops = {
    .read = spbu_read,
    .write = spbu_write,
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
        if (((cpu_current->env.csr[II_REQ] >> 16) & 7) == 6)
            cpu_interrupt(qemu_get_cpu(val & 0x3f), CPU_INTERRUPT_IINM);
        else
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

static void create_fdt_clk(CORE4MachineState *c4ms)
{
    FILE *fp;
    char buff[64];
    char *nodename;
    unsigned long mclk_hz;
    MachineState *ms = MACHINE(c4ms);

    fp = fopen("/sys/kernel/debug/sw64/mclk_hz", "rb");
    if (fp == NULL) {
        fprintf(stderr, "%s: Failed to open file mclk_hz\n", __func__);
        return;
    }

    if (fgets(buff, 64, fp) == NULL) {
        fprintf(stderr, "%s: Error in reading mclk_hz\n", __func__);
        fclose(fp);
	fp = NULL;
	return;
    }

    mclk_hz = atoi(buff);
    fclose(fp);

    qemu_fdt_add_subnode(ms->fdt, "/soc/clocks");

    nodename = g_strdup_printf("/soc/clocks/mclk");
    qemu_fdt_add_subnode(ms->fdt, nodename);
    qemu_fdt_setprop_string(ms->fdt, nodename, "compatible", "sunway,mclk");
    qemu_fdt_setprop_cell(ms->fdt, nodename, "clock-frequency", mclk_hz);
    qemu_fdt_setprop_cell(ms->fdt, nodename, "#clock-cells", 0x0);
    qemu_fdt_setprop_string(ms->fdt, nodename, "clock-output-names", "mclk");
    g_free(nodename);

    nodename = g_strdup_printf("/soc/clocks/extclk");
    qemu_fdt_add_subnode(ms->fdt, nodename);
    qemu_fdt_setprop_string(ms->fdt, nodename, "compatible", "sunway,extclk");
    qemu_fdt_setprop_cell(ms->fdt, nodename, "clock-frequency", 0x0);
    qemu_fdt_setprop_cell(ms->fdt, nodename, "#clock-cells", 0x0);
    qemu_fdt_setprop_string(ms->fdt, nodename, "clock-output-names", "extclk");
    g_free(nodename);
}

static void core4_set_coreonline(int cpuid)
{
   set_bit(cpuid, coreonlines);
}

static int core4_get_stat_coreonline(int cpuid)
{
    int ret = test_bit(cpuid, coreonlines);

    clear_bit(cpuid, coreonlines);
    return ret;
}

static void core4_numa_set_coreonlines(MachineState *ms, int node,
                       unsigned int *logical_core_id, int *coreid_idx)
{
    unsigned int max_cpus = ms->smp.max_cpus;
    int i, cpu_node_id, shift = 0;

    for (i = 0; i < max_cpus; i++) {
        cpu_node_id = ms->possible_cpus->cpus[i].props.node_id;

        if (cpu_node_id == node) {
            core4_set_coreonline(shift);
            logical_core_id[*coreid_idx] = i;
            shift++;
            (*coreid_idx)++;
        }
    }
}

static void core4_get_cpu_to_rcid(MachineState *ms,
                unsigned long *__cpu_to_rcid)
{
    int nb_numa_nodes = ms->numa_state->num_nodes;
    unsigned long  rcid[MAX_CPUS_CORE4];
    unsigned int  logical_core_id[MAX_CPUS_CORE4];
    int i, j, coreid_idx = 0, cpuid = 0, idx = 0;

    for (i = 0; i < nb_numa_nodes; i++) {
        core4_numa_set_coreonlines(ms, i, logical_core_id, &coreid_idx);

        for (j = 0; j < MAX_CPUS_CORE4; j++) {
            if (core4_get_stat_coreonline(j)) {
                rcid[idx] = (i << DOMAIN_ID_SHIFT) | (j << CORE_ID_SHIFT);
                idx++;
            }
        }
    }

    for (i = 0; i < ms->smp.max_cpus; i++) {
        cpuid = logical_core_id[i];
        __cpu_to_rcid[cpuid] = rcid[i];
    }

    cpuid = ms->smp.max_cpus;
    while (cpuid < MAX_CPUS_CORE4) {
        __cpu_to_rcid[cpuid] = -1;
        cpuid++;
    }
}

static int core4_fdt_add_memory_node(void *fdt, hwaddr mem_base,
                                     hwaddr mem_len, int numa_node_id)
{
    char *nodename;
    int ret;

    nodename = g_strdup_printf("/memory@%" PRIx64, mem_base);
    qemu_fdt_add_subnode(fdt, nodename);
    qemu_fdt_setprop_string(fdt, nodename, "device_type", "memory");
    ret = qemu_fdt_setprop_sized_cells(fdt, nodename, "reg", 2, mem_base,
                                       2, mem_len);
    if (ret < 0) {
        goto out;
    }

    /* only set the NUMA ID if it is specified */
    if (numa_node_id >= 0) {
        ret = qemu_fdt_setprop_cell(fdt, nodename,
                                    "numa-node-id", numa_node_id);
    }
out:
    g_free(nodename);
    return ret;
}

static void core4_add_memory_node(MachineState *ms)
{
    hwaddr mem_len=0x0;
    hwaddr mem_start=0x0;
    int nb_numa_nodes = ms->numa_state->num_nodes;
    int i, rc;

    if (ms->numa_state != NULL && nb_numa_nodes > 0) {
        for (i = 0; i < nb_numa_nodes; i++) {
            mem_len = ms->numa_state->nodes[i].node_mem;
            if (!mem_len) {
                continue;
            }

            if (!i) {
                mem_start = 0x910000;
                mem_len -= mem_start;
            }

            rc = core4_fdt_add_memory_node(ms->fdt, mem_start, mem_len, i);
            if (rc < 0) {
                fprintf(stderr, "couldn't add /memory@%"PRIx64" node\n",
                            mem_start);
            }
            mem_start += mem_len;
        }
    }
}

static void core4_add_cpu_map(CORE4MachineState *c4ms)
{
    MachineState *ms = MACHINE(c4ms);
    int cpu;

    qemu_fdt_add_subnode(ms->fdt, "/cpus/cpu-map");

    for (cpu = ms->smp.max_cpus - 1; cpu >= 0; cpu--) {
        char *cpu_path = g_strdup_printf("/cpus/cpu@%d", cpu);
        char *map_path;

        if (ms->smp.threads > 1) {
                map_path = g_strdup_printf(
                    "/cpus/cpu-map/cluster%d/core%d/thread%d",
                    cpu / (ms->smp.cores * ms->smp.threads),
                    (cpu / ms->smp.threads) % ms->smp.cores,
                    cpu % ms->smp.threads);
            } else {
                map_path = g_strdup_printf(
                    "/cpus/cpu-map/cluster%d/core%d",
                    cpu / ms->smp.cores,
                    cpu % ms->smp.cores);
            }
            qemu_fdt_add_path(ms->fdt, map_path);
            qemu_fdt_setprop_phandle(ms->fdt, map_path, "cpu", cpu_path);

            g_free(map_path);
            g_free(cpu_path);
    }
}

static void core4_add_cpu_node(CORE4MachineState *c4ms)
{
    MachineState *ms = MACHINE(c4ms);
    unsigned long  __cpu_to_rcid[MAX_CPUS_CORE4];
    char *nodename;
    int cpu;

    qemu_fdt_add_subnode(ms->fdt, "/cpus");
    qemu_fdt_setprop_cell(ms->fdt, "/cpus", "#address-cells", 1);
    qemu_fdt_setprop_cell(ms->fdt, "/cpus", "#size-cells", 0x0);

    core4_get_cpu_to_rcid(ms, __cpu_to_rcid);
    for (cpu = ms->smp.max_cpus - 1; cpu >= 0; cpu--) {
        nodename = g_strdup_printf("/cpus/cpu@%d", cpu);

        qemu_fdt_add_subnode(ms->fdt, nodename);
        if (ms->possible_cpus->cpus[cpu].props.has_node_id) {
            qemu_fdt_setprop_cell(ms->fdt, nodename, "numa-node-id",
            ms->possible_cpus->cpus[cpu].props.node_id);
        }
        qemu_fdt_setprop_sized_cells(ms->fdt, nodename,
                        "sunway,boot_flag_address", 1, 0x0, 1,
                        c4ms->memmap[VIRT_BOOT_FLAG].base);
        qemu_fdt_setprop_cell(ms->fdt, nodename, "reg", __cpu_to_rcid[cpu]);
        qemu_fdt_setprop_string(ms->fdt, nodename, "device_type", "cpu");
        qemu_fdt_setprop_string(ms->fdt, nodename,
                        "compatible", "sunway,junzhang");
        qemu_fdt_setprop_cell(ms->fdt, nodename, "phandle",
                        qemu_fdt_alloc_phandle(ms->fdt));

        if (cpu < ms->smp.cpus) {
            qemu_fdt_setprop_cell(ms->fdt, nodename, "online-capable", 0);
            qemu_fdt_setprop_string(ms->fdt, nodename, "status", "okay");
        } else {
            qemu_fdt_setprop_cell(ms->fdt, nodename, "online-capable", 1);
            qemu_fdt_setprop_string(ms->fdt, nodename, "status", "disable");
        }

        g_free(nodename);
    }

    core4_add_cpu_map(c4ms);
}

static void core4_add_distance_map_node(MachineState *ms)
{
    int size;
    uint32_t *matrix;
    int idx, i, j;
    int nb_numa_nodes = ms->numa_state->num_nodes;

    if (nb_numa_nodes > 0 && ms->numa_state->have_numa_distance) {
        size = nb_numa_nodes * nb_numa_nodes * 3 * sizeof(uint32_t);
        matrix = g_malloc0(size);

        for (i = 0; i < nb_numa_nodes; i++) {
            for (j = 0; j < nb_numa_nodes; j++) {
                idx = (i * nb_numa_nodes + j) * 3;
                matrix[idx + 0] = cpu_to_be32(i);
            }
        }

        qemu_fdt_add_subnode(ms->fdt, "/distance-map");
        qemu_fdt_setprop_string(ms->fdt, "/distance-map", "compatible",
                                "numa-distance-map-v1");
        qemu_fdt_setprop(ms->fdt, "/distance-map", "distance-matrix",
                         matrix, size);
        g_free(matrix);
    }
}

static void core4_create_numa_fdt(CORE4MachineState *c4ms)
{
    MachineState *ms = MACHINE(c4ms);

    /* Add memory node information */
    core4_add_memory_node(ms);
    /* Add cpus node information */
    core4_add_cpu_node(c4ms);
    /* Add distance-map node information */
    core4_add_distance_map_node(ms);
}

static void create_fdt_misc_platform(CORE4MachineState *c4ms)
{
    char *nodename;
    MachineState *ms = MACHINE(c4ms);

    nodename = g_strdup_printf("/soc/misc_platform@0");
    qemu_fdt_add_subnode(ms->fdt, nodename);
    qemu_fdt_setprop_string(ms->fdt, nodename,
                            "compatible", "sunway,misc-platform");
    qemu_fdt_setprop_cell(ms->fdt, nodename, "numa-node-id", 0);
    qemu_fdt_setprop_sized_cells(ms->fdt, nodename, "sunway,spbu_base",
                                 2, c4ms->memmap[VIRT_SPBU].base);
    qemu_fdt_setprop_sized_cells(ms->fdt, nodename, "sunway,intpu_base",
                                 2, c4ms->memmap[VIRT_INTPU].base);
    g_free(nodename);
}

static void create_fdt_pcie_controller(CORE4MachineState *c4ms)
{
    MachineState *ms = MACHINE(c4ms);
    char *nodename;

    nodename = g_strdup_printf("/soc/pcie@8800");
    qemu_fdt_add_subnode(ms->fdt, nodename);
    qemu_fdt_setprop_string(ms->fdt, nodename, "compatible", "sunway,pcie");
    qemu_fdt_setprop_cell(ms->fdt, nodename, "#address-cells", 0x3);
    qemu_fdt_setprop_cell(ms->fdt, nodename, "#size-cells", 0x2);
    qemu_fdt_setprop_string(ms->fdt, nodename, "device_type", "pci");
    qemu_fdt_setprop_cell(ms->fdt, nodename, "numa-node-id", 0);
    qemu_fdt_setprop_sized_cells(ms->fdt, nodename, "reg",
                                 2, 0x880600000000, 2, 0x100000000);
    qemu_fdt_setprop_string(ms->fdt, nodename, "reg-names", "ecam");
    qemu_fdt_setprop_sized_cells(ms->fdt, nodename, "sunway,rc-config-base",
                                 1, 0x8805, 1, 0x0);
    qemu_fdt_setprop_sized_cells(ms->fdt, nodename, "sunway,ep-config-base",
                                 2, c4ms->memmap[VIRT_PCIE_CFG].base);
    qemu_fdt_setprop_sized_cells(ms->fdt, nodename, "sunway,ep-mem-32-base",
                                 2, c4ms->memmap[VIRT_PCIE_IO_BASE].base);
    qemu_fdt_setprop_sized_cells(ms->fdt, nodename, "sunway,ep-mem-64-base",
                                 2, c4ms->memmap[VIRT_HIGH_PCIE_MMIO].base);
    qemu_fdt_setprop_sized_cells(ms->fdt, nodename, "sunway,ep-io-base",
                                 2, c4ms->memmap[VIRT_PCIE_PIO].base);
    qemu_fdt_setprop_sized_cells(ms->fdt, nodename, "sunway,piu-ior0-base",
                                 2, 0x880200000000);
    qemu_fdt_setprop_sized_cells(ms->fdt, nodename, "sunway,piu-ior1-base",
                                 2, 0x880300000000);
    qemu_fdt_setprop_sized_cells(ms->fdt, nodename, "sunway,rc-index", 2, 0x0);
    qemu_fdt_setprop_sized_cells(ms->fdt, nodename, "sunway,pcie-io-base",
                                 2, c4ms->memmap[VIRT_PCIE_IO_BASE].base);
    qemu_fdt_setprop_sized_cells(ms->fdt, nodename, "bus-range", 2, 0xff);
    qemu_fdt_setprop_sized_cells(ms->fdt, nodename, "ranges",
                                 1, FDT_PCI_RANGE_IOPORT, 2, 0,
                                 2, c4ms->memmap[VIRT_PCIE_PIO].base,
                                 2, c4ms->memmap[VIRT_PCIE_PIO].size,

                                 1, FDT_PCI_RANGE_MMIO, 2,
                                 c4ms->memmap[VIRT_PCIE_MMIO].base,
                                 2, 0x8800e0000000, 2, 0x20000000,

                                 1, 0x43000000, 2,
                                 c4ms->memmap[VIRT_HIGH_PCIE_MMIO].base,
                                 2, c4ms->memmap[VIRT_HIGH_PCIE_MMIO].base,
                                 2, c4ms->memmap[VIRT_HIGH_PCIE_MMIO].size);

      qemu_fdt_setprop_string(ms->fdt, nodename, "status", "okay");
}

static void core4_create_fdt(CORE4MachineState *c4ms)
{
    uint32_t intc_phandle;
    MachineState *ms = MACHINE(c4ms);

    if (ms->dtb) {
        char *filename;

        filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, ms->dtb);
        if (!filename) {
            fprintf(stderr, "Couldn't open dtb file %s\n", ms->dtb);
            exit(1);
        }

        ms->fdt = load_device_tree(ms->dtb, &c4ms->fdt_size);
        if (!ms->fdt) {
            error_report("load_device_tree() failed");
            exit(1);
        }
        goto update_bootargs;
    } else {
        ms->fdt = create_device_tree(&c4ms->fdt_size);
        if (!ms->fdt) {
            error_report("create_device_tree() failed");
            exit(1);
        }

        qemu_fdt_setprop_string(ms->fdt, "/", "compatible", "sunway,junzhang");
        qemu_fdt_setprop_string(ms->fdt, "/", "model", "junzhang");
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
                                     2, c4ms->memmap[VIRT_UART].base,
                                     2, c4ms->memmap[VIRT_UART].size);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/serial0@8801",
                              "interrupt-parent", intc_phandle);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/serial0@8801",
                              "interrupts", c4ms->irqmap[VIRT_UART]);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/serial0@8801", "reg-shift", 0x0);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/serial0@8801",
                              "reg-io-width", 0x1);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/serial0@8801",
                              "clock-frequency", 20000000);
        qemu_fdt_setprop_string(ms->fdt, "/soc/serial0@8801",
                                "status", "okay");

        qemu_fdt_add_subnode(ms->fdt, "/soc/misc0@8036");
        qemu_fdt_setprop_cell(ms->fdt, "/soc/misc0@8036",
                              "#address-cells", 0x2);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/misc0@8036", "#size-cells", 0x2);
        qemu_fdt_setprop_string(ms->fdt, "/soc/misc0@8036",
                                "compatible", "sw6,sunway-ged");
        qemu_fdt_setprop_sized_cells(ms->fdt, "/soc/misc0@8036", "reg",
                                     2, c4ms->memmap[VIRT_SUNWAY_GED].base,
                                     2, c4ms->memmap[VIRT_SUNWAY_GED].size);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/misc0@8036",
                              "interrupt-parent", intc_phandle);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/misc0@8036",
                              "interrupts", c4ms->irqmap[VIRT_SUNWAY_GED]);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/misc0@8036", "reg-shift", 0x0);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/misc0@8036", "reg-io-width", 0x8);
        qemu_fdt_setprop_cell(ms->fdt, "/soc/misc0@8036",
                              "clock-frequency", 20000000);
        qemu_fdt_setprop_string(ms->fdt, "/soc/misc0@8036", "status", "okay");

        create_fdt_clk(c4ms);
        core4_create_numa_fdt(c4ms);
        create_fdt_misc_platform(c4ms);
        create_fdt_pcie_controller(c4ms);

    }

update_bootargs:
    if (ms->kernel_filename) {
        qemu_fdt_setprop_string(ms->fdt, "/chosen", "bootargs", ms->kernel_cmdline);
    } else {
        qemu_fdt_setprop_string(ms->fdt, "/chosen", "bootargs", "pcie_ports=native");
    }
}

static void core4_cpus_init(MachineState *ms)
{
    int i;
    const CPUArchIdList *possible_cpus;

    MachineClass *mc = MACHINE_GET_CLASS(ms);
    possible_cpus = mc->possible_cpu_arch_ids(ms);

    for (i = 0; i < ms->smp.cpus; i++) {
        sw64_new_cpu("core4-sw64-cpu", possible_cpus->cpus[i].arch_id,
		     &error_fatal);
    }
}

void sw64_pm_set_irq(void *opaque, int irq, int level)
{
    if (kvm_enabled()) {
        if (level == 0)
            return;
        kvm_set_irq(kvm_state, irq, level);
        return;
    }
}

void sw64_gpio_set_irq(void *opaque, int irq, int level)
{
    if (kvm_enabled()) {
        if (level == 0) {
            return;
        }
        kvm_set_irq(kvm_state, irq, level);
        return;
    }
}

static inline DeviceState *create_sw64_pm(CORE4MachineState *c4ms)
{
    DeviceState *dev;

    dev = qdev_try_new(TYPE_SW64_PM);

    if (!dev) {
        printf("failed to create sw64_pm,Unknown device TYPE_SW64_PM");
    }
    return dev;
}

static inline DeviceState *create_sw64_gpio(CORE4MachineState *c4ms)
{
    DeviceState *dev;

    dev = qdev_try_new(TYPE_SW64_GPIO);

    if (!dev) {
        printf("failed to create sw64_gpio,Unknown device TYPE_SW64_GPIO\n");
    }
    return dev;
}

static void sw64_create_device_memory(MachineState *machine, BoardState *bs)
{
    ram_addr_t ram_size = machine->ram_size;
    ram_addr_t device_mem_size;

    /* always allocate the device memory information */
    machine->device_memory = g_malloc0(sizeof(*machine->device_memory));

    /* initialize device memory address space */
    if (machine->ram_size < machine->maxram_size) {
        device_mem_size = machine->maxram_size - machine->ram_size;

        if (machine->ram_slots > ACPI_MAX_RAM_SLOTS) {
            printf("unsupported amount of memory slots: %"PRIu64,
                         machine->ram_slots);
            exit(EXIT_FAILURE);
        }

        if (QEMU_ALIGN_UP(machine->maxram_size,
                          TARGET_PAGE_SIZE) != machine->maxram_size) {
            printf("maximum memory size must by aligned to multiple of "
                         "%d bytes", TARGET_PAGE_SIZE);
            exit(EXIT_FAILURE);
        }

        machine->device_memory->base = ram_size;

        memory_region_init(&machine->device_memory->mr, OBJECT(bs),
                           "device-memory", device_mem_size);
        memory_region_add_subregion(get_system_memory(), machine->device_memory->base,
                                    &machine->device_memory->mr);
    }
}

void core4_board_init(MachineState *ms)
{
    CORE4MachineState *core4ms = CORE4_MACHINE(ms);
    DeviceState *dev = qdev_new(TYPE_CORE4_BOARD);
    BoardState *bs = CORE4_BOARD(dev);
    PCIHostState *phb = PCI_HOST_BRIDGE(dev);
    PCIBus *b;

    core4ms->memmap = memmap;
    core4ms->irqmap = irqmap;

    /* Create device tree */
    core4_create_fdt(core4ms);

    core4ms->acpi_dev = create_sw64_pm(core4ms);

    core4_cpus_init(ms);

    if (kvm_enabled()) {
        if (kvm_has_gsi_routing())
            msi_nonbroken = true;
    }
    else
        sw64_create_alarm_timer(ms, bs);

    sw64_create_device_memory(ms, bs);

    memory_region_add_subregion(get_system_memory(), 0, ms->ram);

    memory_region_init_io(&bs->io_spbu, NULL, &spbu_ops, bs, "io_spbu",
                          memmap[VIRT_SPBU].size);
    memory_region_add_subregion(get_system_memory(), memmap[VIRT_SPBU].base,
                                &bs->io_spbu);

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
                              &bs->mem_ep, &bs->io_ep, 0, 537, TYPE_PCI_BUS);
    phb->bus = b;
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    pci_bus_set_route_irq_fn(b, sw64_route_intx_pin_to_irq);
    memory_region_init_io(&bs->conf_piu0, OBJECT(bs), &sw64_pci_config_ops, b,
                          "pci0-ep-conf-io", memmap[VIRT_PCIE_CFG].size);
    memory_region_add_subregion(get_system_memory(),
                                memmap[VIRT_PCIE_CFG].base, &bs->conf_piu0);

    sw64_init_rtc_base_info();
    memory_region_init_io(&bs->io_rtc, OBJECT(bs), &rtc_ops, b,
                          "sw64-rtc", memmap[VIRT_RTC].size);
    memory_region_add_subregion(get_system_memory(), memmap[VIRT_RTC].base,
                                &bs->io_rtc);
    object_property_add_tm(OBJECT(core4ms), "rtc-time", rtc_get_time);

    sw64_create_pcie(bs, b, phb);

    core4ms->fw_cfg = sw64_create_fw_cfg(memmap[VIRT_FW_CFG].base,
                                         memmap[VIRT_FW_CFG].size);
    rom_set_fw(core4ms->fw_cfg);

    core4ms->gpio_dev = create_sw64_gpio(core4ms);

    core4ms->bus = phb->bus;

    if (sw64_is_acpi_enabled(core4ms)) {
        sw64_acpi_setup((SW64MachineState *)core4ms);
    }

    core4_virt_build_smbios(core4ms);
}

static Property core4_main_pci_host_props[] = {
    DEFINE_PROP_UINT32("ofw-addr", BoardState, ofw_addr, 0),
    DEFINE_PROP_END_OF_LIST()
};

static char *core4_main_ofw_unit_address(const SysBusDevice *dev)
{
    BoardState *s = CORE4_BOARD(dev);
    return g_strdup_printf("%x", s->ofw_addr);
}

static void core4_board_pcihost_class_init(ObjectClass *obj, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(obj);
    SysBusDeviceClass *sbc = SYS_BUS_DEVICE_CLASS(obj);

    dc->props_ = core4_main_pci_host_props;
    dc->fw_name = "pci";
    sbc->explicit_ofw_unit_address = core4_main_ofw_unit_address;
}

static const TypeInfo swboard_pcihost_info = {
    .name = TYPE_CORE4_BOARD,
    .parent = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(BoardState),
    .class_init = core4_board_pcihost_class_init,
};

static void swboard_register_types(void)
{
    type_register_static(&swboard_pcihost_info);
}

type_init(swboard_register_types)
