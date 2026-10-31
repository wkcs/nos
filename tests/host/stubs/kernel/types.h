/** Copyright (C) 2026 Nick Hu */
#ifndef NOS_TEST_TYPES_H
#define NOS_TEST_TYPES_H
#include <stdint.h>
#include <sys/types.h>
#ifndef _SSIZE_T_DECLARED
#define _SSIZE_T_DECLARED
#endif
#ifndef _MODE_T_DECLARED
#define _MODE_T_DECLARED
#endif
#ifndef _DEV_T_DECLARED
#define _DEV_T_DECLARED
#endif
typedef int8_t    s8;
typedef uint8_t   u8;
typedef int16_t   s16;
typedef uint16_t  u16;
typedef int32_t   s32;
typedef uint32_t  u32;
typedef int64_t   s64;
typedef uint64_t  u64;
typedef uintptr_t addr_t;
#endif
