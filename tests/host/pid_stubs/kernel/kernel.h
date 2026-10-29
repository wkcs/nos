/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
typedef uint32_t u32;
#define U32_MAX UINT32_MAX
#define GFP_KERNEL 0
#define BUG_ON(c) do { if (c) __builtin_trap(); } while (0)
