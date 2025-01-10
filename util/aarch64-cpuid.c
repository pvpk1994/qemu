// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dealing with arm cpu identification information.
 *
 * Copyright (C) 2024 Phytium, Inc.
 *
 * Authors:
 *  Peng Meng Guang <pengmengguang@phytium.com.cn>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.1
 * or later.  See the COPYING.LIB file in the top-level directory.
 */

#include <math.h>
#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/aarch64-cpuid.h"

#if defined(__aarch64__)
uint64_t qemu_read_cpuid_id(void)
{
#ifdef CONFIG_LINUX
    const char *file = "/sys/devices/system/cpu/cpu0/regs/identification/midr_el1";
    char *buf;
    uint64_t midr = 0;

#define BUF_SIZE 32
    buf = g_malloc0(BUF_SIZE);
    if (!buf) {
        return 0;
    }

    if (!g_file_get_contents(file, &buf, 0, NULL)) {
        goto out;
    }

    if (qemu_strtoul(buf, NULL, 0, &midr) < 0) {
        goto out;
    }

out:
    g_free(buf);

    return midr;
#else
    return 0;
#endif
}

uint8_t qemu_read_cpuid_implementor(void)
{
#ifdef CONFIG_LINUX
    uint64_t aarch64_midr = qemu_read_cpuid_id();

    return MIDR_IMPLEMENTOR(aarch64_midr);
#else
    return 0;
#endif
}

uint16_t qemu_read_cpuid_part_number(void)
{
#ifdef CONFIG_LINUX
    uint64_t aarch64_midr = qemu_read_cpuid_id();

    return MIDR_PARTNUM(aarch64_midr);
#else
    return 0;
#endif
}

bool is_phytium_cpu(void)
{
    return qemu_read_cpuid_implementor() == ARM_CPU_IMP_PHYTIUM;
}

#endif
