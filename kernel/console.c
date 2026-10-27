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
#include <limits.h>
#include <string.h>

static struct console *g_console;
static bool g_console_panic;

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
    if (console->ops == NULL || console->ops->init == NULL) {
        return -ENOSYS;
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

    if (g_console->ops == NULL || g_console->ops->write == NULL) {
        return -ENOSYS;
    }
    if (console_is_panic() && g_console->ops->panic_write != NULL) {
        return g_console->ops->panic_write(buf, len);
    }
    return g_console->ops->write(buf, len);
}

int console_write_record(const char buf[], int len)
{
    if (g_console == NULL) {
        return -ENODEV;
    }
    if (buf == NULL || len < 0) {
        return -EINVAL;
    }
    if (g_console->ops == NULL) {
        return -ENOSYS;
    }
    if (console_is_panic()) {
        return console_write(buf, len);
    }
    if (g_console->ops->write_record != NULL) {
        return g_console->ops->write_record(buf, len);
    }
    return -ENOSYS;
}

int console_write_room(void)
{
    if (g_console == NULL) {
        return -ENODEV;
    }
    if (g_console->ops == NULL || g_console->ops->write_room == NULL) {
        return -ENOSYS;
    }
    return g_console->ops->write_room();
}

int console_write_recordv(const char *first, unsigned int first_len,
                          const char *second, unsigned int second_len)
{
    if (g_console == NULL) {
        return -ENODEV;
    }
    if ((first == NULL && first_len != 0) || (second == NULL && second_len != 0) ||
        first_len > UINT_MAX - second_len || first_len + second_len > INT_MAX) {
        return -EINVAL;
    }
    if (g_console->ops == NULL || g_console->ops->write_recordv == NULL) {
        return -ENOSYS;
    }
    return g_console->ops->write_recordv(first, first_len, second, second_len);
}

int console_panic_begin(void)
{
    if (g_console == NULL) {
        return -ENODEV;
    }
    __atomic_store_n(&g_console_panic, true, __ATOMIC_RELAXED);
    if (g_console->ops == NULL || g_console->ops->panic_begin == NULL) {
        return -ENOSYS;
    }
    return g_console->ops->panic_begin();
}

bool console_is_panic(void)
{
    return __atomic_load_n(&g_console_panic, __ATOMIC_RELAXED);
}

void console_send_log(void)
{
    if (g_console == NULL || g_console->ops == NULL ||
        g_console->ops->send_log == NULL) {
        return;
    }
    g_console->ops->send_log();
}

int console_getc(void)
{
    if (g_console == NULL) {
        return -ENODEV;
    }
    if (g_console->ops == NULL || g_console->ops->getc == NULL) {
        return -ENOSYS;
    }
    return g_console->ops->getc();
}

int console_putc(char c)
{
    if (g_console == NULL) {
        return -ENODEV;
    }
    if (g_console->ops == NULL || g_console->ops->putc == NULL) {
        return -ENOSYS;
    }
    return g_console->ops->putc(c);
}
