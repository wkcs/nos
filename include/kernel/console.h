/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_CONSOLE_H__
#define __NOS_CONSOLE_H__

#include <kernel/section.h>
#include <kernel/types.h>

struct console_ops {
    int (* init)(void);
    int (* write)(const char buf[], int len);
    char (* getc)(void);
    int (* putc)(char c);
    void (* send_log)(void);
};

struct console {
    const char * name;
    struct console_ops *ops;
};

#define console_register(__name, __ops) \
__console struct console console_##__name = { \
    .name = #__name, \
    .ops = __ops, \
}

int console_init(void);
int console_write(const char buf[], int len);
void console_send_log(void);
char console_getc(void);
int console_putc(char c);

#endif /* __NOS_CONSOLE_H__ */
