/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS 内核隔离式 freestanding <inttypes.h> 头文件（仅无工具链 libc 时启用）。
 */

#ifndef __NOS_FREESTANDING_INTTYPES_H__
#define __NOS_FREESTANDING_INTTYPES_H__

#include <stdint.h>

#if defined(__LP64__) || defined(_LP64)
#define __NOS_PRI64_PREFIX "l"
#define __NOS_PRIPTR_PREFIX "l"
#else
#define __NOS_PRI64_PREFIX "ll"
#define __NOS_PRIPTR_PREFIX ""
#endif

#define PRId8   "d"
#define PRIi8   "i"
#define PRIu8   "u"
#define PRIx8   "x"
#define PRIX8   "X"

#define PRId16  "d"
#define PRIi16  "i"
#define PRIu16  "u"
#define PRIx16  "x"
#define PRIX16  "X"

#define PRId32  "d"
#define PRIi32  "i"
#define PRIu32  "u"
#define PRIx32  "x"
#define PRIX32  "X"

#define PRId64  __NOS_PRI64_PREFIX "d"
#define PRIi64  __NOS_PRI64_PREFIX "i"
#define PRIo64  __NOS_PRI64_PREFIX "o"
#define PRIu64  __NOS_PRI64_PREFIX "u"
#define PRIx64  __NOS_PRI64_PREFIX "x"
#define PRIX64  __NOS_PRI64_PREFIX "X"

#define PRIdPTR __NOS_PRIPTR_PREFIX "d"
#define PRIiPTR __NOS_PRIPTR_PREFIX "i"
#define PRIuPTR __NOS_PRIPTR_PREFIX "u"
#define PRIxPTR __NOS_PRIPTR_PREFIX "x"
#define PRIXPTR __NOS_PRIPTR_PREFIX "X"

#endif /* __NOS_FREESTANDING_INTTYPES_H__ */
