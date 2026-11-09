#ifndef TEST_KERNEL_H
#define TEST_KERNEL_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
typedef uintptr_t addr_t;
typedef uint32_t u32;
extern bool kernel_running;
#define GFP_KERNEL 0
#define pr_err(...) do { } while (0)
#define pr_info(...) do { } while (0)
#endif
