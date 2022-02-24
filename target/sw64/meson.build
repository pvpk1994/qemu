sw64_ss = ss.source_set()
sw64_ss.add(files(
  'cpu.c',
  'exception.c',
  'float_helper.c',
  'helper.c',
  'gdbstub.c',
  'int_helper.c',
  'profile.c',
  'simd_helper.c',
  'translate.c',
))

sw64_ss.add(when: 'CONFIG_KVM', if_true: files('kvm.c'))

sw64_system_ss = ss.source_set()
sw64_system_ss.add(files('machine.c'))

target_arch += {'sw64': sw64_ss}
target_system_arch += {'sw64': sw64_system_ss}
