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

__attribute__((weak)) const unsigned char kallsyms_token_table[256] = { 0 };

static void kallsyms_expand_token(unsigned char tok, char *out,
                                  size_t max_len, size_t *pos)
{
    unsigned char idx;
    unsigned char a;
    unsigned char b;

    if (tok < 0x80U) {
        if (*pos + 1U < max_len) {
            out[*pos] = (char)tok;
            (*pos)++;
        }
        return;
    }

    idx = (unsigned char)(tok - 0x80U);
    a = kallsyms_token_table[(size_t)idx * 2U];
    b = kallsyms_token_table[(size_t)idx * 2U + 1U];
    if (a == 0U && b == 0U) {
        return;
    }
    kallsyms_expand_token(a, out, max_len, pos);
    kallsyms_expand_token(b, out, max_len, pos);
}

static const char *kallsyms_decompress_name(const char *raw_name, char *namebuf)
{
    static char s_name_buf[64];
    char *out = (namebuf != NULL) ? namebuf : s_name_buf;
    const unsigned char *p = (const unsigned char *)raw_name;
    bool has_token = false;
    size_t pos = 0U;

    if (raw_name == NULL) {
        return NULL;
    }
    while (*p != 0U) {
        if (*p >= 0x80U) {
            has_token = true;
            break;
        }
        p++;
    }
    if (!has_token) {
        return raw_name;
    }

    p = (const unsigned char *)raw_name;
    while (*p != 0U && pos + 1U < sizeof(s_name_buf)) {
        kallsyms_expand_token(*p, out, sizeof(s_name_buf), &pos);
        p++;
    }
    out[pos] = '\0';
    return out;
}

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

    return kallsyms_decompress_name(kallsyms_table[best_idx].name, namebuf);
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
