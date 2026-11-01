/**
 * Copyright (C) 2023-2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <kernel/kallsyms.h>
#include <lib/vsprintf.h>
#include <string.h>

const char *kallsyms_lookup(unsigned long addr, unsigned long *symbolsize,
                            unsigned long *offset, char **modname, char *namebuf)
{
    unsigned long clean_addr = addr;
    unsigned int low;
    unsigned int high;
    unsigned int best_idx;
    unsigned long best_addr;
    unsigned long size;
    unsigned long off;

    (void)namebuf;

    if (kallsyms_num == 0U) {
        return NULL;
    }

    if ((kallsyms_table[0].addr & 1UL) == 0UL) {
        clean_addr = addr & ~1UL;
    }

    if (clean_addr < kallsyms_table[0].addr) {
        return NULL;
    }

    low = 0U;
    high = kallsyms_num;
    while (low < high) {
        unsigned int mid = low + ((high - low) >> 1);
        if (kallsyms_table[mid].addr <= clean_addr) {
            low = mid + 1U;
        } else {
            high = mid;
        }
    }

    if (low == 0U) {
        return NULL;
    }

    best_idx = low - 1U;
    best_addr = kallsyms_table[best_idx].addr;
    off = clean_addr - best_addr;

    if (best_idx + 1U < kallsyms_num) {
        size = kallsyms_table[best_idx + 1U].addr - best_addr;
    } else {
        size = 0UL;
    }

    if ((size == 0UL && off > 0x4000UL) || (size > 0UL && off >= size)) {
        return NULL;
    }

    if (offset != NULL) {
        *offset = off;
    }
    if (symbolsize != NULL) {
        *symbolsize = size;
    }
    if (modname != NULL) {
        *modname = NULL;
    }

    return kallsyms_table[best_idx].name;
}

void print_symbol(const char *fmt, unsigned long addr)
{
    char symbuf[64];
    const char *name;
    unsigned long offset = 0UL;
    unsigned long size = 0UL;

    name = kallsyms_lookup(addr, &size, &offset, NULL, NULL);
    if (name != NULL) {
        if (size > 0UL) {
            snprintf(symbuf, sizeof(symbuf), "%s+0x%lx/0x%lx", name, offset, size);
        } else {
            snprintf(symbuf, sizeof(symbuf), "%s+0x%lx", name, offset);
        }
        pr_info(fmt, addr, symbuf);
    } else {
        pr_info(fmt, addr, "(unknown)");
    }
}
