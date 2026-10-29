/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
typedef uint32_t u32;
typedef uintptr_t addr_t;
typedef uint32_t nos_test_pid_t;
#define pid_t nos_test_pid_t
#define CONFIG_PAGE_SIZE 4096U
#define CONFIG_MAX_ORDER 9
#define CONFIG_MM_DEBUG 1
#define U32_MAX UINT32_MAX
#define MEM_BASE_MAGIC 0x19950304U
#define BUG_ON(x) do { if (x) abort(); } while (0)
