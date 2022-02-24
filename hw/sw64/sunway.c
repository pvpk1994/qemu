/*
 * QEMU SUNWAY syetem helper.
 *
 * Copyright (c) 2023 Lu Feifei
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/datadir.h"
#include "qapi/error.h"
#include "cpu.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "elf.h"
#include "hw/loader.h"
#include "hw/boards.h"
#include "qemu/error-report.h"
#include "sysemu/sysemu.h"
#include "sysemu/kvm.h"
#include "sysemu/reset.h"
#include "sysemu/rtc.h"
#include "hw/char/serial.h"
#include "hw/pci/msi.h"
#include "hw/firmware/smbios.h"
#include "hw/nvram/fw_cfg.h"
#include "qemu/cutils.h"
#include "ui/console.h"
#include "hw/sw64/core.h"
#include "hw/sw64/sunway.h"
#include "sysemu/numa.h"
#include "net/net.h"
#include "sysemu/device_tree.h"
#include <libfdt.h>

#define SW_PIN_TO_IRQ 16
#define SW_FDT_BASE 0x2d00000ULL
#define SW_BIOS_BASE 0x2f00000ULL
#define SW_INITRD_BASE 0x3000000UL

static uint64_t base_rtc;
static uint64_t last_update;
unsigned long dtb_start_c4;

void sw64_init_rtc_base_info(void)
{
    struct tm tm;
    qemu_get_timedate(&tm, 0);
    base_rtc = mktimegm(&tm);
    last_update = get_clock_realtime() / NANOSECONDS_PER_SECOND;
}

static uint64_t rtc_read(void *opaque, hwaddr addr, unsigned size)
{
    uint64_t val;
    uint64_t guest_clock = get_clock_realtime() / NANOSECONDS_PER_SECOND;

    val = base_rtc + guest_clock - last_update;

    return val;
}

static void rtc_write(void *opaque, hwaddr addr, uint64_t val,
	       unsigned size)
{
}

const MemoryRegionOps rtc_ops = {
    .read = rtc_read,
    .write = rtc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid =
	{
	    .min_access_size = 1,
	    .max_access_size = 8,
	},
    .impl =
	{
	    .min_access_size = 1,
	    .max_access_size = 8,
	},
};

static uint64_t ignore_read(void *opaque, hwaddr addr, unsigned size)
{
    return 1;
}

static void ignore_write(void *opaque, hwaddr addr, uint64_t v, unsigned size)
{
}

const MemoryRegionOps sw64_pci_ignore_ops = {
    .read = ignore_read,
    .write = ignore_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid =
	{
	    .min_access_size = 1,
	    .max_access_size = 8,
	},
    .impl =
	{
	    .min_access_size = 1,
	    .max_access_size = 8,
	},
};

static uint64_t config_read(void *opaque, hwaddr addr, unsigned size)
{
    PCIBus *b = opaque;
    uint32_t trans_addr = 0;

    trans_addr |= ((addr >> 16) & 0xffff) << 8;
    trans_addr |= (addr & 0xff);

    return pci_data_read(b, trans_addr, size);
}

static void config_write(void *opaque, hwaddr addr, uint64_t val,
		         unsigned size)
{
    PCIBus *b = opaque;
    uint32_t trans_addr = 0;

    trans_addr |= ((addr >> 16) & 0xffff) << 8;
    trans_addr |= (addr & 0xff);

    pci_data_write(b, trans_addr, val, size);
}

const MemoryRegionOps sw64_pci_config_ops = {
    .read = config_read,
    .write = config_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid =
        {
	    .min_access_size = 1,
	    .max_access_size = 8,
	},
    .impl =
	{
	    .min_access_size = 1,
	    .max_access_size = 8,
	},
};

static MemTxResult msi_read(void *opaque, hwaddr addr,
		            uint64_t *data, unsigned size,
			    MemTxAttrs attrs)
{
    return MEMTX_OK;
}

MemTxResult msi_write(void *opaque, hwaddr addr,
                      uint64_t value, unsigned size,
                      MemTxAttrs attrs)
{
    int ret = 0;
    MSIMessage msg = {};

    if (!kvm_enabled())
	return MEMTX_DECODE_ERROR;

    msg.address = (uint64_t) addr + 0x8000fee00000;
    msg.data = (uint32_t) value;

    ret = kvm_irqchip_send_msi(kvm_state, msg);
    if (ret < 0) {
	fprintf(stderr, "KVM: injection failed, MSI lost (%s)\n",
		strerror(-ret));
    }

    return MEMTX_OK;
}

const MemoryRegionOps msi_ops = {
    .read_with_attrs = msi_read,
    .write_with_attrs = msi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid =
	{
	    .min_access_size = 1,
	    .max_access_size = 8,
	},
    .impl =
	{
	    .min_access_size = 1,
	    .max_access_size = 8,
	},
};

uint64_t cpu_sw64_virt_to_phys(void *opaque, uint64_t addr)
{
    return addr &= ~0xffffffff80000000 ;
}

CpuInstanceProperties
sw64_cpu_index_to_props(MachineState *ms, unsigned cpu_index)
{
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    const CPUArchIdList *possible_cpus = mc->possible_cpu_arch_ids(ms);

    assert(cpu_index < possible_cpus->len);
    return possible_cpus->cpus[cpu_index].props;
}

int64_t sw64_get_default_cpu_node_id(const MachineState *ms, int idx)
{
    int nb_numa_nodes = ms->numa_state->num_nodes;
    return idx % nb_numa_nodes;
}

const CPUArchIdList *sw64_possible_cpu_arch_ids(MachineState *ms)
{
    int i;
    unsigned int max_cpus = ms->smp.max_cpus;

    if (ms->possible_cpus) {
        /*
         * make sure that max_cpus hasn't changed since the first use, i.e.
         * -smp hasn't been parsed after it
        */
        assert(ms->possible_cpus->len == max_cpus);
        return ms->possible_cpus;
    }

    ms->possible_cpus = g_malloc0(sizeof(CPUArchIdList) +
                                  sizeof(CPUArchId) * max_cpus);
    ms->possible_cpus->len = max_cpus;
    for (i = 0; i < ms->possible_cpus->len; i++) {
        ms->possible_cpus->cpus[i].type = ms->cpu_type;
        ms->possible_cpus->cpus[i].vcpus_count = 1;
        ms->possible_cpus->cpus[i].arch_id = i;

        ms->possible_cpus->cpus[i].props.has_thread_id = true;
        ms->possible_cpus->cpus[i].props.thread_id = i % ms->smp.threads;

        ms->possible_cpus->cpus[i].props.has_core_id = true;
        ms->possible_cpus->cpus[i].props.core_id =
                                   i / ms->smp.threads % ms->smp.cores;

        ms->possible_cpus->cpus[i].props.has_socket_id = true;
        ms->possible_cpus->cpus[i].props.socket_id =
                                   i / (ms->smp.cores * ms->smp.threads);
    }

    return ms->possible_cpus;
}

