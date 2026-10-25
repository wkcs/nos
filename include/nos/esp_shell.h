/* Copyright (C) 2026 NOS contributors. ESP-IDF shared-firmware nshell profile. */
#ifndef NOS_ESP_SHELL_H
#define NOS_ESP_SHELL_H
#include <stddef.h>
int nos_esp_shell_files(int count, char **words,
                        void (*emit)(void *, const char *, size_t), void *context);
void nos_idf_shell_run(void) __attribute__((noreturn));
#endif
