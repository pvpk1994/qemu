#ifndef SW64_SUNWAY_H
#define SW64_SUNWAY_H

#include "exec/cpu-defs.h"
#include "hw/pci/pci.h"
#include "hw/loader.h"
#include "hw/sw64/core.h"

extern const MemoryRegionOps rtc_ops;
extern const MemoryRegionOps sw64_pci_ignore_ops;
extern const MemoryRegionOps sw64_pci_config_ops;
extern const MemoryRegionOps msi_ops;

void sw64_init_rtc_base_info(void);

uint64_t cpu_sw64_virt_to_phys(void *opaque, uint64_t addr);
CpuInstanceProperties sw64_cpu_index_to_props(MachineState *ms,
		                              unsigned cpu_index);
int64_t sw64_get_default_cpu_node_id(const MachineState *ms,
		                     int idx);
const CPUArchIdList *sw64_possible_cpu_arch_ids(MachineState *ms);
void sw64_cpu_reset(void *opaque);
void sw64_board_reset(MachineState *state, ShutdownCause reason);

void sw64_set_clocksource(void);
void sw64_set_ram_size(ram_addr_t ram_size);
void sw64_clear_uefi_bios(void);
void sw64_clear_smp_rcb(void);
void sw64_clear_kernel_print(void);
void sw64_load_hmcode(const char *hmcode_filename, uint64_t *hmcode_entry);
void sw64_find_and_load_bios(const char *bios_name);
void sw64_load_kernel(const char *kernel_filename, uint64_t *kernel_entry,
                      const char *kernel_cmdline);
void sw64_load_initrd(const char *initrd_filename,
                      BOOT_PARAMS *sunway_boot_params);
int sw64_load_dtb(MachineState *ms, BOOT_PARAMS *sunway_boot_params);
void sw64_board_alarm_timer(void *opaque);
void sw64_create_alarm_timer(MachineState *ms, BoardState *bs);
uint64_t convert_bit(int n);
FWCfgState *sw64_create_fw_cfg(hwaddr addr, hwaddr size);
void sw64_virt_build_smbios(FWCfgState *fw_cfg);
void sw64_board_set_irq(void *opaque, int irq, int level);
int sw64_board_map_irq(PCIDevice *d, int irq_num);
void serial_set_irq(void *opaque, int irq, int level);
void sw64_new_cpu(const char *name, int64_t arch_id, Error **errp);
void sw64_create_pcie(BoardState *bs, PCIBus *b, PCIHostState *phb);
PCIINTxRoute sw64_route_intx_pin_to_irq(void *opaque, int pin);
MemTxResult msi_write(void *opaque, hwaddr addr, uint64_t value,
                      unsigned size, MemTxAttrs attrs);
void rtc_get_time(Object *obj, struct tm *current_tm, Error **errp);
#endif /* SW64_SUNWAY_H */