void sw64_cpu_reset(void *opaque)
{
    SW64CPU *cpu = opaque;

    if (!kvm_enabled())
        cpu_reset(CPU(cpu));

    return;
}

void sw64_set_clocksource(void)
{
    FILE *fp;
    unsigned long mclk;
    char buff[64];

    fp = fopen("/sys/kernel/debug/sw64/mclk", "rb");
    if (fp == NULL) {
        printf("Failed to open file mclk.\n");
        return;
    }

    if (fgets(buff, 64, fp) == NULL) {
        printf("Error in reading mclk.\n");
        fclose(fp);
	fp = NULL;
	return;
    }

    mclk = atoi(buff);
    fclose(fp);
    rom_add_blob_fixed("mclk", (char *)&mclk, 0x8, 0x908001);
}

void sw64_board_reset(MachineState *state, ShutdownCause reason)
{
    qemu_devices_reset(reason);
}

void sw64_set_ram_size(ram_addr_t ram_size)
{
    ram_addr_t buf;

    if (kvm_enabled())
	buf = ram_size;
    else
	buf = ram_size | (1UL << 63);

    rom_add_blob_fixed("ram_size", (char *)&buf, 0x8, 0x2040);

    return;
}

void sw64_clear_uefi_bios(void)
{
    unsigned long uefi_bios[1] = {0};

    /* Clear the first 8 bytes of UEFI BIOS to ensure the correctness
     * of reset process.
     * */
    rom_add_blob_fixed("uefi_bios", uefi_bios, 0x8, SW_BIOS_BASE);

    return;
}

void sw64_clear_smp_rcb(void)
{
    unsigned long smp_rcb[4] = {0};

    /* Clear the smp_rcb fields to ensure the correctness of reset process. */
    rom_add_blob_fixed("smp_rcb", smp_rcb, 0x20, 0x820000);

    return;
}

void sw64_clear_kernel_print(void)
{
    unsigned long kernel_print[2048] = {0};

    /*
     * Clear the memory where the kernel is printed when the vm reboots
     * will make debugging easier.
     */
    rom_add_blob_fixed("kernel_print", kernel_print, 0x4000, 0x700000);

    return;
}

