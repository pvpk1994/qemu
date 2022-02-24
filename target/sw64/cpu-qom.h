/*
 * QEMU SW64 CPU
 *
 * Copyright (c) 2018 Lin Hainan
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#ifndef QEMU_SW64_CPU_QOM_H
#define QEMU_SW64_CPU_QOM_H

#include "hw/core/cpu.h"

#define TYPE_SW64_CPU "sw64-cpu"

OBJECT_DECLARE_CPU_TYPE(SW64CPU, SW64CPUClass, SW64_CPU)

#define SW64_CPU_TYPE_SUFFIX "-" TYPE_SW64_CPU
#define SW64_CPU_TYPE_NAME(model) model SW64_CPU_TYPE_SUFFIX

#endif
