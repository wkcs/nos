/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#pragma once
#include <stdlib.h>
#include <kernel/kernel.h>
#define pid_t uint32_t
pid_t pid_alloc(void);
int pid_free(pid_t pid);
void pid_init(void);