void sw64_load_hmcode(const char *hmcode_filename, uint64_t *hmcode_entry)
{
    long size;

    size = load_elf(hmcode_filename, NULL, cpu_sw64_virt_to_phys, NULL,
		    hmcode_entry, NULL, NULL, NULL, 0, EM_SW64, 0, 0);
    if (size < 0) {
	error_report("could not load hmcode: '%s'", hmcode_filename);
	exit(1);
    }

    return;
}

void sw64_find_and_load_bios(const char *bios_name)
{
    char *uefi_filename;
    long size;

    uefi_filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (uefi_filename == NULL) {
        error_report("no virtual bios provided");
	exit(1);
    }

    size = load_image_targphys(uefi_filename, SW_BIOS_BASE, -1);
    if (size < 0) {
	error_report("could not load virtual bios: '%s'", uefi_filename);
	exit(1);
    }

    g_free(uefi_filename);
    return;
}

void sw64_load_kernel(const char *kernel_filename, uint64_t *kernel_entry,
		      const char *kernel_cmdline)
{
    long size;
    uint64_t param_offset;

    size = load_elf(kernel_filename, NULL, cpu_sw64_virt_to_phys, NULL,
		    kernel_entry, NULL, NULL, NULL, 0, EM_SW64, 0, 0);
    if (size < 0) {
	error_report("could not load kernel '%s'", kernel_filename);
	exit(1);
    }

    if (kernel_cmdline) {
        param_offset = 0x90B000UL;
	pstrcpy_targphys("cmdline", param_offset, 0x400, kernel_cmdline);
    }

    return;
}

void sw64_load_initrd(const char *initrd_filename,
                      BOOT_PARAMS *sunway_boot_params)
{
    long initrd_size;

    initrd_size = get_image_size(initrd_filename);
    if (initrd_size < 0) {
	error_report("could not load initial ram disk '%s'",
		     initrd_filename);
	exit(1);
    }
    /* Put the initrd image as high in memory as possible. */
    load_image_targphys(initrd_filename, SW_INITRD_BASE, initrd_size);
    sunway_boot_params->initrd_start = SW_INITRD_BASE | 0xfff0000000000000UL;
    sunway_boot_params->initrd_size = initrd_size;

    return;
}

int sw64_load_dtb(MachineState *ms, BOOT_PARAMS *sunway_boot_params)
{
    int ret, fdt_size;
    hwaddr fdt_base;

    if (ms->kernel_filename) {
        /* For direct kernel boot, place the DTB after the initrd to avoid
	 * overlaying the loaded kernel.
	 */
        sunway_boot_params->dtb_start = (SW_INITRD_BASE | 0xfff0000000000000UL)
		                        + sunway_boot_params->initrd_size;
    } else {
        /* For BIOS boot, place the DTB at SW_FDT_BASE temporarily, the
	 * bootloader will move it to a more proper place later.
	 */
        sunway_boot_params->dtb_start = SW_FDT_BASE | 0xfff0000000000000UL;
    }

    if (!ms->fdt) {
        fprintf(stderr, "Board was unable to create a dtb blob\n");
        return -1;
    }

    ret = fdt_pack(ms->fdt);
    /* Should only fail if we've built a corrupted tree */
    g_assert(ret == 0);
    fdt_size = fdt_totalsize(ms->fdt);
    qemu_fdt_dumpdtb(ms->fdt, fdt_size);

    /* Put the DTB into the memory map as a ROM image: this will ensure
     * the DTB is copied again upon reset, even if addr points into RAM.
     */
    fdt_base = sunway_boot_params->dtb_start & (~0xfff0000000000000UL);

    dtb_start_c4 = sunway_boot_params->dtb_start;

    rom_add_blob_fixed("dtb", ms->fdt, fdt_size, fdt_base);

    return 0;
}

void sw64_board_alarm_timer(void *opaque)
{
    TimerState *ts = (TimerState *)((uintptr_t)opaque);

    if (!kvm_enabled()) {
        int cpu = ts->order;
	cpu_interrupt(qemu_get_cpu(cpu), CPU_INTERRUPT_TIMER);
    }

    return;
}

void sw64_create_alarm_timer(MachineState *ms, BoardState *bs)
{
    TimerState *ts;
    SW64CPU *cpu;
    int i;

    for (i = 0; i < ms->smp.cpus; ++i) {
	cpu = SW64_CPU(qemu_get_cpu(i));
        ts = g_new(TimerState, 1);
	ts->opaque = (void *) ((uintptr_t)bs);
	ts->order = i;
	cpu->alarm_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
			                &sw64_board_alarm_timer, ts);
    }
}

