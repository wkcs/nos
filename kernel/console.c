/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/console.h>
#include <kernel/kernel.h>
#include <kernel/errno.h>
#include <string.h>

static struct console *g_console;

extern addr_t __console_data_start;
extern addr_t __console_data_end;

static u32 get_console_num(void)
{
    addr_t addr_size = (addr_t)&__console_data_end - (addr_t)&__console_data_start;

    if (addr_size == 0) {
        return 0;
    }
    if (addr_size % sizeof(struct console) != 0) {
        return 0;
    }

    return (addr_size / sizeof(struct console));
}

static struct console *find_first_console(void)
{
    addr_t start_addr = (addr_t)&__console_data_start;
    return (struct console *)start_addr;
}

static struct console *find_console(void)
{
    u32 num;
    struct console *console;
#ifdef CONFIG_DEFAULT_CONSOLE
    u32 i;
    struct console *def;
#endif

    num = get_console_num();
    if (num == 0) {
        return NULL;
    }

    console = find_first_console();
#ifdef CONFIG_DEFAULT_CONSOLE
    def = console;

    for(i = 0; i < num; i++) {
        if (strcmp(CONFIG_DEFAULT_CONSOLE, console->name) == 0) {
            return console;
        }
        console++;
    }
    /* not find default console, use first console */
    return def;
#else
    return console;
#endif
}

int console_init(void)
{
    int rc;
    struct console *console;

    console = find_console();
    if (console == NULL) {
        return -ENODEV;
    }

    rc = console->ops->init();
    if (rc < 0) {
        return rc;
    }
    g_console = console;

    return 0;
}

int console_write(const char buf[], int len)
{
    if (g_console == NULL) {
        return -ENODEV;
    }
    if (buf == NULL) {
        return -EINVAL;
    }
    if (len < 0) {
        return -EINVAL;
    }

    return g_console->ops->write(buf, len);
}

void console_send_log(void)
{
    if (g_console == NULL) {
        return;
    }
    return g_console->ops->send_log();
}

char console_getc(void)
{
    if (g_console == NULL) {
        return 0;
    }
    return g_console->ops->getc();
}

int console_putc(char c)
{
    if (g_console == NULL) {
        return -ENODEV;
    }
    return g_console->ops->putc(c);
}
