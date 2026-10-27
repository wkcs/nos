/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS UAPI fixed-width types and object handle definitions.
 * Layer 1 header: depends only on compiler freestanding headers.
 */

#ifndef __NOS_UAPI_TYPES_H__
#define __NOS_UAPI_TYPES_H__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef uint32_t nos_handle_t;
typedef uint32_t nos_cap_rights_t;
typedef int32_t  nos_pid_t;
typedef int32_t  nos_tid_t;
typedef uint64_t nos_tick_t;

#define NOS_HANDLE_INVALID       ((nos_handle_t)0U)
#define NOS_HANDLE_INDEX_MASK    0x0000FFFFU
#define NOS_HANDLE_GEN_SHIFT     16U

static inline uint16_t nos_handle_index(nos_handle_t h)
{
    return (uint16_t)(h & NOS_HANDLE_INDEX_MASK);
}

static inline uint16_t nos_handle_gen(nos_handle_t h)
{
    return (uint16_t)(h >> NOS_HANDLE_GEN_SHIFT);
}

static inline nos_handle_t nos_handle_make(uint16_t index, uint16_t gen)
{
    return ((uint32_t)gen << NOS_HANDLE_GEN_SHIFT) | (uint32_t)index;
}

#endif /* __NOS_UAPI_TYPES_H__ */
