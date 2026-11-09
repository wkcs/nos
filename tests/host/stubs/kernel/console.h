/**
 * Copyright (C) 2026 Nick Hu
 *
 * Console hooks are provided by each host test.
 */

#ifndef NOS_TEST_CONSOLE_H
#define NOS_TEST_CONSOLE_H

int console_write(const char *buf, int len);
int console_write_room(void);
bool console_is_panic(void);
void console_send_log(void);

#endif
