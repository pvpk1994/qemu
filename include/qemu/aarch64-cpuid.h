// SPDX-License-Identifier: GPL-2.0-only
/*
 * aarch64-cpuid.h: Macros to identify the MIDR of aarch64.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef QEMU_AARCH64_CPUID_H
#define QEMU_AARCH64_CPUID_H

#define MIDR_REVISION_MASK  0xf
#define MIDR_REVISION(midr) ((midr) & MIDR_REVISION_MASK)
#define MIDR_PARTNUM_SHIFT  4
#define MIDR_PARTNUM_MASK   (0xfff << MIDR_PARTNUM_SHIFT)
#define MIDR_PARTNUM(midr)  \
    (((midr) & MIDR_PARTNUM_MASK) >> MIDR_PARTNUM_SHIFT)
#define MIDR_ARCHITECTURE_SHIFT 16
#define MIDR_ARCHITECTURE_MASK  (0xf << MIDR_ARCHITECTURE_SHIFT)
#define MIDR_ARCHITECTURE(midr) \
    (((midr) & MIDR_ARCHITECTURE_MASK) >> MIDR_ARCHITECTURE_SHIFT)
#define MIDR_VARIANT_SHIFT  20
#define MIDR_VARIANT_MASK   (0xf << MIDR_VARIANT_SHIFT)
#define MIDR_VARIANT(midr)  \
    (((midr) & MIDR_VARIANT_MASK) >> MIDR_VARIANT_SHIFT)
#define MIDR_IMPLEMENTOR_SHIFT  24
#define MIDR_IMPLEMENTOR_MASK   (0xffU << MIDR_IMPLEMENTOR_SHIFT)
#define MIDR_IMPLEMENTOR(midr)  \
    (((midr) & MIDR_IMPLEMENTOR_MASK) >> MIDR_IMPLEMENTOR_SHIFT)
#define MIDR_CPU_MODEL(imp, partnum) \
    (((imp)            << MIDR_IMPLEMENTOR_SHIFT) | \
    (0xf            << MIDR_ARCHITECTURE_SHIFT) | \
    ((partnum)      << MIDR_PARTNUM_SHIFT))

#define MIDR_CPU_VAR_REV(var, rev) \
    (((var) << MIDR_VARIANT_SHIFT) | (rev))

#define MIDR_CPU_MODEL_MASK (MIDR_IMPLEMENTOR_MASK | MIDR_PARTNUM_MASK | \
                MIDR_ARCHITECTURE_MASK)

#define ARM_CPU_IMP_PHYTIUM             0x70
#define PHYTIUM_CPU_PART_FTC662         0x662
#define PHYTIUM_CPU_PART_FTC663         0x663
#define PHYTIUM_CPU_PART_FTC862         0x862

uint64_t qemu_read_cpuid_id(void);
uint8_t qemu_read_cpuid_implementor(void);
uint16_t qemu_read_cpuid_part_number(void);
bool is_phytium_cpu(void);

#endif
