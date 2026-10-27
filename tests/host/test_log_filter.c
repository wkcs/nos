/**
 * Copyright (C) 2026 Nick Hu
 *
 * Compile-time filters must avoid evaluating disabled log arguments.
 */

#include <kernel/printk.h>
#include <stdarg.h>
#include <stdio.h>

static int calls;
static int side_effects;

int pr_log(bool no_tag, enum log_level level, const char *fmt, ...)
{
    (void)no_tag;
    (void)level;
    (void)fmt;
    calls++;
    return 0;
}

int main(void)
{
    pr_debug("%d", ++side_effects);
    pr_info("%d", ++side_effects);
    pr_warning("%d", ++side_effects);
    if (side_effects != 1 || calls != 1) {
        fputs("FAIL: compile-time filter evaluates or emits disabled logs\n", stderr);
        return 1;
    }
    puts("compile-time log filter checks passed");
    return 0;
}
