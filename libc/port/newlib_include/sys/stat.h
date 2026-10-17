/* Copyright (C) 2026 NOS contributors */
#ifndef NOS_NEWLIB_SYS_STAT_H
#define NOS_NEWLIB_SYS_STAT_H
#include_next <sys/stat.h>
/* The generic bare-metal Newlib header hides lstat behind unrelated target
 * names. NOS supplies this OS hook with the unmodified Newlib stat layout. */
#ifdef __cplusplus
extern "C" {
#endif
int lstat(const char *__restrict, struct stat *__restrict);
#ifdef __cplusplus
}
#endif
#endif
