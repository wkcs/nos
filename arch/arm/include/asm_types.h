/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __ARM_ASM_TYPES_H__
#define __ARM_ASM_TYPES_H__

typedef signed char __wk_s8_t;
typedef unsigned char __wk_u8_t;

typedef signed short __wk_s16_t;
typedef unsigned short __wk_u16_t;

typedef signed int __wk_s32_t;
typedef unsigned int __wk_u32_t;

typedef signed long long __wk_s64_t;
typedef unsigned long long __wk_u64_t;

typedef unsigned int __wk_size_t;

typedef __wk_u32_t __dma_addr_t;
typedef __wk_u32_t __dma64_addr_t;

typedef __wk_u32_t __addr_t;

#endif