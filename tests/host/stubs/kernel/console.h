/**
 * Copyright (C) 2026 Nick Hu
 *
 * Console hooks are provided by each host test.
 */

#ifndef NOS_TEST_CONSOLE_H
#define NOS_TEST_CONSOLE_H

int console_write(const char *buf, int len);
int console_write_record(const char *buf, int len);
int console_write_recordv(const char *first, unsigned int first_len,
                          const char *second, unsigned int second_len);
int console_write_room(void);
bool console_is_panic(void);
void console_send_log(void);

#endif
