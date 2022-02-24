/*
 * arch/sw64/include/asm/processor.h:
 *
 * TASK_UNMAPPED_BASE           TASK_SIZE / 2
 * TASK_SIZE                    0x40000000000UL
 */
#define TASK_UNMAPPED_BASE      0x20000000000ull

/* arch/sw64/include/asm/elf.h */
#define ELF_ET_DYN_BASE         (TASK_UNMAPPED_BASE + 0x1000000)

#include "../generic/target_mman.h"
