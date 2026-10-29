/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <stdlib.h>
typedef uint32_t u32;
typedef uintptr_t addr_t;
#define __init
#define CONFIG_MAX_ORDER 5
#define CONFIG_PAGE_SIZE 4096U
#define ALIGNED(a,b) (((a) + (b) - 1) & ~((addr_t)(b) - 1))
#define GET_PFN(a) ((a) / CONFIG_PAGE_SIZE)
#define container_of(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
#define BUG_ON(x) do { if (x) abort(); } while (0)
