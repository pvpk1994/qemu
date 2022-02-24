sw64_ss = ss.source_set()

sw64_ss.add(files('sunway.c'), fdt)
sw64_ss.add(when: 'CONFIG_SW64_VT_IOMMU', if_true: files('sw64_iommu.c'))
sw64_ss.add(when: 'CONFIG_ACPI', if_true: files('sw64-acpi-build.c'))

sw64_ss.add(when: 'CONFIG_CORE3', if_true: files(
  'core3.c',
  'core3_board.c',
))

sw64_ss.add(when: 'CONFIG_CORE4', if_true: files(
  'core4.c',
  'core4_board.c',
))

hw_arch += {'sw64': sw64_ss}
