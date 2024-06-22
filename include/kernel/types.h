/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_TYPES_H__
#define __NOS_TYPES_H__

#include <stdint.h>
#include <stdbool.h>

#include <asm/types.h>

#undef NULL
#define NULL ((void *)0)

#ifndef bool
#define bool u8
#define true 1
#define false 0
#endif

typedef s8 __s8;
typedef u8 __u8;
typedef s16 __s16;
typedef u16 __u16;
typedef s32 __s32;
typedef u32 __u32;
typedef s64 __s64;
typedef u64 __u64;

#if !defined(_INT8_T_DECLARED) && !defined(_GCC_STDINT_H)
typedef s8 int8_t;
#define _INT8_T_DECLARED
#endif
#if !defined(_UINT8_T_DECLARED) && !defined(_GCC_STDINT_H)
typedef u8 uint8_t;
#define _UINT8_T_DECLARED
#endif
#if !defined(_INT16_T_DECLARED) && !defined(_GCC_STDINT_H)
typedef s16 int16_t;
#define _INT16_T_DECLARED
#endif
#if !defined(_UINT16_T_DECLARED) && !defined(_GCC_STDINT_H)
typedef u16 uint16_t;
#define _UINT16_T_DECLARED
#endif
#if !defined(_INT32_T_DECLARED) && !defined(_GCC_STDINT_H)
typedef s32 int32_t;
#define _INT32_T_DECLARED
#endif
#if !defined(_UINT32_T_DECLARED) && !defined(_GCC_STDINT_H)
typedef u32 uint32_t;
#define _UINT32_T_DECLARED
#endif
#if !defined(_INT64_T_DECLARED) && !defined(_GCC_STDINT_H)
typedef s64 int64_t;
#define _INT64_T_DECLARED
#endif
#if !defined(_UINT64_T_DECLARED) && !defined(_GCC_STDINT_H)
typedef u64 uint64_t;
#define _UINT64_T_DECLARED
#endif

#undef offsetof
#define offsetof(TYPE, MEMBER)	__builtin_offsetof(TYPE, MEMBER)

/**
 * sizeof_field() - Report the size of a struct field in bytes
 *
 * @TYPE: The structure containing the field of interest
 * @MEMBER: The field to return the size of
 */
#define sizeof_field(TYPE, MEMBER) sizeof((((TYPE *)0)->MEMBER))

/**
 * offsetofend() - Report the offset of a struct field within the struct
 *
 * @TYPE: The type of the structure
 * @MEMBER: The member within the structure to get the end offset of
 */
#define offsetofend(TYPE, MEMBER) \
    (offsetof(TYPE, MEMBER)	+ sizeof_field(TYPE, MEMBER))

/**
 * container_of - cast a member of a structure out to the containing structure
 * @ptr:	the pointer to the member.
 * @type:	the type of the container struct this is embedded in.
 * @member:	the name of the member within the struct.
 *
 */
#define container_of(ptr, type, member) ({				\
    const typeof( ((type *)0)->member ) *__mptr = (ptr);   \
    (type *)( (size_t)__mptr - offsetof(type, member) );})

#endif /* __NOS_TYPES_H__ */
