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
#include <stdbool.h>

#define NOS_CONSOLE_PANIC_API 1

struct console_ops {
    int (* init)(void);
    int (* write)(const char buf[], int len);
    int (* write_record)(const char buf[], int len);
    int (* write_recordv)(const char *first, unsigned int first_len,
                          const char *second, unsigned int second_len);
    int (* write_room)(void);
    int (* getc)(void);
    int (* putc)(char c);
    void (* send_log)(void);
    int (* panic_begin)(void);
    int (* panic_write)(const char buf[], int len);
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
int console_write_record(const char buf[], int len);
int console_write_recordv(const char *first, unsigned int first_len,
                          const char *second, unsigned int second_len);
int console_write_room(void);
int console_panic_begin(void);
bool console_is_panic(void);
void console_send_log(void);
int console_getc(void);
int console_putc(char c);

#endif /* __NOS_CONSOLE_H__ */
