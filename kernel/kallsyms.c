/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <kernel/kallsyms.h>
#include <string.h>

const char *kallsyms_lookup(unsigned long addr, unsigned long *symbolsize,
                           unsigned long *offset, char **modname, char *namebuf)
{
    unsigned int i;
    unsigned long best_addr = 0;
    const char *best_name = NULL;

    /* 简单的线性搜索，因为符号表是按地址排序的，也可以用二分查找 */
    for (i = 0; i < kallsyms_num; i++) {
        if (kallsyms_table[i].addr <= addr) {
            if (kallsyms_table[i].addr > best_addr) {
                best_addr = kallsyms_table[i].addr;
                best_name = kallsyms_table[i].name;
            }
        } else {
            /* 既然已排序，当前地址大于目标地址，后续肯定也大于，可提前结束 */
            break; 
        }
    }

    if (best_name) {
        if (offset) *offset = addr - best_addr;
        if (symbolsize) *symbolsize = 0; // 暂时不支持符号大小
        if (modname) *modname = NULL;
        return best_name;
    }
    
    return NULL;
}

void print_symbol(const char *fmt, unsigned long addr)
{
    const char *name;
    unsigned long offset;
    
    name = kallsyms_lookup(addr, NULL, &offset, NULL, NULL);
    
    if (name) {
        pr_info(fmt, addr, name, offset);
    } else {
        pr_info(fmt, addr, "(unknown)");
    }
}