PCIINTxRoute sw64_route_intx_pin_to_irq(void *opaque, int pin)
{
    PCIINTxRoute route;

    route.mode = PCI_INTX_ENABLED;
    route.irq = SW_PIN_TO_IRQ;
    return route;
}

uint64_t convert_bit(int n)
{
    uint64_t ret;

    if (n == 64)
	ret = 0xffffffffffffffffUL;
    else
	ret = (1UL << n) - 1;

    return ret;
}

FWCfgState *sw64_create_fw_cfg(hwaddr addr, hwaddr size)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    uint16_t smp_cpus = ms->smp.cpus;
    FWCfgState *fw_cfg;

    fw_cfg = fw_cfg_init_mem_wide(addr + 8, addr, 8,
		                  addr + 16, &address_space_memory);
    fw_cfg_add_i16(fw_cfg, FW_CFG_NB_CPUS, smp_cpus);

    if (!ms->dtb) {
        char *nodename;

        nodename = g_strdup_printf("/soc/fw_cfg@8049");
        qemu_fdt_add_subnode(ms->fdt, nodename);
        qemu_fdt_setprop_string(ms->fdt, nodename,
                                "compatible", "qemu,fw-cfg-mmio");
        qemu_fdt_setprop(ms->fdt, nodename, "dma-coherent", NULL, 0);
        qemu_fdt_setprop_sized_cells(ms->fdt, nodename,
                                     "reg", 2, addr, 2, size);
        g_free(nodename);
    }

    return fw_cfg;
}

void sw64_virt_build_smbios(FWCfgState *fw_cfg)
{
    uint8_t *smbios_tables, *smbios_anchor;
    size_t smbios_tables_len, smbios_anchor_len;
    const char *product = "QEMU Virtual Machine";

    if (kvm_enabled())
	product = "KVM Virtual Machine";

    smbios_set_defaults("QEMU", product,
		        "sw64", false,
			true, SMBIOS_ENTRY_POINT_TYPE_64);

    smbios_get_tables(MACHINE(qdev_get_machine()), NULL, 0,
		      &smbios_tables, &smbios_tables_len,
		      &smbios_anchor, &smbios_anchor_len,
		      &error_fatal);

    if (smbios_anchor) {
	fw_cfg_add_file(fw_cfg, "etc/smbios/smbios-tables",
			smbios_tables, smbios_tables_len);
	fw_cfg_add_file(fw_cfg, "etc/smbios/smbios-anchor",
			smbios_anchor, smbios_anchor_len);
    }

    return;
}

void sw64_board_set_irq(void *opaque, int irq, int level)
{
    if (level == 0)
	return;

    if (kvm_enabled()) {
        kvm_set_irq(kvm_state, irq, level);
	return;
    }

    cpu_interrupt(qemu_get_cpu(0), CPU_INTERRUPT_PCIE);
}

int sw64_board_map_irq(PCIDevice *d, int irq_num)
{
    /* In fact,the return value is the interrupt type passed to kernel,
     * so it must keep same with the type in do_entInt in kernel.
     */
    return 16;
}

void serial_set_irq(void *opaque, int irq, int level)
{
    if (level == 0)
	return;

    if (kvm_enabled()) {
	kvm_set_irq(kvm_state, irq, level);
	return;
    }

    cpu_interrupt(qemu_get_cpu(0), CPU_INTERRUPT_HARD);
}

void sw64_new_cpu(const char *name, int64_t arch_id, Error **errp)
{
    Object *cpu = NULL;
    Error *local_err = NULL;

    cpu = object_new(name);
    object_property_set_uint(cpu, "core-id", arch_id, &local_err);
    object_property_set_bool(cpu, "realized", true, &local_err);

    object_unref(cpu);
    error_propagate(errp, local_err);
}

void sw64_create_pcie(BoardState *bs, PCIBus *b, PCIHostState *phb)
{
    int i;

    for (i = 0; i < nb_nics; i++) {
	pci_nic_init_nofail(&nd_table[i], b, "e1000", NULL);
    }

    pci_vga_init(b);

    bs->serial_irq = qemu_allocate_irq(serial_set_irq, bs, 12);
    if (serial_hd(0)) {
        serial_mm_init(get_system_memory(), 0x3F8 + 0x880100000000ULL, 0,
		       bs->serial_irq, (1843200 >> 4), serial_hd(0),
		       DEVICE_LITTLE_ENDIAN);
    }
}

void rtc_get_time(Object *obj, struct tm *current_tm, Error **errp)
{
    time_t guest_sec;
    int64_t guest_nsec;

    guest_nsec = get_clock_realtime();
    guest_sec = guest_nsec / NANOSECONDS_PER_SECOND;
    gmtime_r(&guest_sec, current_tm);
}
